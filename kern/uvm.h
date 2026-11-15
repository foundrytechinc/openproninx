#ifndef PRONINX_X86_64_UVM_H
#define PRONINX_X86_64_UVM_H

#include "inc/types.h"

struct inode;
struct proc;
struct process;

struct vm_page {
  struct vm_object *obj;
  uint64_t offset;
  char *addr;
  int refs;
  int dirty;
  struct vm_page *next;
};

struct vm_object {
  struct inode *ip;
  int refs;
  struct vm_page *pages;
  struct vm_object *link;
  int text;
  int wrefs;
};

struct vm_entry {
  uintptr_t start, end;
  struct vm_object *obj;
  uint64_t offset;
  int prot;
  int shared;
  int text; // of the program's image
  struct vm_entry *next;
};

#define PTE_CACHE 0x200
#define PTE_SWAPPED 0x400

#define MMAPBASE 0x400000000ull

void uvminit(void);
int vm_fault(struct proc *p, uintptr_t va, int write);
int vm_fault_pgdir(pte_t *pgdir, uintptr_t va, int write);
int vm_write_force(struct proc *p, uintptr_t va, const void *src, size_t len);
int vm_mmap(struct proc *p, uintptr_t addr, size_t len, int prot, int flags,
            struct inode *ip, uint64_t offset, uintptr_t *out);
int vm_munmap(struct proc *p, uintptr_t addr, size_t len);
uint64_t vm_size(struct process *pr);
int vm_msync(struct proc *p, uintptr_t addr, size_t len);
int vm_mprotect(struct proc *p, uintptr_t addr, size_t len, int prot);
int vm_madvise(struct proc *p, uintptr_t addr, size_t len, int advice);
int vm_fork(struct proc *parent, struct proc *child);
void vm_release(struct proc *p);
int vm_holds(struct proc *p, uintptr_t va);
int vm_text_busy(struct inode *ip);
int vm_write_mapped(struct inode *ip);
int vm_dev_write_mapped(uint dev);
void vm_futex_key(struct proc *p, uintptr_t va, int private, const void **key,
                  uint64_t *off);

struct vm_entry *vm_entry_text(struct inode *ip, uintptr_t start, size_t len,
                               uint64_t offset, int prot);
struct vm_entry *vm_entry_anon(uintptr_t start, size_t len, int prot);
void vm_entries_attach(struct proc *p, struct vm_entry *list);
void vm_entries_free(struct vm_entry *list);

int swapon(struct inode *ip);
int swapoff(void);
int swap_in(pte_t *pte, struct process *pr);
void swap_free_slot(pte_t pte);
int swap_hib_region(uint dev, uint64_t limit, uint64_t *lo, uint64_t *hi);
void swap_hib_hold(uint dev, uint64_t lo, uint64_t hi, int hold);
int swap_read_slot(pte_t pte, char *mem);
void pagedaemon(void *);
void pageout_wakeup(void);
char *page_alloc_wait(int user);
int page_wait(void);
void pageout_targets(int *min, int *targ);
int uvm_hibernate_shrink(int goal);
void swap_stats(uint64_t *total, uint64_t *used, uint64_t *outs, uint64_t *ins);

#endif
