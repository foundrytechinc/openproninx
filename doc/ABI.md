# OpenProninx System Call ABI Reference

> **Architecture:** x86-64 (`int $0x30` — `T_SYSCALL`)  
> **Calling convention:** syscall number in `%rax`, arguments in `%rdi %rsi %rdx %rcx %r8 %r9`, return value in `%rax`, `-1` on error.

---

## Tier 1 — Stable Core ABI

Numbers, signatures, and semantics are **frozen**. Guaranteed backward compatibility across all OpenProninx releases. Binary programs linked against these syscalls will continue to work unchanged.

### Process Management

| # | Name | Userland signature | Notes |
|---|------|--------------------|-------|
| 0 | `exit` | `void exit(void)` | Terminate process |
| 5 | `fork` | `int fork(void)` | Clone process; returns 0 in child, child PID in parent |
| 6 | `wait` | `int wait(void)` | Wait for any child; returns child PID |
| 27 | `waitpid` | `int waitpid(pid_t pid, int flags)` | Wait for specific child; supports `WNOHANG` (1) |
| 7 | `kill` | `int kill(pid_t pid)` | Send SIGKILL to process; root & wheel can signal any process, ordinary users only own UID |
| 8 | `exec` | `int exec(char *path, char **argv)` | Replace process image; last argv must be NULL |
| 25 | `getpid` | `int getpid(void)` | Return current process ID |
| 26 | `sleep` | `int sleep(int seconds)` | Sleep for *n* seconds (calibrated via kernel `HZ`) |

### Signals & Machine State

| # | Name | Userland signature | Notes |
|---|------|--------------------|-------|
| 58 | `signal` | `int signal(pid_t pid, int sig)` | Send signal (`SIGHUP`, `SIGINT`, `SIGKILL`, `SIGTERM`, `SIGUSR1`, `SIGUSR2`); pid `-1` broadcasts |
| 59 | `sigcatch` | `int sigcatch(uint32_t mask)` | Register signal mask to intercept and catch in userland |
| 60 | `sigwait` | `int sigwait(struct siginfo *out, int block)` | Retrieve pending signal with PCATCH semantics |
| 61 | `halt` | `int halt(int howto)` | Request system shutdown (`FNU_RB_HALT`, `FNU_RB_POWEROFF`, `FNU_RB_REBOOT`, `FNU_RB_FORCE`) |

### File System & I/O

| # | Name | Userland signature | Notes |
|---|------|--------------------|-------|
| 1 | `read` | `ssize_t read(int fd, void *buf, size_t n)` | Read from file descriptor |
| 2 | `write` | `ssize_t write(int fd, const void *buf, size_t n)` | Write to file descriptor |
| 3 | `open` | `int open(const char *path, int flags)` | Open file; flags: `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREATE` |
| 4 | `close` | `int close(int fd)` | Close file descriptor |
| 12 | `stat` | `int stat(int fd, struct stat *buf)` | Get file metadata by fd |
| 13 | `pipe` | `int pipe(int pipefd[2])` | Create unidirectional pipe |
| 17 | `ioctl` | `int ioctl(int fd, uint64_t cmd, uint64_t arg)` | Terminal/framebuffer control: `TCGETS`/`TCSETS`, `FBIOGET_VSCREENINFO`/`FBIOPUT_VSCREENINFO`, `FBIOGET_MODELIST`/`FBIODUMPMODES` |
| 18 | `getdents` | `int getdents(int fd, struct linux_dirent64 *buf, int count)` | Read directory entries |
| 19 | `link` | `int link(const char *old, const char *new)` | Create hard link |
| 20 | `mkdir` | `int mkdir(const char *path)` | Create directory |
| 21 | `unlink` | `int unlink(const char *path)` | Remove file or empty directory |
| 22 | `dup` | `int dup(int oldfd)` | Duplicate file descriptor |
| 23 | `mknod` | `int mknod(const char *path, int major, int minor)` | Create device node (supports `/dev/ttyN`, `/dev/console`, etc.) |
| 24 | `chdir` | `int chdir(const char *path)` | Change current directory |
| 64 | `getcwd` | `int getcwd(char *buf, int size)` | Get absolute path of current working directory |
| 53 | `chmod` | `int chmod(const char *path, int mode)` | Change permission bits (octal `07777`); owner or root only |
| 54 | `chown` | `int chown(const char *path, int uid, int gid)` | Change file owner/group; root only |

### Process Security & Terminal Control

| # | Name | Userland signature | Notes |
|---|------|--------------------|-------|
| 45 | `getuid` | `int getuid(void)` | Return current process UID |
| 48 | `setuid` | `int setuid(int uid)` | Set process UID to 0 (root); requires root or `doas_auth` grant |
| 52 | `setforeground` | `int setforeground(pid_t pid)` | Set foreground PID for console signal dispatch (TTY group leader) |

### Memory Management

| # | Name | Userland signature | Notes |
|---|------|--------------------|-------|
| 9 | `sbrk` | `void *sbrk(intptr_t increment)` | Grow/shrink process data segment |
| 10 | `malloc` | `void *malloc(size_t size)` | Kernel-assisted heap allocation (grows address space) |
| 11 | `free` | `void free(void *ptr)` | No-op; preserved for ABI compatibility |

### System Information & Hardware Inventory

| # | Name | Userland signature | Notes |
|---|------|--------------------|-------|
| 14 | `info` | `int info(struct info *buf)` | Fill `struct info` with uptime, RAM stats, process count |
| 15 | `reboot` | `int reboot(void)` | System reboot via `init` ladder (or keyboard/ACPI/EFI reset) |
| 16 | `procinfo` | `int procinfo(struct procinfo *buf, int capacity)` | Fill array of `struct procinfo`; returns count |
| 55 | `poweroff` | `int poweroff(void)` | ACPI/EFI shutdown via `init` ladder |
| 62 | `hwinfo` | `int hwinfo(struct hwinfo *buf)` | Fill hardware inventory: CPU brand, cores, frequencies, VRAM, PCI stats |
| 63 | `diskinfo` | `int diskinfo(struct diskinfo *buf, int capacity)` | Fill block device info: model, serial, sector size, SATA/NVMe kind, sectors |

### Network Subsystem (lwIP slot-handle API)

> **Design note:** Uses integer *slot handles* (not POSIX file descriptors).

| # | Name | Userland signature | Notes |
|---|------|--------------------|-------|
| 28 | `ping` | `int ping(struct network_ping_request *)` | ICMP echo; fills `round_trip_ms` on success |
| 29 | `udp_open` | `int udp_open(void)` | Allocate a UDP slot handle |
| 30 | `udp_bind` | `int udp_bind(int handle, int port)` | Bind UDP slot to local port |
| 31 | `udp_sendto` | `int udp_sendto(int h, const void *buf, int n, const struct network_endpoint *)` | Send UDP datagram |
| 32 | `udp_recvfrom` | `int udp_recvfrom(int h, void *buf, int n, struct network_endpoint *, int timeout_ms)` | Receive UDP datagram |
| 33 | `udp_close` | `int udp_close(int handle)` | Release UDP slot |
| 34 | `netinfo` | `int netinfo(struct network_status *)` | Read primary network interface status |
| 35 | `netconfig` | `int netconfig(struct network_ipv4_config *)` | Apply static IP or restart DHCP |
| 36 | `tcp_open` | `int tcp_open(void)` | Allocate a TCP slot handle |
| 37 | `tcp_bind` | `int tcp_bind(int h, int port)` | Bind TCP slot to local port |
| 38 | `tcp_listen` | `int tcp_listen(int h)` | Mark slot as passive listener |
| 39 | `tcp_accept` | `int tcp_accept(int h, int timeout_ms)` | Accept incoming connection; returns new handle |
| 40 | `tcp_connect` | `int tcp_connect(int h, const struct network_endpoint *, int timeout_ms)` | Connect to remote endpoint |
| 41 | `tcp_send` | `int tcp_send(int h, const void *buf, int n)` | Send TCP data |
| 42 | `tcp_recv` | `int tcp_recv(int h, void *buf, int n, int timeout_ms)` | Receive TCP data |
| 43 | `tcp_close` | `int tcp_close(int handle)` | Close TCP connection and release slot |
| 44 | `dns_resolve` | `int dns_resolve(struct network_dns_request *)` | Resolve hostname to IPv4 address |

### System Time

| # | Name | Userland signature | Notes |
|---|------|--------------------|-------|
| 56 | `rtctime` | `int rtctime(struct rtctime *t)` | Read CMOS RTC; all fields binary (not BCD) |

### Storage Information

| # | Name | Userland signature | Notes |
|---|------|--------------------|-------|
| 57 | `df` | `int df(struct df_stat *buf)` | Fill up to `DF_ENTRIES_MAX` (4) entries; returns count |

### Stable ABI Data Structures (`inc/abi.h` & `inc/signal.h`)

```c
struct info        { uint64 uptime; uint64 total_ram; uint64 free_ram; uint32 nprocs; };
struct procinfo    { pointer_t pid; proc_state_t state; char name[16]; pointer_t memory_size; };
struct linux_dirent64 { uint64 d_ino; int64 d_off; unsigned short d_reclen;
                        unsigned char d_type; char d_name[]; };
struct termios     { uint32 c_iflag; uint32 c_oflag; uint32 c_cflag; uint32 c_lflag;
                     uint8 c_line; uint8 c_cc[19]; };
struct fb_var_screeninfo { uint32 xres; uint32 yres; uint32 bits_per_pixel; };
struct fb_modelist { uint32 count; struct { uint16 width, height, bpp; } mode[96]; };

struct network_ping_request { uint8 address[4]; uint32 timeout_ms; uint32 round_trip_ms; };
struct network_endpoint     { uint8 address[4]; uint16 port; };
struct network_ipv4_config  { uint8 address[4]; uint8 netmask[4]; uint8 gateway[4];
                              uint8 dns_server[4]; uint32 dhcp; };
struct network_dns_request  { char name[64]; uint8 address[4]; uint32 timeout_ms; };
struct network_status       { char interface_name[16]; uint8 hardware_address[6];
                              uint8 address[4]; uint8 gateway[4]; uint8 dns_server[4];
                              uint32 dhcp_bound; uint64 received_frames;
                              uint64 dropped_frames; uint64 transmitted_frames; };

struct rtctime { uint8 sec; uint8 min; uint8 hour; uint8 wday;
                 uint8 mday; uint8 mon; uint16 year; };

#define DF_ENTRIES_MAX 4
struct df_stat { char label[24]; char mount_point[32];
                 uint64 total_bytes; uint64 used_bytes; uint64 free_bytes;
                 uint8 is_read_only; uint8 is_present; uint8 _pad[6]; };

struct hwinfo {
  char     cpu_brand[49];
  char     cpu_vendor[13];
  uint8    pad0[2];
  uint32   cpu_count, cpu_family, cpu_model, cpu_stepping;
  uint64   tsc_hz, cpu_base_hz, cpu_max_hz;
  uint32   fb_width, fb_height, fb_bpp, fb_columns, fb_rows;
  uint32   pci_devices, pci_total, fb_modeset, fb_source, pad1;
  uint64   fb_vram, ram_total, ram_free;
};

struct diskinfo {
  char     model[41];
  char     serial[21];
  uint8    kind, pad0[1];
  uint32   sector_size, link_gen, pad1;
  uint64   sectors;
};

struct siginfo { int32_t signo; int32_t reserved; pid_t sender; };
```

---

## Tier 2 — Experimental / Non-ABI

These syscalls are **fully functional** but their numbers, signatures, or structures may change in a future release without prior notice.

### In-Kernel Authentication

> **Design note:** Password hashing (SHA-256), account storage, and credential verification currently reside inside the kernel (`kern/auth.c`). A future release will move these to userspace (PAM / `libcrypt` / `/etc/shadow`). The syscall numbers are reserved; do not rely on their semantics remaining stable.

| # | Name | Notes |
|---|------|-------|
| 46 | `login` | Authenticate `name`+`password`; sets process UID/GID on success |
| 47 | `doas_auth` | Grant temporary root-elevation token; verified by `setuid` |
| 49 | `useradd` | Create new account; root only |
| 50 | `passwd` | Change password for a user |
| 51 | `users` | Enumerate accounts by index into `struct user_info` |

---

## Syscall Number Quick Reference

```
 0  exit          1  read          2  write         3  open
 4  close         5  fork          6  wait          7  kill
 8  exec          9  sbrk         10  malloc        11  free
12  stat         13  pipe         14  info          15  reboot
16  procinfo     17  ioctl        18  getdents      19  link
20  mkdir        21  unlink       22  dup           23  mknod
24  chdir        25  getpid       26  sleep         27  waitpid
28  ping         29  udp_open     30  udp_bind      31  udp_sendto
32  udp_recvfrom 33  udp_close    34  netinfo       35  netconfig
36  tcp_open     37  tcp_bind     38  tcp_listen    39  tcp_accept
40  tcp_connect  41  tcp_send     42  tcp_recv      43  tcp_close
44  dns_resolve  45  getuid       46* login         47* doas_auth
48  setuid       49* useradd      50* passwd        51* users
52  setforeground 53 chmod        54  chown         55  poweroff
56  rtctime      57  df           58  signal        59  sigcatch
60  sigwait      61  halt         62  hwinfo        63  diskinfo
64  getcwd
```

> Total syscall count: **65** (`PRONINX_SYSCALL_COUNT`).  
> `*` = EXPERIMENTAL (Tier 2, 5 syscalls). All other 60 syscalls are **Stable Core ABI** (Tier 1).

---

*See also: [`inc/syscall.h`](../inc/syscall.h) (definitive table), [`inc/abi.h`](../inc/abi.h) (stable ABI data structures), [`inc/signal.h`](../inc/signal.h) (signal definitions).*
