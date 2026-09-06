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
int inode_access(struct inode *, uint, int);
struct network_ping_request;
struct network_endpoint;
struct network_status;
struct network_ipv4_config;
struct network_dns_request;
struct user_info;

// bio.c
void binit(void);
struct buf *bread(uint dev, uint blockno);
void bwrite(struct buf *b);
void brelse(struct buf *b);

// blockdev.c
int blockdev_read(const struct blockdev *, uint64_t, void *, uint);
int blockdev_write(const struct blockdev *, uint64_t, const void *, uint);

// ufs2.c
int ufs2_probe(const struct blockdev *, struct ufs2_volume *);
void ufs2_init(void);
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
void console_set_foreground(pid_t pid);
void console_flush(void);
void console_flush_if_dirty(void);

// framebuffer.c
uint32_t framebuffer_phys(void);
uint framebuffer_size(void);
void *framebuffer_virt(void);
int framebuffer_available(void);
uint32_t framebuffer_boot_tag(void);
uint32_t framebuffer_boot_physical_base(void);
uint framebuffer_boot_width(void);
uint framebuffer_boot_height(void);
uint framebuffer_boot_pitch(void);
uint framebuffer_boot_bits_per_pixel(void);
int framebuffer_initialization_error(void);
uint framebuffer_width(void);
uint framebuffer_height(void);
uint framebuffer_columns(void);
uint framebuffer_rows(void);
void framebuffer_init(void);
void framebuffer_draw_cell(int, ushort);
void framebuffer_draw_row(uint, const ushort *);
void framebuffer_redraw_cells(const ushort *, int);
void framebuffer_flush_cells(const ushort *, int);
void framebuffer_scroll_up(void);
void framebuffer_scroll_lines(int, ushort);
void framebuffer_clear(void);
int framebuffer_has_dispi(void);
void framebuffer_init_gpu(void *, uint, uint, uint);
void console_switch_to_gpu(void);
int display_set_resolution(uint w, uint h);
void display_get_resolution(uint *w, uint *h, uint *bpp);


// fnustate.c
void fnustateinit(void);
int fnustateread(struct inode *, char *, int);
int fnustatewrite(struct inode *, char *, int);

// driver.c & device drivers
void driver_framework_init(void);
int driver_attach_pci_devices(void);
int driver_dispatch_irq(int irq);
void driver_poll_all(void);
void e1000_driver_init(void);
void virtio_net_driver_init(void);
int virtio_gpu_driver_init(void);


// auth.c
void auth_init(void);
int auth_login(const char *, const char *, uint *, uint *);
int auth_doas(uint, const char *);
int auth_set_password(uint, const char *, const char *);
int auth_add_user(const char *, const char *, int);
int auth_user_info(int, struct user_info *);
int auth_is_wheel(uint);

// lwip.c
void proninx_lwip_init(void);
void proninx_lwip_timers(void);
int proninx_lwip_ping(struct network_ping_request *);
int proninx_udp_open(void);
int proninx_udp_bind(int, ushort);
int proninx_udp_sendto(int, const void *, uint, const struct network_endpoint *);
int proninx_udp_recvfrom(int, void *, uint, struct network_endpoint *, uint);
int proninx_udp_close(int);
void proninx_udp_process_exit(pid_t);
int proninx_lwip_status(struct network_status *);
int proninx_lwip_configure(const struct network_ipv4_config *);
int proninx_tcp_open(void);
int proninx_tcp_bind(int, ushort);
int proninx_tcp_listen(int);
int proninx_tcp_accept(int, uint);
int proninx_tcp_connect(int, const struct network_endpoint *, uint);
int proninx_tcp_send(int, const void *, uint);
int proninx_tcp_recv(int, void *, uint, uint);
int proninx_tcp_close(int);
void proninx_tcp_process_exit(pid_t);
int proninx_dns_resolve(struct network_dns_request *);

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
extern uint rootdev;
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

// ramdisk.c
void ramdisk_init(void);
uint64_t ramdisk_size(void);
int ramdisk_available(void);
void ramdiskrw(struct buf *b);

// ide.c
void ideinit(void);
void ideintr(int channel);
void iderw(struct buf *b);
uint64_t ide_size(uint device);

// ahci.c
void ahci_driver_init(void);
void ahci_init(void);
uint64_t ahci_size(uint port);
int ahci_read(uint port, uint64_t lba, uint count, void *dst);
int ahci_write(uint port, uint64_t lba, uint count, const void *src);
void ahcirw(struct buf *b);

// nvme.c
void nvme_driver_init(void);
void nvme_init(void);
uint64_t nvme_size(uint ns_id);
int nvme_read(uint ns_id, uint64_t lba, uint count, void *dst);
int nvme_write(uint ns_id, uint64_t lba, uint count, const void *src);
void nvmerw(struct buf *b);

// blockdev.c
uint64_t blockdev_device_size(uint device);
void blockdev_rw(struct buf *b);

// storage.c
void storageinit(void);
const struct ufs2_volume *storage_ufs2_volume(uint device);
int storage_mount_root(void);

// fs.c
int fs_root_readonly(void);

// ioapic.c
extern volatile struct ioapic *ioapic;
extern uint8_t ioapicid;

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
void lapicstartap(uchar, uint32_t);
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
void *gpu_alloc_backing(uint32_t, uint64_t *);

// acpi.c
int acpi_init(void);
void acpi_print_summary(void);
void *acpi_find_table(const char *signature);
int acpi_mp_init(void);
void acpi_poweroff(void) __attribute__((noreturn));
void acpi_reboot(void) __attribute__((noreturn));

// mp.c
void mpinit(void);
void startothers(void);
void mpmain(void) __attribute__((noreturn));

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
void *ioremap(uintptr_t, uint);

// number of elements in fixed-size array
#define NELEM(x) (sizeof(x) / sizeof((x)[0]))

#endif /* ifndef PRONINX_X86_64_DEFS_H */
