#include "defs.h"
#include "proc.h"
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
  if (myproc()->uid != 0)
    return -1;
  cprintf("Rebooting...\n");
  // Pulse the CPU reset line via the keyboard controller
  uint8_t good = 0x02;
  while (good & 0x02)
    good = inb(0x64);
  outb(0x64, 0xFE);
  return 0; // Should not reach here
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
  exit();
  return 0; // not reached
}

int64_t sys_wait(void) { return wait(); }

int64_t sys_waitpid(void) {
  pid_t pid;
  int flags;

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
