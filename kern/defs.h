#ifndef PRONINX_X86_64_DEFS_H
#define PRONINX_X86_64_DEFS_H

#include "inc/string.h"
#include "inc/types.h"

struct buf;
struct blockdev;
struct context;
struct cpu;
struct inode;
struct ioapic;
struct pipe;
struct proc;
struct sleeplock;
struct spinlock;
struct stat;
struct superblock;
struct procinfo;
struct ufs2_inode;
struct ufs2_volume;

// bio.c
void binit(void);
struct buf *bread(uint dev, uint blockno);
void bwrite(struct buf *b);
void brelse(struct buf *b);

// blockdev.c
int blockdev_read(const struct blockdev *, uint64_t, void *, uint);

// ufs2.c
int ufs2_probe(const struct blockdev *, struct ufs2_volume *);
int ufs2_read_inode(const struct ufs2_volume *, uint, struct ufs2_inode *);
int ufs2_read_direct(const struct ufs2_volume *, const struct ufs2_inode *,
                     uint, void *, uint);
int ufs2_lookup(const struct ufs2_volume *, uint, const char *, uint *);
int ufs2_read(const struct ufs2_volume *, const struct ufs2_inode *,
              uint64_t, void *, uint);
int ufs2_readdir(const struct ufs2_volume *, uint, uint, uint *, char *, uint);

// console.c
void consoleinit(void);
void consoleintr(int (*)(void));
void cprintf(char *, ...);
void panic(char *) __attribute__((noreturn));
int consoleioctl(struct inode *, uint64_t, uint64_t);

// fnustate.c
void fnustateinit(void);
int fnustateread(struct inode *, char *, int);
int fnustatewrite(struct inode *, char *, int);

// virtio_net.c
void virtio_net_init(void);
void virtio_net_intr(void);

// exec.c
int exec(char *, char **);

// file.c
struct file *filealloc(void);
void fileclose(struct file *);
struct file *filedup(struct file *);
void fileinit(void);
int fileread(struct file *, char *, int n);
int filestat(struct file *, struct stat *);
int filewrite(struct file *, char *, int n);

// fs.c
void readsb(int dev, struct superblock *sb);
int dirlink(struct inode *, char *, uint);
struct inode *dirlookup(struct inode *, char *, uint *);
struct inode *ialloc(uint, short);
struct inode *idup(struct inode *);
void iinit(int dev);
struct inode *iget(uint dev, uint inum);
void iupdate_inum(struct inode *ip, uint inum);
void ilock(struct inode *);
void iput(struct inode *);
void iunlock(struct inode *);
void iunlockput(struct inode *);
void iupdate(struct inode *);
int namecmp(const char *, const char *);
struct inode *namei(char *);
struct inode *nameiparent(char *, char *);
int readi(struct inode *, char *, uint, uint);
void stati(struct inode *, struct stat *);
int writei(struct inode *, char *, uint, uint);

// fat32.c
struct fat32_dirent;
void fat32_init(int dev);
int fat32_read_cluster(int dev, uint cluster, char *buf, uint n);
int fat32_read(int dev, uint start_cluster, char *dst, uint off, uint n);
int fat32_write(int dev, uint *start_cluster, char *src, uint off, uint n);
void fat32_iupdate(struct inode *ip);
uint fat32_lookup(int dev, uint dir_cluster, char *name, struct fat32_dirent *res, uint *off);
int fat32_dirlink(struct inode *dp, char *name, struct inode *ip);
int fat32_unlink(struct inode *dp, uint off);
int fat32_isdirempty(struct inode *dp);
struct inode* fat32_ialloc(uint dev, short type);

// ide.c
void ideinit(void);
void ideintr(int channel);
void iderw(struct buf *b);
uint64_t ide_size(uint device);

// storage.c
void storageinit(void);
const struct ufs2_volume *storage_ufs2_volume(uint device);
int storage_mount_root(void);

// fs.c
int fs_root_readonly(void);

// ioapic.c
extern volatile struct ioapic *ioapic;
extern uint8_t ioapicid;
void ioapicenable(int irq, int cpunum);
void ioapicinit(void);

// kbd.c
void kbdintr(void);

// lapic.c
extern volatile uint32_t *lapic;
void lapiceoi(void);
int lapicid(void);
void lapicinit(void);
void microdelay(int us);

// log.c
void initlog(int dev);
void initlog_readonly(void);
void log_write(struct buf *);
void begin_op();
void end_op();

// kalloc.c
char *kalloc(void);
void kfree(char *);
void kinit1(void *vstart);
void kinit2();
uint64_t get_total_ram(void);
uint64_t get_free_ram(void);

// mp.c
void mpinit(void);

// picirq.c
void picinit(void);

// pipe.c
int pipealloc(struct file **, struct file **);
void pipeclose(struct pipe *, int);
int piperead(struct pipe *, char *, int);
int pipewrite(struct pipe *, char *, int);

// proc.c
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wbuiltin-declaration-mismatch"
void exit(void);
#pragma GCC diagnostic pop
pid_t fork(void);
int growproc(int n);
struct cpu *mycpu(void);
int cpuid(void);
struct proc *myproc(void);
void sched(void);
void scheduler(void) __attribute__((noreturn));
void sleep(void *chan, struct spinlock *lk);
void userinit(void);
pid_t wait(void);
pid_t waitpid(pid_t target, int nohang);
void wakeup(void *chan);
int kill(pid_t pid);
void yield(void);
void pinit(void);
int64_t get_nprocs(void);
int procinfo_next(pid_t after, struct procinfo *out);
void procdump(void);

// sleeplock.c
void acquiresleep(struct sleeplock *);
void releasesleep(struct sleeplock *);
int holdingsleep(struct sleeplock *);
void initsleeplock(struct sleeplock *, char *);

// swtch.S
void swtch(struct context **, struct context *);

// spinlock.c
void acquire(struct spinlock *);
// void            getcallerpcs(void*, uint*);
int holding(struct spinlock *);
void initlock(struct spinlock *, char *);
void release(struct spinlock *);
void pushcli(void);
void popcli(void);

// syscall.c
int arg(int n, uint64_t *ip);
int argint(int n, int *ip);
int argptr(int n, char **pp, int size);
int argstr(int n, char **pp);
int fetchint(uintptr_t addr, uint64_t *ip);
int fetchstr(uintptr_t addr, char **pp);
void syscall(void);

// trap.c
void idtinit(void);
void tvinit(void);
extern uint ticks;
extern struct spinlock tickslock;

// uart.c
void uartinit(void);
void uartintr(void);
void uartputc(int);

// vm.c
int allocuvm(pte_t *pgdir, size_t oldsz, size_t newsz);
void clearpteu(pte_t *pgdir, char *uva);
int copyout(pte_t *pgdir, uintptr_t va, void *p, size_t len);
int copyin(pte_t *pgdir, void *p, uintptr_t va, size_t len);
pte_t *copyuvm(pte_t *, size_t);
int deallocuvm(pte_t *pgdir, size_t oldsz, size_t newsz);
void freevm(pte_t *pgdir, uintptr_t utop);
void inituvm(pte_t *pgdir, char *init, size_t sz);
void kvmalloc(void);
int loaduvm(pte_t *pgdir, char *addr, struct inode *ip, uint offset, size_t sz);
void seginit(void);
pte_t *setupkvm(void);
void switchkvm(void);
void switchuvm(struct proc *p);

// number of elements in fixed-size array
#define NELEM(x) (sizeof(x) / sizeof((x)[0]))

#endif /* ifndef PRONINX_X86_64_DEFS_H */
