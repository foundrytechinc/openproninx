// MAPPED FILES AND THE SWAP
#include "buf.h"
#include "blockdev.h"
#include "hibernate.h"
#include "defs.h"
#include "file.h"
#include "inc/abi.h"
#include "inc/stat.h"
#include "malloc.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "sleeplock.h"
#include "mutex.h"
#include "uvm.h"
#include "x86.h"

static struct {
  struct mutex lock;
  struct vm_object *objects;
} uvm;

static void swapinit(void);

static void pageout_tune(void);

void uvminit(void) {
  mtx_init(&uvm.lock, "uvm");
  swapinit();
  pageout_tune();
}

static void tlb_flush(struct process *pr) {
  if (curproc != 0 && curproc->p_p == pr)
    lcr3(V2P(pr->pgdir));
  if (pr->ps_nthreads > 1)
    tlb_shootdown();
}

// THE LOCK
static void vm_lock(struct process *pr) { acquiresleep(&pr->vmlock); }

static void vm_unlock(struct process *pr) { releasesleep(&pr->vmlock); }

static void vm_account(struct process *pr) {
  struct vm_entry *e;
  uint64_t n = 0;

  for (e = pr->vm; e != 0; e = e->next)
    n += e->end - e->start;
  pr->vmsize = n;
}

static uint64_t anon_size(struct process *pr) {
  struct vm_entry *e;
  uint64_t n = 0;

  for (e = pr->vm; e != 0; e = e->next)
    if (e->obj == 0 && !e->shared)
      n += e->end - e->start;
  return n;
}

// ENTRIES
static struct vm_entry *entry_alloc(void) {
  return malloc(sizeof(struct vm_entry), M_VM, M_WAITOK | M_ZERO);
}

static void entry_free(struct vm_entry *e) { free(e, M_VM); }

static struct vm_entry *entry_find(struct proc *p, uintptr_t va) {
  struct vm_entry *e;

  for (e = p->p_p->vm; e != 0; e = e->next)
    if (va >= e->start && va < e->end)
      return e;
  return 0;
}

int vm_holds(struct proc *p, uintptr_t va) {
  int held;

  vm_lock(p->p_p);
  held = entry_find(p, va) != 0;
  vm_unlock(p->p_p);
  return held;
}

static int range_clear(struct proc *p, uintptr_t start, uintptr_t end) {
  struct vm_entry *e;

  for (e = p->p_p->vm; e != 0; e = e->next)
    if (start < e->end && e->start < end)
      return 0;
  return hat_range_empty(p->p_p->pgdir, start, end - start);
}

static void munmap_locked(struct proc *p, uintptr_t addr, size_t len);

// FNU_MEM_FIXED
static int fixed_clear(struct proc *p, uintptr_t start, size_t len, int flags) {
  if (range_clear(p, start, start + len))
    return 0;
  if (flags & FNU_MEM_NOREPLACE)
    return FNU_ERR_NOMEMORY;
  munmap_locked(p, start, len);
  p->p_p->rss -= hat_unload(p->p_p->pgdir, start, len);
  tlb_flush(p->p_p);
  return range_clear(p, start, start + len) ? 0 : FNU_ERR_NOMEMORY;
}

// OBJECTS
static struct vm_object *object_get(struct inode *ip) {
  struct vm_object *o, *fresh = malloc(sizeof(*fresh), M_VM, M_WAITOK | M_ZERO);

  fresh->ip = ip;
  mtx_enter(&uvm.lock);
  for (o = ip != 0 ? uvm.objects : 0; o != 0; o = o->link)
    if (o->ip == ip) {
      o->refs++;
      mtx_leave(&uvm.lock);
      free(fresh, M_VM);
      return o;
    }
  fresh->refs = 1;
  fresh->link = uvm.objects;
  uvm.objects = fresh;
  mtx_leave(&uvm.lock);
  if (ip != 0)
    idup(ip);
  return fresh;
}

// lock held
static int object_lock(struct vm_object *o) {
  if (holdingsleep(&o->ip->lock))
    return 0;
  ilock(o->ip);
  return 1;
}

static void page_writeback(struct vm_object *o, struct vm_page *pg) {
  struct inode *ip = o->ip;
  uint n = PGSIZE;
  int locked;

  if (!pg->dirty || ip == 0)
    return;
  pg->dirty = 0;
  locked = object_lock(o);
  if (pg->offset >= ip->size)
    n = 0;
  else if (pg->offset + n > ip->size)
    n = (uint)(ip->size - pg->offset);
  if (n > 0)
    writei(ip, pg->addr, pg->offset, n);
  if (locked)
    iunlock(ip);
}

static void page_release(struct vm_object *o, struct vm_page *pg) {
  struct vm_page **pp;

  mtx_enter(&uvm.lock);
  if (--pg->refs > 0) {
    mtx_leave(&uvm.lock);
    return;
  }
  if (pg->dirty) {
    pg->refs++;
    mtx_leave(&uvm.lock);
    page_writeback(o, pg);
    mtx_enter(&uvm.lock);
    if (--pg->refs > 0) {
      mtx_leave(&uvm.lock);
      return;
    }
  }
  for (pp = &o->pages; *pp != 0; pp = &(*pp)->next)
    if (*pp == pg) {
      *pp = pg->next;
      break;
    }
  mtx_leave(&uvm.lock);
  page_free(pg->addr);
  free(pg, M_VM);
}

static void object_put(struct vm_object *o) {
  struct vm_object **pp;
  struct inode *ip;

  if (o == 0)
    return;
  mtx_enter(&uvm.lock);
  if (--o->refs > 0) {
    mtx_leave(&uvm.lock);
    return;
  }
  for (pp = &uvm.objects; *pp != 0; pp = &(*pp)->link)
    if (*pp == o) {
      *pp = o->link;
      break;
    }
  mtx_leave(&uvm.lock);
  ip = o->ip;
  free(o, M_VM);
  if (ip != 0)
    iput(ip);
}

static int writes_object(struct vm_entry *e) {
  return e->obj != 0 && e->shared && (e->prot & FNU_MEM_WRITE);
}

static void entry_ref(struct vm_entry *e) {
  if (e->obj == 0)
    return;
  mtx_enter(&uvm.lock);
  e->obj->refs++;
  if (writes_object(e))
    e->obj->wrefs++;
  if (e->text)
    e->obj->text++;
  mtx_leave(&uvm.lock);
}

static void entry_unref(struct vm_entry *e) {
  if (e->obj == 0)
    return;
  mtx_enter(&uvm.lock);
  if (writes_object(e))
    e->obj->wrefs--;
  if (e->text)
    e->obj->text--;
  mtx_leave(&uvm.lock);
  object_put(e->obj);
}

static struct vm_page *page_find(struct vm_object *o, uint64_t offset) {
  struct vm_page *pg;

  for (pg = o->pages; pg != 0; pg = pg->next)
    if (pg->offset == offset)
      return pg;
  return 0;
}

// the page at offset, or 0 and why in *err
static struct vm_page *object_page(struct vm_object *o, uint64_t offset,
                                   int *err) {
  struct vm_page *pg, *had;
  char *mem;
  int n = 0, locked;

  mtx_enter(&uvm.lock);
  if ((pg = page_find(o, offset)) != 0) {
    pg->refs++;
    mtx_leave(&uvm.lock);
    return pg;
  }
  mtx_leave(&uvm.lock);
  if ((mem = page_alloc_user()) == 0) {
    *err = FNU_ERR_NOMEMORY;
    return 0;
  }
  memset(mem, 0, PGSIZE);
  if (o->ip != 0) {
    locked = object_lock(o);
    n = offset < o->ip->size ? readi(o->ip, mem, (uint)offset, PGSIZE) : 0;
    if (locked)
      iunlock(o->ip);
  }
  if (n < 0) {
    page_free(mem);
    *err = FNU_ERR_IO;
    return 0;
  }
  pg = malloc(sizeof(*pg), M_VM, M_WAITOK | M_ZERO);
  pg->obj = o;
  pg->offset = offset;
  pg->addr = mem;
  pg->refs = 1;
  mtx_enter(&uvm.lock);
  if ((had = page_find(o, offset)) != 0) {
    had->refs++;
    mtx_leave(&uvm.lock);
    page_free(mem);
    free(pg, M_VM);
    return had;
  }
  pg->next = o->pages;
  o->pages = pg;
  mtx_leave(&uvm.lock);
  return pg;
}

static struct vm_page *page_of_addr(struct vm_object *o, char *addr) {
  struct vm_page *pg;

  mtx_enter(&uvm.lock);
  for (pg = o->pages; pg != 0; pg = pg->next)
    if (pg->addr == addr)
      break;
  mtx_leave(&uvm.lock);
  return pg;
}

// PAGES OUT OF THE TABLE
#define REAP_BATCH 32

struct reap {
  struct process *pr;
  int n;
  struct {
    char *addr;
    struct vm_object *obj;
    struct vm_page *pg;
  } item[REAP_BATCH];
};

static void reap_flush(struct reap *r) {
  int i;

  if (r->n == 0)
    return;
  tlb_flush(r->pr);
  for (i = 0; i < r->n; i++) {
    if (r->item[i].pg != 0)
      page_release(r->item[i].obj, r->item[i].pg);
    else
      page_free(r->item[i].addr);
  }
  r->n = 0;
}

static void reap_add(struct reap *r, char *addr, struct vm_object *obj,
                     struct vm_page *pg) {
  if (r->n == REAP_BATCH)
    reap_flush(r);
  r->item[r->n].addr = addr;
  r->item[r->n].obj = obj;
  r->item[r->n].pg = pg;
  r->n++;
}

// UNMAPPING ONE PAGE
static void unmap_page(struct proc *p, struct vm_entry *e, uintptr_t va,
                       struct reap *r) {
  pte_t *pte = hat_pte(p->p_p->pgdir, (void *)va, 0);
  struct vm_page *pg;
  char *addr;

  if (pte == 0)
    return;
  if (*pte & PTE_SWAPPED) {
    swap_free_slot(*pte);
    *pte = 0;
    return;
  }
  if (!(*pte & PTE_P))
    return;
  addr = P2V(PTE_ADDR(*pte));
  p->p_p->rss--;
  if (*pte & PTE_CACHE) {
    pg = page_of_addr(e->obj, addr);
    if (pg != 0 && (*pte & PTE_D) && e->shared && (*pte & PTE_W))
      pg->dirty = 1;
    *pte = 0;
    if (pg != 0)
      reap_add(r, addr, e->obj, pg);
    return;
  }
  *pte = 0;
  reap_add(r, addr, 0, 0);
}

// THE FAULT
// 0, or why not
static int fault_in_entry(struct proc *p, struct vm_entry *e, uintptr_t va,
                          int write, int force, int nofault) {
  uintptr_t page = PGROUNDDOWN(va);
  pte_t *pte;
  struct vm_page *pg;
  char *mem;
  pte_t perm = e->prot != 0 ? PTE_U : 0;
  pte_t nx = (e->prot & FNU_MEM_EXEC) ? 0 : vm_nx();
  pte_t w = (e->prot & FNU_MEM_WRITE) ? PTE_W : 0;
  int err = 0;

  if (force && e->shared && !w)
    return FNU_ERR_DENIED;
  if (write && !w && !force)
    return FNU_ERR_DENIED;
  if (!write && !(e->prot & (FNU_MEM_READ | FNU_MEM_EXEC)))
    return FNU_ERR_DENIED;

  pte = hat_pte(p->p_p->pgdir, (void *)page, WALK_NOSLEEP);
  if (pte == 0)
    return FNU_ERR_NOMEMORY;
  if ((*pte & PTE_P) && !(*pte & PTE_CACHE)) {
    if (write && w && !(*pte & PTE_W)) {
      *pte |= PTE_W;
      tlb_flush(p->p_p);
    }
    return 0;
  }
  if ((*pte & PTE_P) && (*pte & PTE_CACHE)) {
    if (!write)
      return 0;
    if (e->shared) {
      *pte |= PTE_W;
      pg = page_of_addr(e->obj, P2V(PTE_ADDR(*pte)));
      if (pg != 0)
        pg->dirty = 1;
      tlb_flush(p->p_p);
      return 0;
    }
    if ((mem = page_alloc_user()) == 0)
      return FNU_ERR_NOMEMORY;
    memmove(mem, P2V(PTE_ADDR(*pte)), PGSIZE);
    pg = page_of_addr(e->obj, P2V(PTE_ADDR(*pte)));
    *pte = V2P(mem) | PTE_P | perm | w | nx;
    tlb_flush(p->p_p);
    if (pg != 0)
      page_release(e->obj, pg);
    return 0;
  }

  if (e->obj == 0) {
    if ((mem = page_alloc_user()) == 0)
      return FNU_ERR_NOMEMORY;
    memset(mem, 0, PGSIZE);
    *pte = V2P(mem) | PTE_P | perm | w | nx;
    p->p_p->rss++;
    return 0;
  }

  if (nofault)
    return FNU_ERR_WOULDBLOCK;
  pg = object_page(e->obj, e->offset + (page - e->start), &err);
  if (pg == 0)
    return err;
  if (e->shared && w && write) {
    perm |= PTE_W;
    pg->dirty = 1;
  }
  if (!e->shared && write) {
    if ((mem = page_alloc_user()) == 0) {
      page_release(e->obj, pg);
      return FNU_ERR_NOMEMORY;
    }
    memmove(mem, pg->addr, PGSIZE);
    page_release(e->obj, pg);
    *pte = V2P(mem) | PTE_P | perm | w | nx;
    p->p_p->rss++;
    return 0;
  }
  *pte = V2P(pg->addr) | PTE_P | perm | PTE_CACHE | nx;
  p->p_p->rss++;
  return 0;
}

// 0, or why the page is not there
int vm_fault(struct proc *p, uintptr_t va, int write) {
  struct proc *self = curproc;
  struct process *pr;
  struct vm_entry *e;
  pte_t *pte;
  int nofault, r = FNU_ERR_FAULT;

  if (p == 0 || va >= USERTOP)
    return FNU_ERR_FAULT;
  pr = p->p_p;
  nofault = self != 0 && self->p_p == pr && self->nofault;
  if (holdingsleep(&pr->vmlock))
    return FNU_ERR_WOULDBLOCK;
  // no memory: let go, wait, fault again
  for (;;) {
    if (nofault) {
      if (!trysleep(&pr->vmlock))
        return FNU_ERR_WOULDBLOCK;
    } else {
      vm_lock(pr);
    }
    r = FNU_ERR_FAULT;
    pte = hat_pte(pr->pgdir, (void *)PGROUNDDOWN(va), 0);
    if (pte != 0 && (*pte & PTE_SWAPPED))
      r = nofault ? FNU_ERR_WOULDBLOCK : swap_in(pte, pr);
    else if ((e = entry_find(p, va)) != 0)
      r = fault_in_entry(p, e, va, write, 0, nofault);
    vm_unlock(pr);
    if (r != FNU_ERR_NOMEMORY || nofault || page_wait() < 0)
      return r;
  }
}

int vm_fault_pgdir(pte_t *pgdir, uintptr_t va, int write) {
  struct proc *p = proc_of_pgdir(pgdir);

  return p != 0 ? vm_fault(p, va, write) : FNU_ERR_FAULT;
}

int vm_write_force(struct proc *p, uintptr_t va, const void *src, size_t len) {
  struct process *pr = p->p_p;
  const char *from = src;
  struct vm_entry *e;
  uintptr_t page;
  pte_t *pte;
  size_t n;
  int r = 0;

  vm_lock(pr);
  while (len > 0) {
    page = PGROUNDDOWN(va);
    r = -1;
    if (va >= USERTOP || (e = entry_find(p, va)) == 0)
      break;
    pte = hat_pte(pr->pgdir, (void *)page, 0);
    if (pte != 0 && (*pte & PTE_SWAPPED) && swap_in(pte, pr) != 0)
      break;
    if (fault_in_entry(p, e, va, 1, 1, 0) != 0 ||
        (pte = hat_pte(pr->pgdir, (void *)page, 0)) == 0 || !(*pte & PTE_P))
      break;
    n = PGSIZE - (va - page);
    if (n > len)
      n = len;
    memmove((char *)P2V(PTE_ADDR(*pte)) + (va - page), from, n);
    va += n;
    from += n;
    len -= n;
    r = 0;
  }
  vm_unlock(pr);
  return r;
}

// THE ENTRIES OF A NEW IMAGE
struct vm_entry *vm_entry_text(struct inode *ip, uintptr_t start, size_t len,
                               uint64_t offset, int prot) {
  struct vm_entry *e = entry_alloc();

  e->obj = object_get(ip);
  mtx_enter(&uvm.lock);
  e->obj->text++;
  mtx_leave(&uvm.lock);
  e->text = 1;
  e->start = start;
  e->end = start + len;
  e->offset = offset;
  e->prot = prot;
  return e;
}

struct vm_entry *vm_entry_anon(uintptr_t start, size_t len, int prot) {
  struct vm_entry *e = entry_alloc();

  e->start = start;
  e->end = start + len;
  e->prot = prot;
  return e;
}

void vm_entries_attach(struct proc *p, struct vm_entry *list) {
  struct vm_entry **pp;

  for (pp = &p->p_p->vm; *pp != 0; pp = &(*pp)->next)
    ;
  *pp = list;
  vm_account(p->p_p);
}

void vm_entries_free(struct vm_entry *list) {
  struct vm_entry *next;

  for (; list != 0; list = next) {
    next = list->next;
    entry_unref(list);
    entry_free(list);
  }
}

void vm_futex_key(struct proc *p, uintptr_t va, int private, const void **key,
                  uint64_t *off) {
  struct process *pr = p->p_p;
  struct vm_entry *e;

  *key = pr;
  *off = va;
  if (private)
    return;
  vm_lock(pr);
  if ((e = entry_find(p, va)) != 0 && e->shared && e->obj != 0) {
    *key = e->obj;
    *off = e->offset + (va - e->start);
  }
  vm_unlock(pr);
}

int vm_write_mapped(struct inode *ip) {
  struct vm_object *o;
  int mapped = 0;

  mtx_enter(&uvm.lock);
  for (o = uvm.objects; o != 0 && !mapped; o = o->link)
    mapped = o->ip == ip && o->wrefs > 0;
  mtx_leave(&uvm.lock);
  return mapped;
}

int vm_dev_write_mapped(uint dev) {
  struct vm_object *o;
  int mapped = 0;

  mtx_enter(&uvm.lock);
  for (o = uvm.objects; o != 0 && !mapped; o = o->link)
    mapped = o->ip != 0 && o->ip->dev == dev && o->wrefs > 0;
  mtx_leave(&uvm.lock);
  return mapped;
}

int vm_text_busy(struct inode *ip) {
  struct vm_object *o;
  int busy = 0;

  mtx_enter(&uvm.lock);
  for (o = uvm.objects; o != 0 && !busy; o = o->link)
    busy = o->ip == ip && o->text > 0;
  mtx_leave(&uvm.lock);
  return busy;
}

uint64_t vm_size(struct process *pr) { return pr->vmsize; }

// MMAP AND MUNMAP
int vm_mmap(struct proc *p, uintptr_t addr, size_t len, int prot, int flags,
            struct inode *ip, uint64_t offset, uintptr_t *out) {
  struct vm_entry *e, *last = 0;
  struct vm_object *o;
  uintptr_t start;

  if (len == 0 || (offset & (PGSIZE - 1)) != 0)
    return FNU_ERR_INVALID;
  len = PGROUNDUP(len);
  vm_lock(p->p_p);
  if (flags & FNU_MEM_FIXED) {
    int error;

    if ((addr & (PGSIZE - 1)) != 0 || addr == 0 || addr + len > USERTOP ||
        addr + len < addr) {
      vm_unlock(p->p_p);
      return FNU_ERR_INVALID;
    }
    if ((error = fixed_clear(p, addr, len, flags)) != 0) {
      vm_unlock(p->p_p);
      return error;
    }
    start = addr;
  } else {
    start = MMAPBASE;
    for (;;) {
      if (start + len > USERTOP || start + len < start) {
        vm_unlock(p->p_p);
        return FNU_ERR_NOMEMORY;
      }
      for (e = p->p_p->vm; e != 0; e = e->next)
        if (start < e->end && e->start < start + len)
          break;
      if (e != 0)
        start = PGROUNDUP(e->end);
      else if (hat_range_empty(p->p_p->pgdir, start, len))
        break;
      else
        start += PGSIZE;
    }
  }
  // RLIMIT_DATA
  if (ip == 0 && !(flags & FNU_MEM_SHARED) &&
      p->p_p->rlim[FNU_LIMIT_DATA].cur != FNU_LIMIT_NONE &&
      anon_size(p->p_p) + len > p->p_p->rlim[FNU_LIMIT_DATA].cur) {
    vm_unlock(p->p_p);
    return FNU_ERR_NOMEMORY;
  }
  o = 0;
  if (ip != 0 || (flags & FNU_MEM_SHARED))
    o = object_get(ip);
  e = entry_alloc();
  e->start = start;
  e->end = start + len;
  e->obj = o;
  e->offset = offset;
  e->prot = prot;
  e->shared = (flags & FNU_MEM_SHARED) != 0;
  e->next = 0;
  if (writes_object(e)) {
    mtx_enter(&uvm.lock);
    o->wrefs++;
    mtx_leave(&uvm.lock);
  }
  if (p->p_p->vm == 0)
    p->p_p->vm = e;
  else {
    for (last = p->p_p->vm; last->next != 0; last = last->next)
      ;
    last->next = e;
  }
  vm_account(p->p_p);
  vm_unlock(p->p_p);
  *out = start;
  return 0;
}

static void munmap_locked(struct proc *p, uintptr_t addr, size_t len) {
  struct reap r = {.pr = p->p_p};
  struct vm_entry *e, **pp, *tail;
  uintptr_t end = addr + len, a;

  for (pp = &p->p_p->vm; (e = *pp) != 0;) {
    uintptr_t lo, hi;
    if (end <= e->start || addr >= e->end) {
      pp = &e->next;
      continue;
    }
    lo = addr > e->start ? addr : e->start;
    hi = end < e->end ? end : e->end;
    for (a = lo; a < hi; a += PGSIZE)
      unmap_page(p, e, a, &r);
    reap_flush(&r);
    if (lo == e->start && hi == e->end) {
      *pp = e->next;
      entry_unref(e);
      entry_free(e);
      continue;
    }
    if (lo == e->start) {
      e->offset += hi - e->start;
      e->start = hi;
    } else if (hi == e->end) {
      e->end = lo;
    } else {
      tail = entry_alloc();
      *tail = *e;
      tail->start = hi;
      tail->offset = e->offset + (hi - e->start);
      e->end = lo;
      entry_ref(e);
      tail->next = e->next;
      e->next = tail;
    }
    pp = &e->next;
  }
  tlb_flush(p->p_p);
  vm_account(p->p_p);
}

int vm_munmap(struct proc *p, uintptr_t addr, size_t len) {
  vm_lock(p->p_p);
  munmap_locked(p, addr, len);
  vm_unlock(p->p_p);
  return 0;
}

int vm_mprotect(struct proc *p, uintptr_t addr, size_t len, int prot) {
  struct vm_entry *e, **pp, *piece;
  uintptr_t end = addr + len, a;
  pte_t *pte;

  vm_lock(p->p_p);
  for (pp = &p->p_p->vm; (e = *pp) != 0; pp = &e->next) {
    uintptr_t lo, hi;
    if (end <= e->start || addr >= e->end)
      continue;
    lo = addr > e->start ? addr : e->start;
    hi = end < e->end ? end : e->end;
    if (lo > e->start) {
      piece = entry_alloc();
      *piece = *e;
      piece->end = lo;
      e->offset += lo - e->start;
      e->start = lo;
      entry_ref(e);
      piece->next = e;
      *pp = piece;
      pp = &piece->next;
    }
    if (hi < e->end) {
      piece = entry_alloc();
      *piece = *e;
      piece->start = hi;
      piece->offset = e->offset + (hi - e->start);
      e->end = hi;
      entry_ref(e);
      piece->next = e->next;
      e->next = piece;
    }
    if (e->obj != 0 && e->shared) {
      mtx_enter(&uvm.lock);
      e->obj->wrefs += ((prot & FNU_MEM_WRITE) != 0) - ((e->prot & FNU_MEM_WRITE) != 0);
      mtx_leave(&uvm.lock);
    }
    e->prot = prot;
    for (a = e->start; a < e->end; a += PGSIZE) {
      if ((pte = hat_pte(p->p_p->pgdir, (void *)a, 0)) == 0 || !(*pte & PTE_P))
        continue;
      if (!(prot & FNU_MEM_WRITE))
        *pte &= ~(pte_t)PTE_W;
      if (prot & FNU_MEM_EXEC)
        *pte &= ~PTE_NX;
      else
        *pte |= vm_nx();
      if (!(prot & (FNU_MEM_READ | FNU_MEM_WRITE | FNU_MEM_EXEC)))
        *pte &= ~(pte_t)PTE_U;
      else
        *pte |= PTE_U;
    }
  }
  tlb_flush(p->p_p);
  vm_unlock(p->p_p);
  return 0;
}

int vm_madvise(struct proc *p, uintptr_t addr, size_t len, int advice) {
  struct reap r;
  struct vm_entry *e;
  uintptr_t a, end = addr + len;

  if (advice != FNU_MEMUSE_FREE && advice != FNU_MEMUSE_DONTNEED)
    return 0;
  r.pr = p->p_p;
  r.n = 0;
  vm_lock(p->p_p);
  for (e = p->p_p->vm; e != 0; e = e->next) {
    if (e->shared || end <= e->start || addr >= e->end)
      continue;
    for (a = e->start > addr ? e->start : addr; a < end && a < e->end;
         a += PGSIZE)
      unmap_page(p, e, a, &r);
    reap_flush(&r);
  }
  vm_unlock(p->p_p);
  return 0;
}

int vm_msync(struct proc *p, uintptr_t addr, size_t len) {
  struct vm_entry *e;
  uintptr_t a, end = addr + len;
  pte_t *pte;
  struct vm_page *pg;

  vm_lock(p->p_p);
  for (e = p->p_p->vm; e != 0; e = e->next) {
    if (!e->shared || end <= e->start || addr >= e->end)
      continue;
    for (a = e->start > addr ? e->start : addr; a < end && a < e->end; a += PGSIZE) {
      pte = hat_pte(p->p_p->pgdir, (void *)a, 0);
      if (pte == 0 || !(*pte & PTE_P) || !(*pte & PTE_CACHE))
        continue;
      pg = page_of_addr(e->obj, P2V(PTE_ADDR(*pte)));
      if (pg == 0)
        continue;
      if (*pte & PTE_D) {
        pg->dirty = 1;
        *pte &= ~(pte_t)PTE_D;
      }
      page_writeback(e->obj, pg);
    }
  }
  tlb_flush(p->p_p);
  vm_unlock(p->p_p);
  return 0;
}

// FORK AND EXIT
int vm_fork(struct proc *parent, struct proc *child) {
  struct vm_entry *e, *ne, **tail = &child->p_p->vm;
  uintptr_t a;
  pte_t *pte, *npte;
  char *mem;

  child->p_p->vm = 0;
  vm_lock(parent->p_p);
  for (e = parent->p_p->vm; e != 0; e = e->next) {
    ne = entry_alloc();
    *ne = *e;
    ne->next = 0;
    entry_ref(e);
    *tail = ne;
    tail = &ne->next;
    for (a = e->start; a < e->end; a += PGSIZE) {
      pte = hat_pte(parent->p_p->pgdir, (void *)a, 0);
      if (pte == 0 || !(*pte & (PTE_P | PTE_SWAPPED)))
        continue;
      if ((npte = hat_pte(child->p_p->pgdir, (void *)a, 1)) == 0)
        goto bad;
      if (*pte & PTE_SWAPPED) {
        if ((mem = page_alloc_wait(1)) == 0)
          goto bad;
        if (swap_read_slot(*pte, mem) < 0) {
          page_free(mem);
          goto bad;
        }
        *npte = V2P(mem) | PTE_P | (*pte & (PTE_U | PTE_W | PTE_NX));
        child->p_p->rss++;
        continue;
      }
      if (*pte & PTE_CACHE) {
        struct vm_page *pg = page_of_addr(e->obj, P2V(PTE_ADDR(*pte)));
        if (pg == 0)
          continue;
        if (!e->shared)
          *pte &= ~(pte_t)PTE_W;
        mtx_enter(&uvm.lock);
        pg->refs++;
        mtx_leave(&uvm.lock);
        *npte = *pte;
        child->p_p->rss++;
        continue;
      }
      if ((mem = page_alloc_wait(1)) == 0)
        goto bad;
      memmove(mem, P2V(PTE_ADDR(*pte)), PGSIZE);
      *npte = V2P(mem) | PTE_FLAGS(*pte);
      child->p_p->rss++;
    }
  }
  tlb_flush(parent->p_p);
  vm_unlock(parent->p_p);
  vm_account(child->p_p);
  return 0;
bad:
  tlb_flush(parent->p_p);
  vm_unlock(parent->p_p);
  vm_release(child);
  return -1;
}

void vm_release(struct proc *p) {
  struct reap r = {.pr = p->p_p};
  struct vm_entry *e, *next;
  uintptr_t a;

  vm_lock(p->p_p);
  for (e = p->p_p->vm; e != 0; e = next) {
    next = e->next;
    for (a = e->start; a < e->end; a += PGSIZE)
      unmap_page(p, e, a, &r);
    reap_flush(&r);
    entry_unref(e);
    entry_free(e);
  }
  p->p_p->vm = 0;
  vm_account(p->p_p);
  vm_unlock(p->p_p);
}

// THE SWAP
static struct {
  struct mutex lock;
  int on;
  uint dev;
  struct inode *ip;
  uint64_t nslots;
  uchar *map;
  uint64_t used;
  uint64_t hint;
  uint64_t outs, ins;
  uint64_t bad; // slots that would not take a page
  struct buf rbuf; // pages coming in, any process
  struct buf wbuf; // pages going out, the daemon's alone
  char *cluster;   // a run of them, one write
  int clpages;
} swap;

void swapinit(void) {
  mtx_init(&swap.lock, "swap");
  initsleeplock(&swap.rbuf.lock, "swapin");
  initsleeplock(&swap.wbuf.lock, "swapout");
}

static int swap_full(void) { return !swap.on || swap.used >= swap.nslots; }

// the swap is on dev, or on a partition of it
int swap_holds(uint dev) {
  int r;

  mtx_enter(&swap.lock);
  r = swap.on && (swap.dev == dev || partition_disk(swap.dev) == dev);
  mtx_leave(&swap.lock);
  return r;
}

void swap_stats(uint64_t *total, uint64_t *used, uint64_t *outs, uint64_t *ins) {
  *total = swap.on ? swap.nslots * PGSIZE : 0;
  *used = swap.on ? swap.used * PGSIZE : 0;
  *outs = swap.outs;
  *ins = swap.ins;
}

#define SLOT_USED(s) (swap.map[(s) >> 3] & (1 << ((s) & 7)))

// want slots in a row, else the first free run
static int slot_alloc_run(int want, uint64_t *out) {
  uint64_t i, s, run, first = ~(uint64_t)0, firstrun = 0;

  mtx_enter(&swap.lock);
  for (i = 0; i < swap.nslots; i += run) {
    s = (swap.hint + i) % swap.nslots;
    run = 1;
    if (SLOT_USED(s))
      continue;
    while (run < (uint64_t)want && s + run < swap.nslots && !SLOT_USED(s + run))
      run++;
    if (first == ~(uint64_t)0) {
      first = s;
      firstrun = run;
    }
    if (run == (uint64_t)want) {
      first = s;
      firstrun = run;
      break;
    }
  }
  if (first == ~(uint64_t)0) {
    mtx_leave(&swap.lock);
    return 0;
  }
  for (s = first; s < first + firstrun; s++)
    swap.map[s >> 3] |= (uchar)(1 << (s & 7));
  swap.used += firstrun;
  swap.hint = first + firstrun;
  mtx_leave(&swap.lock);
  *out = first;
  return (int)firstrun;
}

static void slot_free(uint64_t s) {
  mtx_enter(&swap.lock);
  if (s < swap.nslots && (swap.map[s >> 3] & (1 << (s & 7)))) {
    swap.map[s >> 3] &= (uchar)~(1 << (s & 7));
    swap.used--;
  }
  mtx_leave(&swap.lock);
}

static int slot_io(uint64_t s, char *page, int npages, int write) {
  struct buf *b = write ? &swap.wbuf : &swap.rbuf;
  int err;

  proc_swap_freeze(curproc);
  acquiresleep(&b->lock);
  b->flags = write ? B_DIRTY : 0;
  b->dev = swap.dev;
  b->blockno = s * (PGSIZE / BSIZE);
  b->addr = (uchar *)page;
  b->bcount = (uint)npages * PGSIZE;
  blockdev_rw(b);
  err = (b->flags & B_ERROR) != 0;
  releasesleep(&b->lock);
  proc_swap_thaw(curproc);
  return err ? -1 : 0;
}

void swap_free_slot(pte_t pte) { slot_free(PTE_SLOT(pte)); }

// HIBERNATE
int swap_hib_region(uint dev, uint64_t limit, uint64_t *lo, uint64_t *hi) {
  uint64_t s, start = 0, per = PGSIZE / BSIZE, best_lo = 0, best_hi = 0, end;

  if (!swap.on || swap.dev != dev) {
    *lo = 0;
    *hi = limit;
    return limit > 0 ? 0 : -1;
  }
  for (s = 0; s <= swap.nslots; s++) {
    if (s < swap.nslots && !(swap.map[s >> 3] & (1 << (s & 7))))
      continue;
    end = s == swap.nslots ? limit : s * per;
    if (end > limit)
      end = limit;
    if (end > start * per && end - start * per > best_hi - best_lo) {
      best_lo = start * per;
      best_hi = end;
    }
    start = s + 1;
  }
  *lo = best_lo;
  *hi = best_hi;
  return best_hi > best_lo ? 0 : -1;
}

// image sectors, kept from the pager till written or given up
void swap_hib_hold(uint dev, uint64_t lo, uint64_t hi, int hold) {
  uint64_t per = PGSIZE / BSIZE, s;

  mtx_enter(&swap.lock);
  for (s = lo / per; swap.on && swap.dev == dev && s < swap.nslots && s * per < hi;
       s++)
    if (hold && !SLOT_USED(s)) {
      swap.map[s >> 3] |= (uchar)(1 << (s & 7));
      swap.used++;
    } else if (!hold && SLOT_USED(s)) {
      swap.map[s >> 3] &= (uchar)~(1 << (s & 7));
      swap.used--;
    }
  mtx_leave(&swap.lock);
}

int swap_read_slot(pte_t pte, char *mem) {
  if (!swap.on)
    return -1;
  return slot_io(PTE_SLOT(pte), mem, 1, 0);
}

int swapon(struct inode *ip) {
  uint64_t sectors, nslots, bytes;
  char *cluster;
  uchar *map;
  uint dev;
  int clpages;

  if (ip->type != FNU_TYPE_DISK)
    return FNU_ERR_NOTDISK;
  dev = makedev(ip->major, ip->minor);
  sectors = blockdev_device_size(dev);
  if (sectors > (MSGBUF_DUMP_SECTORS + HIB_TAIL_SECTORS) * 4)
    sectors -= MSGBUF_DUMP_SECTORS + HIB_TAIL_SECTORS;
  nslots = sectors / (PGSIZE / BSIZE);
  if (nslots < 8)
    return FNU_ERR_INVALID;
  bytes = (nslots + 7) / 8;
  if ((map = malloc(bytes, M_VM, M_CANFAIL | M_ZERO)) == 0)
    return FNU_ERR_NOMEMORY;
  mtx_enter(&swap.lock);
  if (swap.on) {
    mtx_leave(&swap.lock);
    free(map, M_VM);
    return FNU_ERR_BUSY;
  }
  swap.dev = dev;
  swap.ip = idup(ip);
  swap.nslots = nslots;
  swap.map = map;
  swap.used = 0;
  swap.hint = 0;
  swap.clpages = 1;
  swap.on = 1;
  mtx_leave(&swap.lock);
  // daemon's run: what one transfer carries
  clpages = (int)((blockdev_maxphys(dev) < MAXPHYS ? blockdev_maxphys(dev) : MAXPHYS) / PGSIZE);
  if (clpages > 1 && (cluster = contig_alloc((uint64_t)clpages * PGSIZE)) != 0) {
    acquiresleep(&swap.wbuf.lock);
    swap.cluster = cluster;
    swap.clpages = clpages;
    releasesleep(&swap.wbuf.lock);
  }
  printf("swap: %d pages on device %d,%d, written %d at a time\n", (int)nslots,
         major(dev), minor(dev), swap.clpages);
  return 0;
}

static int drain_process(struct proc *p) {
  struct process *pr = p->p_p;
  struct vm_entry *e;
  uintptr_t a;
  pte_t *pte;

  for (e = pr->vm; e != 0; e = e->next)
    for (a = e->start; a < e->end; a += PGSIZE) {
      pte = hat_pte(pr->pgdir, (void *)a, 0);
      if (pte != 0 && (*pte & PTE_SWAPPED) && swap_in(pte, pr) != 0)
        return -1;
    }
  return 0;
}

static int swap_drain(void) {
  struct proc *p, *me = curproc;
  pid_t hand = 0;
  int held, r;

  vm_lock(me->p_p);
  r = drain_process(me);
  vm_unlock(me->p_p);
  while (r == 0 && (held = proc_swap_next(&hand, &p)) >= 0) {
    if (p->p_p == me->p_p)
      continue;
    if (!held)
      return -1;
    if (trysleep(&p->p_p->vmlock)) {
      r = drain_process(p);
      vm_unlock(p->p_p);
    } else {
      r = -1;
    }
    proc_swap_drop(p);
  }
  return r;
}

int swapoff(void) {
  struct inode *ip;
  char *cluster;
  uchar *map;

  mtx_enter(&swap.lock);
  if (!swap.on) {
    mtx_leave(&swap.lock);
    return FNU_ERR_INVALID;
  }
  mtx_leave(&swap.lock);
  proc_swap_freeze(curproc);
  if (swap_drain() < 0) {
    proc_swap_thaw(curproc);
    return FNU_ERR_NOMEMORY;
  }
  proc_swap_thaw(curproc);
  mtx_enter(&swap.lock);
  if (swap.used != 0) {
    mtx_leave(&swap.lock);
    return FNU_ERR_BUSY;
  }
  swap.on = 0;
  swap.nslots = 0;
  swap.hint = 0;
  map = swap.map;
  swap.map = 0;
  ip = swap.ip;
  swap.ip = 0;
  swap.dev = 0;
  cluster = swap.cluster;
  swap.cluster = 0;
  swap.clpages = 1;
  mtx_leave(&swap.lock);
  if (cluster != 0)
    contig_free(cluster);
  free(map, M_VM);
  blockdev_released(makedev(ip->major, ip->minor));
  iput(ip);
  return 0;
}

// 0, NOMEMORY, or IO when the slot won't read
int swap_in(pte_t *pte, struct process *pr) {
  uint64_t s = PTE_SLOT(*pte);
  char *mem;

  if (!swap.on)
    return FNU_ERR_IO;
  if ((mem = page_alloc_user()) == 0)
    return FNU_ERR_NOMEMORY;
  if (slot_io(s, mem, 1, 0) < 0) {
    page_free(mem);
    return FNU_ERR_IO;
  }
  *pte = V2P(mem) | PTE_P | (*pte & (PTE_U | PTE_W | PTE_NX));
  pr->rss++;
  slot_free(s);
  swap.ins++;
  tlb_flush(pr);
  return 0;
}

// a run of pages, one write; failed slots stay taken
static int swap_out_run(struct proc *p, pte_t **ptes, int n) {
  int i, k, got, taken = 0;
  uint64_t s;
  char *buf;

  for (i = 0; i < n; i += got) {
    if ((got = slot_alloc_run(swap.cluster != 0 ? n - i : 1, &s)) == 0)
      break;
    if (got == 1)
      buf = P2V(PTE_ADDR(*ptes[i]));
    else {
      buf = swap.cluster;
      for (k = 0; k < got; k++)
        memmove(buf + k * PGSIZE, P2V(PTE_ADDR(*ptes[i + k])), PGSIZE);
    }
    if (slot_io(s, buf, got, 1) < 0) {
      swap.bad += (uint64_t)got;
      printf("swap: slots %lu-%lu would not take a write, retired\n",
             (unsigned long)s, (unsigned long)(s + (uint64_t)got - 1));
      continue;
    }
    for (k = 0; k < got; k++) {
      pte_t *pte = ptes[i + k];
      char *addr = P2V(PTE_ADDR(*pte));

      *pte = SLOT_PTE(s + (uint64_t)k) | PTE_SWAPPED | (*pte & (PTE_U | PTE_W | PTE_NX));
      page_free(addr);
    }
    swap.outs += (uint64_t)got;
    p->p_p->rss -= got;
    taken += got;
  }
  return taken;
}

static int freemin, freetarg;
static int pageout_goal;      // uvm_hibernate_shrink's

static int pageout_target(void) {
  return pageout_goal > freetarg ? pageout_goal : freetarg;
}

void pageout_targets(int *min, int *targ) {
  *min = freemin;
  *targ = freetarg;
}

static pid_t pageout_pid;
static uintptr_t pageout_va;

static int pageout_range(struct proc *p, uintptr_t lo, uintptr_t hi, int want,
                         uintptr_t *stop) {
  pte_t *pte, *run[MAXPHYS / PGSIZE];
  int taken = 0, n = 0;
  uintptr_t a;

  // stop at the target
  for (a = lo; a < hi && taken + n < want && kalloc_nfree() < pageout_target();
       a += PGSIZE) {
    pte = hat_pte(p->p_p->pgdir, (void *)a, 0);
    if (pte == 0 || !(*pte & PTE_P) || !(*pte & PTE_U) || (*pte & PTE_CACHE))
      continue;
    if (*pte & PTE_A) {
      *pte &= ~(pte_t)PTE_A;
      continue;
    }
    run[n++] = pte;
    if (n == swap.clpages) {
      taken += swap_out_run(p, run, n);
      n = 0;
    }
  }
  if (n > 0)
    taken += swap_out_run(p, run, n);
  *stop = a;
  return taken;
}

static int pageout(int want) {
  int taken = 0, tries, held;
  struct proc *p;
  struct vm_entry *e;
  uintptr_t cur, stop, lo;
  pid_t hand;

  proc_swap_freeze(curproc);
  for (tries = 0; tries <= 2 * get_nprocs() && taken < want &&
                  kalloc_nfree() < pageout_target();
       tries++) {
    hand = pageout_va != 0 ? pageout_pid - 1 : pageout_pid;
    if ((held = proc_swap_next(&hand, &p)) < 0) {
      pageout_pid = 0;
      pageout_va = 0;
      continue;
    }
    cur = hand == pageout_pid ? pageout_va : 0;
    pageout_pid = hand;
    pageout_va = 0;
    if (!held)
      continue;
    if (!trysleep(&p->p_p->vmlock)) {
      proc_swap_drop(p);
      continue;
    }
    stop = cur;
    for (e = p->p_p->vm; e != 0 && taken < want; e = e->next) {
      lo = e->start > cur ? e->start : cur;
      if (e->shared || e->end <= lo)
        continue;
      taken += pageout_range(p, lo, e->end, want - taken, &stop);
    }
    if (taken >= want)
      pageout_va = stop;
    vm_unlock(p->p_p);
    proc_swap_drop(p);
  }
  proc_swap_thaw(curproc);
  return taken;
}


static void pageout_tune(void) {
  int npages = (int)(get_total_ram() / PGSIZE);

  freemin = npages / 30;
  if (freemin < (16 * 1024) / PGSIZE)
    freemin = (16 * 1024) / PGSIZE;
  freetarg = freemin * 4 / 3;
  if (freetarg <= freemin)
    freetarg = freemin + 1;
}

void pageout_wakeup(void) { wakeup(&pageout_pid); }

// the daemon frees memory, the rest sleep until it has
#define PD_GIVEUP 3 // sweeps that freed nothing before a fault is refused
static int pd_wanted, pd_stuck;

int page_wait(void) {
  if (swap_full() || pd_stuck >= PD_GIVEUP)
    return -1;
  pageout_wakeup();
  tsleep(&pd_wanted, PVM, "flt_noram", HZ);
  return 0;
}

// the daemon pages out until goal pages are free
int uvm_hibernate_shrink(int goal) {
  int before = kalloc_nfree(), last, idle = 0;

  if (page_daemon == 0)
    return 0;
  pageout_goal = goal;
  while (kalloc_nfree() < goal && !swap_full() && idle < PD_GIVEUP) {
    last = kalloc_nfree();
    pageout_wakeup();
    tsleep(&pd_wanted, PVM, "hibshrink", HZ);
    idle = kalloc_nfree() > last ? 0 : idle + 1;
  }
  pageout_goal = 0;
  return kalloc_nfree() - before;
}

char *page_alloc_wait(int user) {
  char *mem;

  for (;;) {
    if ((mem = user ? page_alloc_user() : page_alloc()) != 0) {
      if (swap.on && kalloc_nfree() < freemin)
        pageout_wakeup();
      return mem;
    }
    if (page_daemon == 0 || curproc == page_daemon || page_wait() < 0)
      return 0;
  }
}

// woken when short, and 10 Hz for the sneaks
void pagedaemon(void *arg __unused) {
  int free, got;

  page_daemon = curproc;
  for (;;) {
    tsleep(&pageout_pid, PVM, "pgdaemon", HZ / 10);
    free = kalloc_nfree();
    if (free >= pageout_target()) {
      pd_stuck = 0;
      wakeup(&pd_wanted);
      continue;
    }
    got = swap_full() ? 0 : pageout(pageout_target() - free);
    if (got > 0 || kalloc_nfree() > page_reserve_kernel)
      pd_stuck = 0;
    else if (pd_stuck < PD_GIVEUP)
      pd_stuck++;
    wakeup(&pd_wanted);
  }
}
