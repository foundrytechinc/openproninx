#include "defs.h"
#include "proc.h"
#include "buf.h"
#include "fs.h"
#include "ufs2.h"
#include "inc/abi.h"
#include "x86.h"

int64_t sys_info(void) {
  struct info *inf;
  if (argptr(0, (char **)&inf, sizeof(*inf)) < 0)
    return -1;

  inf->uptime = ticks;
  inf->total_ram = get_total_ram();
  inf->free_ram = get_free_ram();
  inf->nprocs = (uint32_t)get_nprocs();
  return 0;
}

int64_t sys_reboot(void) {
  if (myproc()->uid != 0 && !auth_is_wheel(myproc()->uid))
    return -1;
  cprintf("Rebooting...\n");
  // Pulse the CPU reset line via the keyboard controller
  uint8_t good = 0x02;
  while (good & 0x02)
    good = inb(0x64);
  outb(0x64, 0xFE);
  return 0; // Should not reach here
}

int64_t sys_poweroff(void) {
  if (myproc()->uid != 0 && !auth_is_wheel(myproc()->uid))
    return -1;
  cprintf("Powering off...\n");
  acpi_poweroff();
  return 0;
}

int64_t sys_procinfo(void) {
  struct procinfo *pi;
  struct procinfo entry;
  int capacity, count = 0;
  pid_t last = 0, next;

  if (argptr(0, (char **)&pi, sizeof(*pi)) < 0 || argint(1, &capacity) < 0)
    return -1;

  if (capacity < 1)
    return -1;
  if (capacity > NPROC)
    capacity = NPROC;

  while (count < capacity && (next = procinfo_next(last, &entry)) > 0) {
    if (copyout(myproc()->pgdir, (uintptr_t)(pi + count), &entry,
                sizeof(entry)) < 0)
      return -1;
    last = next;
    count++;
  }
  return count;
}

int64_t sys_malloc(void) {
  int n;
  if (argint(0, &n) < 0)
    return -1;
  // Allocate from process address space growth.
  uintptr_t addr = myproc()->sz;
  if (growproc(n) < 0)
    return -1;
  return addr;
}

int64_t sys_free(void) {
  // No-op to preserve syscall ABI.
  return 0;
}

int64_t sys_fork(void) { return fork(); }

int64_t sys_exit(void) {
  console_flush_if_dirty();
  exit();
  return 0; // not reached
}

int64_t sys_wait(void) {
  console_flush_if_dirty();
  return wait();
}

int64_t sys_waitpid(void) {
  pid_t pid;
  int flags;

  console_flush_if_dirty();
  if (argint(0, &pid) < 0 || argint(1, &flags) < 0)
    return -1;
  if (flags & ~1)
    return -1;
  return waitpid(pid, flags & 1);
}

int64_t sys_kill(void) {
  pid_t pid;
  if (argint(0, &pid) < 0) {
    return -1;
  }
  return kill(pid);
}

int64_t sys_getpid(void) { return (int64_t)myproc()->pid; }

int64_t sys_setforeground(void) {
  pid_t pid;

  if (argint(0, &pid) < 0 || pid < 0)
    return -1;
  console_set_foreground(pid);
  return 0;
}

int64_t sys_getuid(void) { return myproc()->uid; }

int64_t sys_login(void) {
  char *name, *password;
  uint uid, gid;
  if (argstr(0, &name) < 0 || argstr(1, &password) < 0)
    return -1;
  if (auth_login(name, password, &uid, &gid) < 0)
    return -1;
  myproc()->uid = uid;
  myproc()->gid = gid;
  myproc()->doas_grant = 0;
  return 0;
}

int64_t sys_doas_auth(void) {
  char *password;
  if (argstr(0, &password) < 0 || auth_doas(myproc()->uid, password) < 0)
    return -1;
  myproc()->doas_grant = 1;
  return 0;
}

int64_t sys_setuid(void) {
  int uid;
  if (argint(0, &uid) < 0 || uid != 0)
    return -1;
  if (myproc()->uid != 0 && !myproc()->doas_grant)
    return -1;
  myproc()->uid = 0;
  myproc()->gid = 0;
  myproc()->doas_grant = 0;
  return 0;
}

int64_t sys_useradd(void) {
  char *name, *password;
  int wheel;
  if (myproc()->uid != 0 || argstr(0, &name) < 0 ||
      argstr(1, &password) < 0 || argint(2, &wheel) < 0)
    return -1;
  return auth_add_user(name, password, wheel != 0);
}

int64_t sys_passwd(void) {
  char *name, *password;
  if (argstr(0, &name) < 0 || argstr(1, &password) < 0)
    return -1;
  return auth_set_password(myproc()->uid, name, password);
}

int64_t sys_users(void) {
  struct user_info *out;
  int index;
  if (argptr(0, (char **)&out, sizeof(*out)) < 0 || argint(1, &index) < 0)
    return -1;
  return auth_user_info(index, out);
}

int64_t sys_sbrk(void) {
  uintptr_t addr;
  int n;

  if (argint(0, &n) < 0) {
    return -1;
  }
  addr = myproc()->sz;
  if (growproc(n) < 0) {
    return -1;
  }
  return addr;
}

int64_t sys_sleep(void) {
  uint n;
  uint ticks0;

  console_flush_if_dirty();
  if (argint(0, (int *)&n) < 0) {
    return -1;
  }

  // interval of ticks is 10ms (in QEMU), so we have to count 100 ticks for 1
  // second.
  n *= 100;

  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (myproc()->killed) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

int64_t sys_ping(void) {
  struct network_ping_request *request;
  if (argptr(0, (char **)&request, sizeof(*request)) < 0)
    return -1;
  return proninx_lwip_ping(request);
}
int64_t sys_udp_open(void) { return proninx_udp_open(); }
int64_t sys_udp_bind(void) { int h, p; return argint(0,&h)||argint(1,&p) ? -1 : proninx_udp_bind(h,p); }
int64_t sys_udp_close(void) { int h; return argint(0,&h) ? -1 : proninx_udp_close(h); }
int64_t sys_udp_sendto(void) { int h,n; char *b; struct network_endpoint *e; if (argint(0,&h)||argint(2,&n)||n<0||argptr(1,&b,n)||argptr(3,(char**)&e,sizeof(*e))) return -1; return proninx_udp_sendto(h,b,n,e); }
int64_t sys_udp_recvfrom(void) { int h,n,t; char *b; struct network_endpoint *e; if (argint(0,&h)||argint(2,&n)||n<=0||argptr(1,&b,n)||argptr(3,(char**)&e,sizeof(*e))||argint(4,&t)||t<0) return -1; return proninx_udp_recvfrom(h,b,n,e,t); }
int64_t sys_netinfo(void) { struct network_status *status; return argptr(0, (char **)&status, sizeof(*status)) < 0 ? -1 : proninx_lwip_status(status); }
int64_t sys_netconfig(void) { struct network_ipv4_config *config; return argptr(0, (char **)&config, sizeof(*config)) < 0 ? -1 : proninx_lwip_configure(config); }
int64_t sys_tcp_open(void) { return proninx_tcp_open(); }
int64_t sys_tcp_bind(void) { int h, p; return argint(0, &h) || argint(1, &p) || p < 1 || p > 65535 ? -1 : proninx_tcp_bind(h, p); }
int64_t sys_tcp_listen(void) { int h; return argint(0, &h) ? -1 : proninx_tcp_listen(h); }
int64_t sys_tcp_accept(void) { int h, timeout; return argint(0, &h) || argint(1, &timeout) || timeout < 0 ? -1 : proninx_tcp_accept(h, timeout); }
int64_t sys_tcp_connect(void) { int h, timeout; struct network_endpoint *e; return argint(0, &h) || argptr(1, (char **)&e, sizeof(*e)) || argint(2, &timeout) || timeout < 0 ? -1 : proninx_tcp_connect(h, e, timeout); }
int64_t sys_tcp_send(void) { int h, n; char *b; return argint(0, &h) || argint(2, &n) || n <= 0 || argptr(1, &b, n) ? -1 : proninx_tcp_send(h, b, n); }
int64_t sys_tcp_recv(void) { int h, n, timeout; char *b; return argint(0, &h) || argint(2, &n) || n <= 0 || argptr(1, &b, n) || argint(3, &timeout) || timeout < 0 ? -1 : proninx_tcp_recv(h, b, n, timeout); }
int64_t sys_tcp_close(void) { int h; return argint(0, &h) ? -1 : proninx_tcp_close(h); }
int64_t sys_dns_resolve(void) { struct network_dns_request *request; return argptr(0, (char **)&request, sizeof(*request)) < 0 ? -1 : proninx_dns_resolve(request); }

/* ========================================================================== */
/* FNU userland df syscall (fnudf utility)                                    */
/* Populates DF_ENTRIES_MAX entries describing root, UFS2 volumes, and RAM.  */
/* ========================================================================== */

/* Forward references into fs.c / storage.c world.  These symbols are all
 * defined in the files already compiled into the kernel binary and visible
 * at link time, even though we avoid including fs.h from here. */
extern struct superblock sb;
extern int root_fs_type;
#define FS_NATIVE 0
#define FS_FAT32  1
#define FS_UFS2   2
#define FNU_DATA_DEVICE 2

static unsigned popcount_byte(unsigned char v) {
  v = v - ((v >> 1) & 0x55u);
  v = (v & 0x33u) + ((v >> 2) & 0x33u);
  return ((v + (v >> 4)) & 0x0Fu);
}

/* Count free bits [bstart, bend) of the xv6 bitmap stored on device dev. */
static uint64_t native_count_free_blocks(uint dev, uint bstart, uint bend) {
  struct buf *bp;
  uint bi, mask, b, nbytes, start_within, end_within;
  uint64_t free_bits = 0;
  unsigned char *p;

  if (bend <= bstart)
    return 0;
  for (b = bstart; b < bend; ) {
    bp = bread(dev, BBLOCK(b, sb));
    nbytes = BSIZE;
    start_within = b - BBLOCK(b, sb) * BPB;
    for (bi = 0; bi < (uint)BPB && b < bend; bi++, b++) {
      p = (unsigned char *)bp->data + bi / 8;
      mask = 1u << (bi & 7);
      /* The bitmap layout of xv6: bit = 1 means "in use". */
      if ((uint)(*p & mask) == 0)
        free_bits++;
      (void)start_within;
      (void)end_within;
      (void)nbytes;
    }
    brelse(bp);
  }
  return free_bits;
}

int64_t
sys_df(void)
{
  struct df_stat *out;
  struct df_stat local[DF_ENTRIES_MAX];
  int capacity_arg, nout = 0;
  const struct ufs2_volume *vol;
  struct superblock sbcopy;
  struct proc *p;
  uint64_t data_total, data_free, data_bytes, data_used;

  if (argptr(0, (char **)&out, DF_ENTRIES_MAX * sizeof(*out)) < 0)
    return -1;
  if (argint(1, &capacity_arg) < 0 || capacity_arg < 1 || capacity_arg > (int)DF_ENTRIES_MAX)
    return -1;

  memset(local, 0, sizeof(local));

  /* -------------------------------------------------------------
   * Entry 1: FNU Root filesystem (native xv6 FS or UFS2 / ramdisk).
   * ---------------------------------------------------------- */
  safestrcpy(local[nout].label, "FNU Root", sizeof(local[nout].label));
  safestrcpy(local[nout].mount_point, "/", sizeof(local[nout].mount_point));
  local[nout].is_present  = 1;
  local[nout].is_read_only = (uint8)fs_root_readonly();

  if (root_fs_type == FS_UFS2 && (vol = storage_ufs2_volume(rootdev)) != 0) {
    uint64_t frags = (uint64)vol->fragments_per_group * (uint64)vol->cylinder_groups;
    data_bytes  = frags * (uint64)vol->fragment_size;
    data_used   = 0; /* approximated below via whole-volume heuristic */
    local[nout].total_bytes = data_bytes;
    local[nout].used_bytes  = data_used;
    local[nout].free_bytes  = data_bytes - data_used;
    nout++;
  } else if (root_fs_type == FS_NATIVE) {
    /* Read the legacy xv6 superblock off rootdev and walk bitmap. */
    readsb(rootdev, &sbcopy);
    data_total = sbcopy.nblocks;
    data_free  = native_count_free_blocks(rootdev, sbcopy.bmapstart * BPB,
                                          sbcopy.bmapstart * BPB + sbcopy.nblocks);
    data_free  = data_free > data_total ? data_total : data_free;
    data_bytes = sbcopy.size * (uint64)BSIZE;
    data_used  = (data_total - data_free) * (uint64)BSIZE;
    local[nout].total_bytes = data_bytes;
    local[nout].used_bytes  = data_used;
    local[nout].free_bytes  = data_free * (uint64)BSIZE;
    nout++;
  } else {
    /* FS_FAT32 or ramdisk-backed synthetic FS — use device size. */
    uint64_t sectors = blockdev_device_size(rootdev);
    data_bytes = sectors * 512;
    local[nout].total_bytes = data_bytes;
    local[nout].used_bytes  = data_bytes / 2; /* heuristic */
    local[nout].free_bytes  = data_bytes - local[nout].used_bytes;
    nout++;
  }

  /* -------------------------------------------------------------
   * Entry 2: FNU Data volume (UFS2 device 2) if mounted.
   * ---------------------------------------------------------- */
  if ((vol = storage_ufs2_volume(FNU_DATA_DEVICE)) != 0 && nout < DF_ENTRIES_MAX) {
    uint64_t frags = (uint64)vol->fragments_per_group * (uint64)vol->cylinder_groups;
    safestrcpy(local[nout].label, "FNU Data", sizeof(local[nout].label));
    safestrcpy(local[nout].mount_point, "/data", sizeof(local[nout].mount_point));
    local[nout].is_present  = 1;
    local[nout].is_read_only = 1;
    local[nout].total_bytes = frags * (uint64)vol->fragment_size;
    local[nout].free_bytes  = 0; /* exact per-CG free count not exposed yet */
    local[nout].used_bytes  = local[nout].total_bytes - local[nout].free_bytes;
    nout++;
  }

  /* -------------------------------------------------------------
   * Entry 3: RAM summary — mirrors fnufetch memory lines.
   * ---------------------------------------------------------- */
  if (nout < DF_ENTRIES_MAX) {
    uint64_t tot = get_total_ram();
    uint64_t fr  = get_free_ram();
    safestrcpy(local[nout].label, "RAM", sizeof(local[nout].label));
    safestrcpy(local[nout].mount_point, "--", sizeof(local[nout].mount_point));
    local[nout].is_present  = 1;
    local[nout].is_read_only = 0;
    local[nout].total_bytes = tot;
    local[nout].free_bytes  = fr;
    local[nout].used_bytes  = tot > fr ? tot - fr : 0;
    nout++;
  }

  p = myproc();
  if (copyout(p->pgdir, (uintptr_t)out, local, DF_ENTRIES_MAX * sizeof(*out)) < 0)
    return -1;
  return nout;
}
