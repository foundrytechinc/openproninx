#ifndef PRONINX_X86_64_DEFS_H
#define PRONINX_X86_64_DEFS_H

#include "inc/error.h"
#include "inc/string.h"
#include "inc/types.h"
#include "mutex.h"

struct buf;
struct blockdev;
struct uio;
struct context;
struct cpu_info;
#include "font.h"
struct inode;
struct ioapic;
struct pipe;
struct proc;
struct fnu_procids;
struct process;
struct sleeplock;
struct fnu_siginfo;
struct fnu_exitinfo;
struct fnu_wsmouse_calib;
struct fnu_stat;
struct fnu_fsstat;
struct vfs_mount;
struct fnu_procinfo;
struct fnu_thread_start;
#define CALL_RESTART -1
#define CALL_JUSTRETURN -2

struct ufs2_inode;
struct ufs2_volume;
int inode_access(struct inode *, uint, int);
struct fnu_network_endpoint;
struct fnu_network_status;
struct network_ipv4_config;
struct fnu_user_info;

struct fnu_bootinfo;

// MACHINE

void bootinfo_init(uint64_t phys);
const struct fnu_bootinfo *bootinfo(void);
const char *kgetenv(const char *name);
int getenv_int(const char *name, int *data);
int getenv_flag(const char *name);
void ulog_overlay_read(uint dev, uint64_t blockno, uint nblk, uchar *data);
void ulog_overlay_write(uint dev, uint64_t blockno, uint nblk);
int bootinfo_is_ram(uint64_t base, uint64_t end);
void bootinfo_print(void);

void efirt_init(void);
int efirt_available(void);
void efirt_report(void);
int efirt_poweroff(void);
int efirt_reboot(void);

void *ioremap(uint64_t, uint64_t);
#define KMAP_CACHED 0
#define KMAP_UNCACHED 1
#define KMAP_COMBINING 2
uintptr_t kmap_alloc(uint64_t len);
void kmap_page(uintptr_t va, uint64_t pa, int write, int exec, int cache);
void kunmap_page(uintptr_t va);
int kva2pa(uintptr_t va, uint64_t *pa);
int hat_kmap(uintptr_t va, char *page);
char *hat_kunmap(uintptr_t va);
void *kheap_alloc(size_t bytes, size_t guard, int flags);
size_t kheap_free(void *p);
int kheap_owns(const void *p);
void kheap_init(void);
void kheap_reclaimer(void *arg);
void kmap_update(void);
void tlb_shootdown(void);
void tlb_flush_local(void);
void tlb_flush_ack(void);
void *ioremap_wc(uint64_t, uint64_t);

void smbios_init(void);
void smbios_sysctl(void);
const char *hw_vendor_name(void);

void mpinit(void);
void startothers(void);
void mp_startup_common(void) __dead;

extern volatile uint32_t *lapic;
void lapiceoi(void);
uint32_t lapic_introff(void);
void lapic_intron(uint32_t tpr);
int lapicid(void);
extern int x2apic_mode;
void lapicinit(void);
uint64_t lapic_timer_hz(void);
uint32_t lapic_timer_reload(void);
void lapic_halt_others(void);
void lapic_halt_to(uint32_t apicid);
void lapic_notify(uint32_t apicid);
void lapic_choose_mode(uint32_t maxid);
#define LINT_BY_UID 0
#define LINT_BY_APIC 1
void lapic_lint_nmi(int by, int id, int lint, int flags);
void lapic_x2apic_enable(void);
void lapic_tlb_others(void);
int lapicstartap(uint32_t, uint32_t);
int lapicstartap_all(uint32_t);

int mp_ioapic_each(void (*fn)(int id, uint64_t addr, int gsibase));
int ioapic_gsi_vector(int gsi, int cpu, int level, int low);
int ioapic_isa_vector(int irq, int cpu);
int intr_vector_alloc(void);
void intr_vector_free(int v);
int intr_is_device_vector(int v);
void lapic_error(void);
uint32_t lapic_esr(void);
void lapic_print_state(void);
void lapic_esr_check(const char *step);
void lapic_suspend(void);
int mp_pci_route(int bus, int device, int pin, int *intin, int *level, int *low);
void ioapicinit(void);

void i8259_init(void);
int pic_spurious(int);

struct fnu_hwinfo;
struct fnu_diskinfo;

void mcheck_init(void);
void mcheck_report_boot(void);
int mcheck_report(const char *);

void delay_init(void);
void lowmem_init(void);
uint64_t lowmem_alloc(int npages);
uint64_t lowmem_ceiling(void);
uint64_t lowmem_tables(void);
uint64_t lowmem_identity(void);
int64_t tsc_adjust_check(void);
void delay(uint us);
void delay_ms(uint ms);
uint64_t delay_tsc_hz(void);
int tsc_bracket(uint32_t (*read)(void), uint64_t *tsc, uint32_t *value);

struct fnu_timespec;
struct fnu_rtctime;
void clockinit(void);
void time_now(struct fnu_timespec *out);

void system_halt(int howto) __dead;
void system_halt_handoff(void);
void shutdown_panic(void);
void shutdown_sysctl(void);
void trap_sysctl(void);
void system_poweroff(void) __dead;
void system_reboot(void) __dead;
void machine_restart(void) __dead;

// MEMORY

char *page_alloc(void);
char *page_alloc_user(void);
extern int page_reserve_daemon, page_reserve_kernel;
extern struct proc *page_daemon;
char *kalloc_low(void);
void kalloc_freemap(uint8_t *map, uint64_t pages);
void kalloc_rebuild(const uint8_t *map, uint64_t pages);
int kalloc_claim(uint64_t pa, uint64_t npages);
void *dma_alloc(uint64_t bytes);
void dma_free(void *p);
void page_free(char *);
void page_init(void *vstart);
uint64_t msgbuf_reserved(void);
void page_init_rest(void);
void page_boot_tables_free(void);
uint64_t get_total_ram(void);
uint64_t get_free_ram(void);
void contig_setup(void);
void bufmem_region(char **base, uint64_t *size);
void *contig_alloc(uint64_t bytes);
void contig_lock_enable(void);
void contig_zero_free(void);
void pci_lock_enable(void);
void contig_free(void *p);
uint64_t contig_capacity(void);

int map_sigcode(pte_t *pgdir, uintptr_t va, const char *code, uint len);
int hat_memload(pte_t *pgdir, uintptr_t va, char *mem, pte_t perm);
uint64_t hat_unload(pte_t *pgdir, uintptr_t va, size_t len);
uint64_t hat_resident(pte_t *pgdir);
int hat_range_empty(pte_t *pgdir, uintptr_t va, size_t len);
int hat_chgprot(pte_t *pgdir, uintptr_t va, size_t len, int prot);
int copyout(pte_t *pgdir, uintptr_t va, void *p, size_t len);
int copyin(pte_t *pgdir, void *p, uintptr_t va, size_t len);
int copyinstr(pte_t *pgdir, char *buf, uintptr_t va, size_t len, size_t *done);
int uva_prefault(pte_t *pgdir, uintptr_t va, int write);
int uva_range_ok(pte_t *pgdir, uintptr_t va, size_t len);
void hat_free(pte_t *pgdir);
void hat_kern_setup(void);
void gdt_init(void);
pte_t *hat_alloc(void);
void hat_switch(pte_t *pgdir);
void tss_kstack(struct proc *p);
int map_range(pte_t *, uintptr_t, uintptr_t, uint64_t, pte_t);

#define WALK_NOSLEEP 2 // hat_pte's alloc: a table only if one is free now
pte_t *hat_pte(pte_t *pgdir, const void *va, int alloc);
int kalloc_nfree(void);
struct proc *proc_of_pgdir(pte_t *pgdir);
int proc_swap_next(pid_t *hand, struct proc **out);
void proc_swap_drop(struct proc *p);
void proc_swap_freeze(struct proc *p);
void proc_swap_thaw(struct proc *p);

void memdevinit(void);
void memdevattach(void);
void console_flush(void);
int cngets(char *buf, int size);
void console_redraw(void);
void console_drop_lock(void);
int log_enter(void);
void tty_devname(int minor, char *out, size_t n);
pid_t tty_foreground(int minor);
int64_t tty_inputat(int minor);
void log_leave(int locking);
void console_flush_if_dirty(void);
void console_refresh_thread(void *);

// PROCESSES

extern pte_t *kernel_pgdir;
int kthread_create(void (*func)(void *), void *arg, struct proc **newpp,
                   const char *name);
void kthread_exit(int ret) __dead;
void sched_peg_curproc(struct cpu_info *ci);
void sched_unpeg_curproc(void);
void exit(void);
pid_t fork(void);
struct cpu_info *curcpu(void);
void percpu_attach(void);
int cpuid(void);
extern int nx_ready;
pte_t vm_nx(void);
void nxinit(void);
struct proc *proc_self(void);
#define curproc (proc_self())
void sched(void);
void scheduler(void) __dead;
#define INFSLP (~(uint64_t)0)
void sleep_setup(const volatile void *ident, int prio, const char *wmesg);
int sleep_finish(uint64_t nsecs, int do_sleep);
int msleep(void *chan, struct mutex *lk, int pri, const char *wmesg, int timo);
int tsleep(void *chan, int pri, const char *wmesg, int timo);
int tsleep_nsec(void *chan, int pri, const char *wmesg, uint64_t nsecs);
int msleep_nsec(void *chan, struct mutex *lk, int pri, const char *wmesg,
                uint64_t nsecs);
void wakeup_one(const volatile void *chan);
void create_init(void);
pid_t waitpid(pid_t target, struct fnu_exitinfo *out, int flags);
void wakeup(const volatile void *chan);
#include "selinfo.h"
void selscan_begin(void);
void selscan_end(void);
uint selscan_gen(void);
uint64_t selsleep(uint seen, uint64_t left, int forever);
struct file;
int file_ready(struct file *f, int want);
struct eventq;
struct knote;
void eventinit(void);
int eventq_ready(struct eventq *eq);
void eventq_close(struct eventq *eq);
int eventq_count(struct eventq *eq);
void knote_inode(struct inode *ip, uint32_t hint);
void knote_proc(struct process *pr, uint32_t hint, int64_t status);
void knote_signal(struct process *pr, int sig);
int process_with(pid_t pid, int (*fn)(struct process *, void *), void *arg);
struct pipe;
int pipe_ready(struct pipe *p, int want);
int64_t pipe_avail(struct pipe *p, int write, int *eof);

// msgbuf.c
void msgbuf_putc(int c);
void consbuf_put(const char *s, int n);
int msgbuf_read(char *dst, uint64_t off, int n);
uint64_t msgbuf_bytes(void);
void msgbuf_attach(uint64_t phys);
void msgbuf_init(void);
void msgbuf_dump(int panicked);
void msgbuf_restore(void);
extern const char *panicstr;
extern struct proc *initprocess;
typedef uint32_t fd_mask;
#define NFDBITS 32
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#define FD_SET(n, p) ((p)[(n) / NFDBITS] |= (1u << ((n) % NFDBITS)))
#define FD_ISSET(n, p) (((p)[(n) / NFDBITS] & (1u << ((n) % NFDBITS))) != 0)
struct fnu_ready;
int fnu_ready_wait(struct fnu_ready *list, int n, uint64_t left, int forever);
int kill(pid_t pid, int sig);
// kern_descrip.c
void fd_init(struct process *pr);
void fd_reset(struct process *pr);
struct file *fd_getfile(struct process *pr, int fd);
int fd_alloc_at(struct process *pr, struct file *f, int min, int flags);
int fd_set(struct process *pr, int fd, struct file *f, int flags,
           struct file **old);
struct file *fd_take(struct process *pr, int fd);
void fd_untake(struct process *pr, int fd, struct file *f);
int fd_getflags(struct process *pr, int fd);
int fd_setflags(struct process *pr, int fd, int set, int clear);
int fd_copy(struct process *from, struct process *to);
void fd_closeall(struct process *pr, int exec_only);
int at_dir(int fd, struct inode **dp);
struct inode *fd_cwd(struct process *pr);
void fd_setcwd(struct process *pr, struct inode *ip);
int kill_perm(pid_t pid, int sig);
pid_t setsid(void);
int setpgid(pid_t pid, pid_t pgid);
int proc_ids(pid_t pid, struct fnu_procids *out);
pid_t pgrp_session(pid_t pgid);
struct fnu_timer;
void hardclock(int usermode);
int setitimer(int which, const struct fnu_timer *in, struct fnu_timer *out);
struct fnu_limit;
struct fnu_usage;
int setrlimit(int which, const struct fnu_limit *in, struct fnu_limit *out);
int proc_rusage(int who, struct fnu_usage *out);
void ruadd(struct process *parent, struct process *child);
int proc_niceness(int whom, int id, int how, int *value);
int proc_umask(int mask);
int proc_uid_of(pid_t pid, uint *uid);
int groupmember(uint gid);
int signal_take(uint64_t set, struct fnu_siginfo *out, int block);
int kern_sigsuspend(uint64_t mask);
int proc_interrupted(void);
int proc_trace_stop(int sig);
struct proc *proc_hold(pid_t pid);
void proc_release(void);
void proc_pin(struct proc *p);
void proc_unpin(struct proc *p);
void proc_lock(void);
void proc_unlock(void);
void proc_resume(struct proc *p);
void proc_trace_attach(struct proc *p, struct process *tracer);
void proc_trace_detach(struct proc *p);
void sig_post(struct proc *p, int sig, pid_t sender);
void sig_post_locked(struct proc *p, int sig, pid_t sender);
void sig_post_process(struct process *pr, int sig, pid_t sender);
uint64_t sig_pending_of(struct proc *p);
int sig_valid(int sig);
pid_t sig_forget(struct proc *p, int sig);
void proc_stop(int sig);
void proc_suspend(void);
void process_continue(struct process *pr, int report);
void exit_signal(int sig);
int thread_fork(const struct fnu_thread_start *st, pid_t *tidout);
void thread_exit(void) __dead;
int single_thread(struct proc *p);
void futex_init(void);
int futex_wake(struct proc *p, uintptr_t va, uint32_t n, int op);
void single_thread_clear(struct proc *p);

struct fnu_sigact;
struct trapframe;
void sigexec(struct proc *p);
int sigcode_map(pte_t *pgdir, uintptr_t va);
void sigcode_attach(struct proc *p, uintptr_t va);
void sig_deliver(struct trapframe *tf);
void trapsignal(struct proc *p, int sig, int32_t why, uint64_t addr);
int sig_for_trap(struct proc *p, const struct trapframe *tf, uintptr_t fva,
                 int pferr, int *why);
int sig_default_ignores(int sig);
int sig_stops(int sig);
int sig_continues(int sig);
int kern_sigreturn(uintptr_t scp);
int kern_sigact(int sig, const struct fnu_sigact *act, struct fnu_sigact *old);
struct fnu_altstack;
int kern_sigstack(const struct fnu_altstack *in, struct fnu_altstack *out);
uint64_t kern_sigmask(uint64_t block, uint64_t unblock);
void yield(int voluntary);
void pinit(void);
int64_t get_nprocs(void);
int procinfo_all(struct fnu_procinfo *out, int n);
int64_t get_nthreads(void);
void proc_setruid(struct process *pr, uint ruid);

int exec(char *, char **, char **);
int exec_user(uintptr_t upath, uintptr_t uargv, uintptr_t uenvp);

void dbregs_load(const uint64_t *dr);
void dbregs_clear(void);
uint64_t dbregs_status(void);

void fpuinit(void);

void swtch(struct context **, struct context *);

void acquiresleep(struct sleeplock *);
int trysleep(struct sleeplock *);
void releasesleep(struct sleeplock *);
int holdingsleep(struct sleeplock *);
void initsleeplock(struct sleeplock *, char *);

int rdmsr_safe(uint32_t msr, uint64_t *val);
void mtrr_save(void);
void mtrr_load(void);
int wrmsr_safe(uint32_t msr, uint64_t val);
int64_t smi_count(void);
extern volatile uint64_t console_said;
extern const char *volatile boot_step;
void lockup_start(void);
void lockup_stop(void);
int lockup_nmi(struct trapframe *);
void hwlat_report(const char *);
void lapic_pmi_nmi(int);
void mtrr_check_ram(void);
void kernel_text_init(void);
int kernel_text_check(const char *when, uint64_t *first);
void kernel_text_sysctl(void);
uint32_t kernel_text_crc(void);
void spinlock_enter(void);
void spinlock_exit(void);

int pledge_check(struct proc *p, int num);
int pledge_need(struct proc *p, uint64_t bits, int num);
int pledge_need_all(struct proc *p, uint64_t bits, int num);
extern const char *const syscallnames[];
void pledge_fail(struct proc *p, int num);
int pledge_socket(struct proc *p, int domain);
int pledge_passfd(struct proc *p, struct file *f, int recv);
int contig_owns(void *p);
#include "inc/stdarg.h"
int kvprintf(const char *fmt, void (*func)(int, void *), void *arg,
             va_list ap) __printflike(1, 0);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
    __printflike(3, 0);
int snprintf(char *buf, size_t size, const char *fmt, ...) __printflike(3, 4);
void sysctl_init(void);
int sysctl_add_opaque(const char *name, uint64_t (*length)(void *),
                      int (*readat)(void *, char *, uint64_t, int), void *arg,
                      int flags, uint64_t need);
int sysctl_add_quad_fn(const char *name, int64_t (*get)(void *), void *arg);
int sysctl_add_string_fn(const char *name, int (*gets)(void *, char *, size_t),
                         void *arg, uint64_t need);
void intr_init(void);
int intr_establish(int vector, int (*fn)(void *arg), void *arg, const char *name);
int intr_dispatch(int vector);
struct proc;
void lend_priority(struct proc *owner, struct proc *waiter);
void unlend_priority(struct proc *owner);
int sleeplock_lends(void);
int sysctl_add_int(const char *name, int *var, int flags, int lo, int hi);
int sysctl_add_string(const char *name, char *var, size_t size, int flags);
int sysctl_on_write(const char *name, int (*fn)(void *arg, int old), void *arg);
int sysctl_on_read(const char *name, void (*fn)(void *arg), void *arg);
struct fnu_hwinfo;
void hwinfo_fill(struct fnu_hwinfo *hw);
// pm_trace steps after the wake code's own, then the way down
enum { PMT_KERNEL = 3, PMT_CPU, PMT_CLOCK, PMT_VBESTATE, PMT_CONSOLE, PMT_WAK,
       PMT_CPUS, PMT_INPUT, PMT_PCI, PMT_PRT, PMT_DRIVERS, PMT_USB,
       PMT_DONE, PMT_S_VIDEO, PMT_S_USB, PMT_S_DRIVERS, PMT_S_PCI,
       PMT_S_PARK, PMT_S_PTS, PMT_S_WAKERS, PMT_S_CLOCK, PMT_S_FREEZE, PMT_S_IO, PMT_EMUPOST };
void pm_trace_step(int step, int sub);
void pm_trace_name_driver(void);
const char *driver_device_name(int index);
uint64_t cpu_bus_clock(void);
int pipe_nbig(void);
int pipe_maxbig(void);
void syscall(void);

void idtinit(void);
void tvinit(void);
extern volatile uint64_t ticks;
int ppsratecheck(uint64_t *last, int *curpps, int maxpps);
extern int maxusers, maxproc, maxprocperuid, maxfiles, maxfilesperproc;
extern int maxfilesrootres, maxthread, ncallout, maxvnodes;
extern int max_threads_per_proc;
void init_param2(void);
void param_sysctl(void);

// FILES

struct file *filealloc(void);
int fdalloc(struct file *f);
int fd_limit(void);
void fileclose(struct file *);
struct file *filedup(struct file *);
int file_refs(struct file *f);
void fileinit(void);
int fileread(struct file *, char *, int n);
int fileread_uio(struct file *, struct uio *, uint64_t *);
int filewrite_uio(struct file *, struct uio *, uint64_t *);
uint file_device(struct file *);
int fileread_user(struct file *, uintptr_t, int);
int filewrite_user(struct file *, uintptr_t, int);
int fileread_off(struct file *, char *, int, uint64_t *);
int filewrite_off(struct file *, char *, int, uint64_t *);
int fileread_user_off(struct file *, uintptr_t, int, uint64_t *);
int filewrite_user_off(struct file *, uintptr_t, int, uint64_t *);
int filestat(struct file *, struct fnu_stat *);
int filewrite(struct file *, char *, int n);

int fs_dev_busy(uint dev);
void fs_forget_dev(uint dev);
int fs_dev_writers(uint dev);
void flock_release(struct file *f);
void fs_reload_dev(uint dev);
void bio_forget_dev(uint dev);
void bio_hibernate(void);
int bio_flush(void);
int bio_flush_device(uint dev);
struct inode *dirlookup(struct inode *, char *);
int name_is_dot(const char *);
struct inode *idup(struct inode *);
extern uint rootdev;
void iinit(void);
int fs_mountroot(uint dev, const char *type, int readonly, int nosuid);
struct inode *iget(uint dev, uint inum);
void iupdate_inum(struct inode *ip, uint inum);
void ilock(struct inode *);
void iput(struct inode *);
void iunlock(struct inode *);
void iunlockput(struct inode *);
void iupdate(struct inode *);
void fs_rename_lock(void);
void fs_rename_unlock(void);
void itimes(struct inode *ip);
int dirread(struct inode *dp, uint off, uint *next, uint *inum, char *name,
            int namesz, uchar *dtype);
int namei(char *, struct inode **);
int namei_at(struct inode *, char *, int, struct inode **);
int namei_nofollow_at(struct inode *, char *, int, struct inode **);
int nameiparent_at(struct inode *, char *, char *, int, struct inode **);
struct inode *inode_dotdot(struct inode *ip);
struct inode *mount_enter(struct inode *ip);
int inode_path(struct inode *, char *, uint);
int readi(struct inode *, char *, uint64_t, uint);
int readi_uio(struct inode *, struct uio *);
void stati(struct inode *, struct fnu_stat *);
int writei(struct inode *, char *, uint64_t, uint);
int writei_uio(struct inode *, struct uio *);


void binit(void);
struct buf *bread(uint dev, uint64_t blockno);
struct buf *bread_size(uint dev, uint64_t blockno, uint size);
struct buf *bgetblk_size(uint dev, uint64_t blockno, uint size);
struct buf *bread_file(uint dev, uint64_t owner, uint64_t lblkno,
                       uint64_t blockno, uint size);
struct buf *bgetblk_file(uint dev, uint64_t owner, uint64_t lblkno,
                         uint64_t blockno, uint size);
int bio_regrow(struct buf *b, uint64_t blockno, uint size);
void bio_forget_file(uint dev, uint64_t owner, uint64_t from);
void bio_forget_mount(uint dev, uint owner_high);
void bio_forget_run(uint dev, uint64_t blockno, uint nblk);
int bio_read_run(uint dev, uint64_t blockno, uint nblk, uchar *dst);
int bio_write_run(uint dev, uint64_t blockno, uint nblk, const uchar *src);
uint bio_run_max(void);
uint64_t bio_capacity(void);
struct buf *bgetblk(uint dev, uint64_t blockno);
void bwrite(struct buf *b);
void bdwrite(struct buf *b);
void bdirty(struct buf *b);
struct bpinlist;
void bpin(struct buf *b, struct bpinlist *l);
void bunpin(struct buf *b);
void bawrite(struct buf *b);
void cluster_write(struct buf *b);
void bio_syncer(void *);
void brelse(struct buf *b);

void vfs_init(void);
void vfs_attach_boot(void);

void devfs_init(void);

int readlinki(struct inode *ip, char *buf, int size);

int pipe_bufsize(void);
void pipeinit(void);
struct fnu_stat;
void pipe_stat(struct pipe *, struct fnu_stat *);
int pipe_nread(struct pipe *);
void fifoinit(void);
int fifo_open(uint dev, uint inum, int readable, int writable, int nonblock,
              struct pipe **out);
void fifo_close(struct pipe *p, int readable, int writable);
int pipealloc(struct file **, struct file **);
void pipeclose(struct pipe *, int);
int piperead(struct pipe *, struct uio *, int nonblock);
int pipewrite(struct pipe *, struct uio *, int);
int piperead_timo(struct pipe *, struct uio *, int nonblock, int timo);
int pipewrite_timo(struct pipe *, struct uio *, int nonblock, int timo);
struct pipe *pipe_create(void);


void randominit(void);
void randomdevinit(void);
void random_bytes(void *dst, uint n);
void random_add(const void *data, uint n);
uint64_t siphash24(const uchar key[16], const void *data, uint len);

// STORAGE

int blockdev_read(const struct blockdev *, uint64_t, void *, uint);
int blockdev_write(const struct blockdev *, uint64_t, const void *, uint);

uint64_t blockdev_device_size(uint device);
void blockdev_rw(struct buf *b);
uint blockdev_maxphys(uint device);

void partition_init(void);

int storage_recognise(uint dev, const char *type, struct vfs_mount *m);
int storage_probe_ufs2(uint device, struct ufs2_volume *volume);
void storage_mountroot(uint ramdisk);
struct root_hold {
  struct root_hold *next;
  const char *who;
  int held;
};
void root_mount_hold(struct root_hold *h, const char *who);
void root_mount_rel(struct root_hold *h);
void root_mount_published(void);
void cpu_park_others(void);
int proc_freeze_all(void);
void proc_thaw_all(void);
uint storage_swap_device(void);


void pciide_driver_init(void);
int wdc_intr(int channel);
void wd_strategy(struct buf *b);
uint64_t ide_size(uint device);

void ahci_driver_init(void);
uint64_t ahci_size(uint unit);
void ahcirw(struct buf *b);

void nvme_driver_init(void);
uint64_t nvme_size(uint ns_id);
void nvmerw(struct buf *b);

void ramdisk_init(void);
uint64_t ramdisk_size(void);
int ramdisk_available(void);
void ramdiskrw(struct buf *b);

void hotpluginit(void);
void hotplug_attach(int devclass, const char *name, uint flags, uint dev);
void hotplug_detach(int devclass, const char *name, uint flags, uint dev);

// CONSOLE

void consoleinit(void);
void console_enable_input(void);
int printf(const char *fmt, ...) __printflike(1, 2);
int vprintf(const char *fmt, va_list ap) __printflike(1, 0);
void panic(const char *fmt, ...) __dead __printflike(1, 2);
int consoleioctl(struct inode *, uint64_t, uint64_t);
void console_mouse_event(int type, int x, int y, int event, int cnt);
void console_mouse_show(int show);
void console_mouse_move(int x, int y, int relative);
void console_mouse_info(int *x, int *y, int *buttons);
void sysmouseinit(void);
pid_t console_leave_session(pid_t sid);
void tty_pgsignal(pid_t pgid, pid_t sid, int sig);
int proc_sig_masked(struct proc *p, int sig);
int proc_pgrp_orphaned(struct process *pr);
void pgrp_status(pid_t pgid, char *buf, size_t n);
int kern_open_file(struct proc *p, struct inode *start, char *path, int omode,
                   int mode, struct file **fp);
int kern_open(struct proc *p, struct inode *start, char *path, int omode,
              int mode, register_t *retval);
int session_tty(struct process *pr);
int session_take_tty(struct process *pr, int tty);

struct fnu_ttymode;
void com_init(void);
void uartintr_enable(void);
void uartintr(void);
void comcnputc(int);
int comcncheckc(void);
void uart_announce(void);
int uart_ports(void);
int uart_console_speed(void);
void uart_tty_putc(int unit, int c);
int uart_tx_full(int unit);
int uart_outq(int unit);
void *uart_tx_chan(int unit);
void uart_drain(int unit);
int uart_param(int unit, const struct fnu_ttymode *t);
int uart_ioctl(int unit, uint64_t cmd, int *val);
void console_serial_input(int unit, const int *chars, int n);
void console_serial_carrier(int unit, int on);
void console_serial_txdone(int unit);

uint64_t framebuffer_phys(void);
uint64_t framebuffer_size(void);
uint64_t framebuffer_vram(void);
void *framebuffer_virt(void);
int framebuffer_available(void);
uint64_t framebuffer_boot_physical_base(void);
uint framebuffer_boot_width(void);
uint framebuffer_boot_height(void);
uint framebuffer_boot_pitch(void);
uint framebuffer_boot_bits_per_pixel(void);
int framebuffer_initialization_error(void);
uint framebuffer_width(void);
uint framebuffer_height(void);
const char *framebuffer_font_name(void);
uint framebuffer_columns(void);
uint framebuffer_rows(void);
uint framebuffer_cell_width(void);
uint framebuffer_cell_height(void);
void framebuffer_pointer(int x, int y, int shown);
void framebuffer_init(void);
void framebuffer_draw_cell(int, vtcell_t);
void framebuffer_draw_row(uint, const vtcell_t *);
void framebuffer_redraw_cells(const vtcell_t *, int);
void framebuffer_flush_cells(const vtcell_t *, int, int);
void framebuffer_scroll_up(void);
void framebuffer_scroll_lines(int, vtcell_t);
void framebuffer_clear(void);
int framebuffer_has_dispi(void);
const char *framebuffer_mode_source(void);
uint framebuffer_mode_source_code(void);
void framebuffer_init_gpu(void *, uint, uint, uint);
void framebuffer_shadow_init(void);
void console_switch_to_gpu(void);
int display_set_resolution(uint w, uint h);
int display_can_modeset(void);
int display_resume_bios(void);
const char *display_resume_how(void);
int acpi_idle_key(void);
void cpu_idle(volatile int *watch);
void idle_sysctl(void);
int display_back_after_s3(void);
void display_get_resolution(uint *w, uint *h, uint *bpp);
struct fnu_video_info;
int display_mode_current(void);
int display_mode_info(int mode, struct fnu_video_info *vi);
int display_mode_find(uint w, uint h);
int display_mode_set(int mode);

void atkbdinit(void);
void atkbd_intr_locked(void);
int atkbd_cncheckc(void);
int ukbd_cncheckc(void);
int cncheckc(void);
void kbdc_intr_locked(void);

void wsmouseinit(void);
void wsmouse_input(int unit, int dx, int dy, int dz, uint32 buttons);
void wsmouse_attach(int unit);
const char *wsmouse_name(int unit);
int wsmouse_new(void);
void wsmouse_release(int unit);
void wsmouse_set_hw(int unit, uint32 type, const struct fnu_wsmouse_calib *calib);
void wsmouse_touch(int unit, int x, int y, int p, int w, int contacts, uint32 buttons);

// PSM
void psminit(void);
void psm_intr_locked(void);
void wsmouse_detach(int unit);

void uaudioinit(void);
void uaudio_init(void);
void uptime_now(struct fnu_timespec *out);
int64_t uptime_ns(void);
void uptime_init(void);
int64_t time_second(void);
void clock_secs_to_ymdhms(int64_t secs, struct fnu_rtctime *dt);
int64_t clock_ymdhms_to_secs(const struct fnu_rtctime *dt);

// SOCKETS

struct socket;
struct sockaddr_any;
void socketinit(void);
int socreate(int domain, int type, int proto, struct socket **out);
void soclose(struct socket *so);
struct sorights;
int sosend(struct socket *so, struct uio *uio, const struct sockaddr_any *to,
           struct sorights *rights, int flags);
int soreceive(struct socket *so, struct uio *uio, struct sockaddr_any *from,
              struct sorights **rights, int *flagsp);
int soready(struct socket *so, int want);
int64_t soavail(struct socket *so, int write, int *eof);
void so_stat(struct socket *so, struct fnu_stat *st);
struct inode *vn_mksock(char *path, int *error);

// DRIVERS

void driver_framework_init(void);
int driver_attach_pci_devices(void);
void driver_shutdown_all(void);
int driver_device_count(void);
int driver_pci_function_count(void);
void e1000_driver_init(void);
void virtio_net_driver_init(void);
void re_driver_init(void);
int virtio_gpu_driver_init(void);

void usbd_init(void);
void usbd_explore_thread(void *);
void xhci_driver_init(void);
void ehci_driver_init(void);
void uhub_init(void);
void hid_init(void);
void ukbd_init(void);
void ums_init(void);
void umass_init(void);
struct buf;
void umassrw(struct buf *b);

#endif
