#include "defs.h"
#include "elf.h"
#include "malloc.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "file.h"
#include "inc/open.h"
#include "inc/stat.h"
#include "uvm.h"
#include "vfs.h"
#include "pathname.h"
#include "unveil.h"

#define MAXINTERP 128
#define ELF_MAX_VALID_PHDR 32

// THE ARGUMENT VECTORS
struct argvec {
  char *pre[3];
  int npre;
  int skip;
  char **kv;
};

static char *vec_at(struct argvec *v, int i) {
  if (i < v->npre)
    return v->pre[i];
  i += v->skip - v->npre;
  return v->kv != 0 ? v->kv[i] : 0;
}

static int vec_size(struct argvec *v, int *count, size_t *bytes) {
  size_t total = 0;
  char *s;
  int n;

  for (n = 0;; n++) {
    if ((s = vec_at(v, n)) == 0)
      break;
    total += strlen(s) + 1;
    if (total > NCARGS)
      return FNU_ERR_ARGSTOOLONG;
  }
  *count = n;
  *bytes = total;
  return 0;
}

static void args_keep(struct argvec *v, char *out, size_t n) {
  size_t used = 0, len;
  char *s;
  int i;

  out[0] = 0;
  for (i = 0; (s = vec_at(v, i)) != 0 && used + 1 < n; i++) {
    if (i > 0)
      out[used++] = ' ';
    len = strlen(s);
    if (len > n - 1 - used)
      len = n - 1 - used;
    memmove(out + used, s, len);
    used += len;
  }
  out[used] = 0;
}

static int vec_copyout(pte_t *pgdir, struct argvec *v, int n, uintptr_t at,
                       uintptr_t array) {
  uintptr_t sp = at;
  char *s;
  int i;

  for (i = 0; i < n; i++, sp += strlen(s) + 1) {
    if ((s = vec_at(v, i)) == 0)
      return FNU_ERR_FAULT;
    if (copyout(pgdir, sp, s, strlen(s) + 1) < 0)
      return FNU_ERR_FAULT;
    if (copyout(pgdir, array + (uintptr_t)i * sizeof(uintptr_t), &sp,
                sizeof(sp)) < 0)
      return FNU_ERR_FAULT;
  }
  sp = 0;
  if (copyout(pgdir, array + (uintptr_t)n * sizeof(uintptr_t), &sp,
              sizeof(sp)) < 0)
    return FNU_ERR_FAULT;
  return 0;
}

static int exec1(char *path, struct argvec *argv, struct argvec *envp,
                 int depth);

static int exec_script(struct inode *ip, char *path, struct argvec *argv,
                       struct argvec *envp, int depth) {
  char line[MAXINTERP + 1], *interp, *arg = 0, *p;
  struct argvec nargv;
  int n;

  if (depth > 0)
    return FNU_ERR_BADEXEC;
  if ((n = readi(ip, line, 0, MAXINTERP)) < 2)
    return FNU_ERR_BADEXEC;
  line[n] = 0;
  if ((p = strchr(line, '\n')) == 0)
    return FNU_ERR_BADEXEC;
  *p = 0;
  for (p = line + 2; *p == ' ' || *p == '\t'; p++)
    ;
  interp = p;
  while (*p != 0 && *p != ' ' && *p != '\t')
    p++;
  if (*p != 0) {
    *p++ = 0;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p != 0)
      arg = p;
  }
  if (*interp == 0)
    return FNU_ERR_BADEXEC;
  iunlockput(ip);
  nargv = *argv;
  nargv.npre = 0;
  nargv.pre[nargv.npre++] = interp;
  if (arg != 0)
    nargv.pre[nargv.npre++] = arg;
  nargv.pre[nargv.npre++] = path;
  nargv.skip = argv->skip + 1;
  return exec1(interp, &nargv, envp, depth + 1);
}

int exec(char *path, char **argv, char **envp) {
  struct argvec a, e;

  memset(&a, 0, sizeof(a));
  memset(&e, 0, sizeof(e));
  a.kv = argv;
  e.kv = envp;
  return exec1(path, &a, envp != 0 ? &e : 0, 0);
}

static int vec_copyin(uintptr_t uvec, char **ptrs, int room, char *buf,
                      size_t *used) {
  pte_t *pgdir = curproc->p_p->pgdir;
  uint64_t u;
  size_t done;
  int n, error;

  for (n = 0; uvec != 0; n++) {
    if (copyin(pgdir, &u, uvec + (uint64_t)n * sizeof(u), sizeof(u)) < 0)
      return -FNU_ERR_FAULT;
    if (u == 0)
      break;
    if (n >= room - 1)
      return -FNU_ERR_ARGSTOOLONG;
    if ((error = copyinstr(pgdir, buf + *used, (uintptr_t)u, NCARGS - *used,
                           &done)) != 0)
      return error == FNU_ERR_NAMETOOLONG ? -FNU_ERR_ARGSTOOLONG : -error;
    ptrs[n] = buf + *used;
    *used += done;
  }
  ptrs[n] = 0;
  return n + 1;
}

int exec_user(uintptr_t upath, uintptr_t uargv, uintptr_t uenvp) {
  int room = NCARGS / (int)sizeof(char *) + 2, na, ne;
  struct pathname path;
  char **ptrs, *buf;
  size_t used = 0;
  int error;

  if ((error = pn_get(upath, &path)) != 0)
    return error;
  buf = malloc(NCARGS, M_TEMP, M_WAITOK);
  ptrs = malloc((size_t)room * sizeof(char *), M_TEMP, M_WAITOK);
  if ((na = vec_copyin(uargv, ptrs, room, buf, &used)) < 0)
    error = -na;
  else if ((ne = vec_copyin(uenvp, ptrs + na, room - na, buf, &used)) < 0)
    error = -ne;
  else
    error = exec(path.pn_buf, ptrs, ptrs + na);
  free(ptrs, M_TEMP);
  free(buf, M_TEMP);
  pn_free(&path);
  return error;
}

// THE IMAGE
enum { PG_NONE, PG_FILE, PG_COPY, PG_ZERO };

struct pageplan {
  int kind;
  int prot;
  uint64_t off;
};

static int prot_of(uint flags) {
  int prot = FNU_MEM_READ;

  if (flags & ELF_PROG_FLAG_WRITE)
    prot |= FNU_MEM_WRITE;
  if (flags & ELF_PROG_FLAG_EXEC)
    prot |= FNU_MEM_EXEC;
  return prot;
}

static void plan_page(struct proghdr *seg, int nseg, uintptr_t va,
                      struct pageplan *pp) {
  int i, n = 0, filebytes = 0;
  struct proghdr *one = 0;

  pp->kind = PG_NONE;
  pp->prot = 0;
  for (i = 0; i < nseg; i++) {
    struct proghdr *s = &seg[i];

    if (s->vaddr >= va + PGSIZE || s->vaddr + s->memsz <= va)
      continue;
    n++;
    one = s;
    pp->prot |= prot_of(s->flags);
    if (s->vaddr < va + PGSIZE && s->vaddr + s->filesz > va)
      filebytes = 1;
  }
  if (n == 0)
    return;
  if (!filebytes) {
    pp->kind = PG_ZERO;
    return;
  }
  if (n == 1 && one->vaddr <= va && one->vaddr + one->filesz >= va + PGSIZE &&
      (one->off & (PGSIZE - 1)) == (one->vaddr & (PGSIZE - 1))) {
    pp->kind = PG_FILE;
    pp->off = one->off + (va - one->vaddr);
    return;
  }
  pp->kind = PG_COPY;
}

static int copy_page(pte_t *pgdir, struct inode *ip, struct proghdr *seg,
                     int nseg, uintptr_t va, int prot) {
  char *mem = page_alloc_wait(1);
  pte_t perm = 0;
  int i;

  if (mem == 0)
    return FNU_ERR_NOMEMORY;
  memset(mem, 0, PGSIZE);
  for (i = 0; i < nseg; i++) {
    struct proghdr *s = &seg[i];
    uint64_t lo = s->vaddr > va ? s->vaddr : va;
    uint64_t hi = s->vaddr + s->filesz < va + PGSIZE ? s->vaddr + s->filesz
                                                      : va + PGSIZE;

    if (lo >= hi)
      continue;
    if (readi(ip, mem + (lo - va), (uint)(s->off + (lo - s->vaddr)),
              (uint)(hi - lo)) != (int)(hi - lo)) {
      page_free(mem);
      return FNU_ERR_IO;
    }
  }
  if (prot & FNU_MEM_WRITE)
    perm |= PTE_W;
  if (!(prot & FNU_MEM_EXEC))
    perm |= vm_nx();
  if (hat_memload(pgdir, va, mem, perm) < 0) {
    page_free(mem);
    return FNU_ERR_NOMEMORY;
  }
  return 0;
}

// the image's entries, by address
static int build_image(pte_t *pgdir, struct inode *ip, struct proghdr *seg,
                       int nseg, uintptr_t lo, uintptr_t hi,
                       struct vm_entry **list) {
  struct vm_entry **tail = list, *e;
  struct pageplan pp, run;
  uintptr_t va, start = 0;
  int error;

  run.kind = PG_NONE;
  for (va = lo; va <= hi; va += PGSIZE) {
    if (va < hi)
      plan_page(seg, nseg, va, &pp);
    else
      pp.kind = PG_NONE;
    if (run.kind != PG_NONE &&
        (pp.kind != run.kind || pp.prot != run.prot ||
         (pp.kind == PG_FILE && pp.off != run.off + (va - start)))) {
      if (run.kind == PG_FILE)
        e = vm_entry_text(ip, start, va - start, run.off, run.prot);
      else
        e = vm_entry_anon(start, va - start, run.prot);
      if (e == 0)
        return FNU_ERR_NOMEMORY;
      *tail = e;
      tail = &e->next;
      run.kind = PG_NONE;
    }
    if (pp.kind == PG_COPY &&
        (error = copy_page(pgdir, ip, seg, nseg, va, pp.prot)) != 0)
      return error;
    if (pp.kind != PG_NONE && run.kind == PG_NONE) {
      run = pp;
      start = va;
    }
  }
  return 0;
}

static int exec1(char *path, struct argvec *argv, struct argvec *envp,
                 int depth) {
  int i;
  uint64_t off, ssize;
  uintptr_t sp;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph, *seg = 0;
  struct vm_entry *image = 0;
  pte_t *pgdir = NULL, *oldpgdir;
  int setuid_to = -1, setgid_to = -1;
  uintptr_t lo = ~(uintptr_t)0, hi = 0;
  int nseg = 0;
  int argc = 0, envc = 0, error = FNU_ERR_BADEXEC, leave = 0;
  size_t argbytes = 0, envbytes = 0;
  uintptr_t envp_at, argv_at, top;
  uint64_t head[4];


  if ((error = namei_at(0, path, depth == 0 ? UV_EXEC : UV_NONE, &ip)) != 0) {
    return error;
  }
  ilock(ip);
  if (!inode_access(ip, curproc->p_p->uid, 1)) {
    error = FNU_ERR_DENIED;
    goto bad;
  }
  if (ip->writecount > 0 || vm_write_mapped(ip)) {
    error = FNU_ERR_TEXTBUSY;
    goto bad;
  }
  setuid_to = (ip->mode & FNU_MODE_SETUID) ? (int)ip->uid : -1;
  setgid_to = (ip->mode & FNU_MODE_SETGID) ? (int)ip->gid : -1;
  if (vfs_nosuid(ip->dev))
    setuid_to = setgid_to = -1;

  memset(&elf, 0, sizeof(elf));
  if (readi(ip, (char *)&elf, 0, sizeof(elf)) < 2) {
    goto bad;
  }
  if (((char *)&elf)[0] == '#' && ((char *)&elf)[1] == '!')
    return exec_script(ip, path, argv, envp, depth);
  if (elf.magic != ELF_MAGIC) {
    goto bad;
  }

  if (elf.phnum == 0 || elf.phnum > ELF_MAX_VALID_PHDR ||
      elf.phentsize != sizeof(ph) || elf.phoff > ip->size ||
      ip->size - elf.phoff < elf.phnum * sizeof(ph))
    goto bad;
  seg = malloc(elf.phnum * sizeof(ph), M_TEMP, M_WAITOK);
  for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
    if (readi(ip, (char *)&ph, (uint)off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if (ph.type != ELF_PROG_LOAD || ph.memsz == 0)
      continue;
    if (ph.memsz < ph.filesz || ph.vaddr < PGSIZE ||
        ph.vaddr + ph.memsz < ph.vaddr || ph.vaddr + ph.memsz > MMAPBASE ||
        ph.off + ph.filesz < ph.off || ph.off + ph.filesz > ip->size)
      goto bad;
    seg[nseg++] = ph;
    if (PGROUNDDOWN(ph.vaddr) < lo)
      lo = PGROUNDDOWN(ph.vaddr);
    if (PGROUNDUP(ph.vaddr + ph.memsz) > hi)
      hi = PGROUNDUP(ph.vaddr + ph.memsz);
  }
  if (nseg == 0 || elf.entry < lo || elf.entry >= hi)
    goto bad;
  if ((pgdir = hat_alloc()) == NULL) {
    error = FNU_ERR_NOMEMORY;
    goto bad;
  }
  if ((error = build_image(pgdir, ip, seg, nseg, lo, hi, &image)) != 0)
    goto bad;
  free(seg, M_TEMP);
  seg = 0;
  iunlockput(ip);
  ip = 0;

  error = FNU_ERR_NOMEMORY;
  if (sigcode_map(pgdir, SIGCODE_VA) < 0)
    goto bad;
  ssize = curproc->p_p->rlim[FNU_LIMIT_STACK].cur;
  if (ssize > MAXSSIZ)
    ssize = MAXSSIZ;
  ssize = PGROUNDUP(ssize);

  if ((error = vec_size(argv, &argc, &argbytes)) != 0)
    goto bad;
  if (envp != 0 && (error = vec_size(envp, &envc, &envbytes)) != 0)
    goto bad;
  error = FNU_ERR_ARGSTOOLONG;
  if (argbytes + envbytes +
          (size_t)(argc + envc + 6) * sizeof(uintptr_t) > NCARGS)
    goto bad;

  if (argbytes + envbytes + (size_t)(argc + envc + 6) * sizeof(uintptr_t) >
      ssize)
    goto bad;

  if (single_thread(curproc) < 0) {
    leave = 1;
    goto bad;
  }

  char *last, *s;
  for (last = s = path; *s; s++) {
    if (*s == '/') {
      last = s + 1;
    }
  }
  strlcpy(curproc->p_p->name, last, sizeof(curproc->p_p->name));
  args_keep(argv, curproc->p_p->args, sizeof(curproc->p_p->args));

  proc_lock();
  curproc->p_p->ps_flags |= PS_EXEC;
  proc_unlock();
  if (setuid_to >= 0)
    curproc->p_p->uid = (uint)setuid_to;
  if (setgid_to >= 0)
    curproc->p_p->gid = (uint)setgid_to;
  if (setuid_to >= 0 || setgid_to >= 0)
    curproc->p_p->sugid = 1;
  curproc->p_p->svuid = curproc->p_p->uid;
  curproc->p_p->svgid = curproc->p_p->gid;
  // pledge(2): execpromises
  if (curproc->p_p->execpledge == 0) {
    unveil_destroy(curproc->p_p);
    curproc->p_p->uvdone = 0;
  }
  curproc->p_p->pledge = curproc->p_p->execpledge;
  curproc->p_p->pledged = curproc->p_p->pledge != 0;
  curproc->p_p->execpledge = 0;
  sigexec(curproc);
  knote_proc(curproc->p_p, FNU_NOTE_EXEC, 0);
  sigcode_attach(curproc, SIGCODE_VA);
  fd_closeall(curproc->p_p, 1);
  vm_release(curproc);
  oldpgdir = curproc->p_p->pgdir;
  curproc->p_p->pgdir = pgdir;
  curproc->p_p->rss = hat_resident(pgdir);
  vm_entries_attach(curproc, image);
  image = 0;
  vm_entries_attach(curproc, vm_entry_anon(USRSTACK - ssize, ssize,
                                           FNU_MEM_READ | FNU_MEM_WRITE));
  hat_switch(curproc->p_p->pgdir);
  single_thread_clear(curproc);
  hat_free(oldpgdir);

  top = USRSTACK;
  envp_at = (top - envbytes - (uintptr_t)(envc + 1) * sizeof(uintptr_t)) & ~(uintptr_t)7;
  argv_at = (envp_at - argbytes - (uintptr_t)(argc + 1) * sizeof(uintptr_t)) & ~(uintptr_t)15;
  sp = argv_at - 4 * sizeof(uintptr_t);
  head[0] = 0xffffffffffffffff;
  head[1] = (uint64_t)argc;
  head[2] = argv_at;
  head[3] = envp_at;
  if ((envp != 0 &&
       vec_copyout(curproc->p_p->pgdir, envp, envc, top - envbytes, envp_at) != 0) ||
      vec_copyout(curproc->p_p->pgdir, argv, argc,
                  argv_at + (uintptr_t)(argc + 1) * sizeof(uintptr_t), argv_at) != 0 ||
      copyout(curproc->p_p->pgdir, sp, head, sizeof(head)) < 0)
    exit_signal(FNU_SIG_MEMORY);
  curproc->tf->rip = elf.entry;
  curproc->tf->rsp = sp;
  curproc->tcb = 0;
  wrmsr(MSR_FS_BASE, 0);
  fpureset(&curproc->fpu);
  if (curproc->p_p->traced) {
    curproc->p_p->pt_report = FNU_PTRACE_EXEC;
    curproc->p_p->pt_other = 0;
    sig_post(curproc, FNU_SIG_BREAK, 0);
  }
  return 0;

bad:
  if (seg != 0)
    free(seg, M_TEMP);
  vm_entries_free(image);
  if (pgdir)
    hat_free(pgdir);
  if (ip) {
    iunlockput(ip);
  }
  if (leave)
    thread_exit();
  return error;
}
