#include "inc/syscall.h"

#define T_SYSCALL 0x30

#define SYSCALL(name) \
  .globl name; \
  name: \
    mov $SYS_ ## name, %rax; \
    int $T_SYSCALL; \
    ret

SYSCALL(fork)
SYSCALL(exit)
SYSCALL(wait)
SYSCALL(waitpid)
SYSCALL(pipe)
SYSCALL(read)
SYSCALL(kill)
SYSCALL(exec)
SYSCALL(stat)
SYSCALL(malloc)
SYSCALL(free)
SYSCALL(info)
SYSCALL(reboot)
SYSCALL(procinfo)
SYSCALL(ioctl)
SYSCALL(getdents)
SYSCALL(chdir)
SYSCALL(dup)
SYSCALL(getpid)
SYSCALL(getuid)
SYSCALL(login)
SYSCALL(doas_auth)
SYSCALL(setuid)
SYSCALL(useradd)
SYSCALL(passwd)
SYSCALL(users)
SYSCALL(setforeground)
SYSCALL(chmod)
SYSCALL(chown)
SYSCALL(sbrk)
SYSCALL(sleep)
SYSCALL(open)
SYSCALL(write)
SYSCALL(mknod)
SYSCALL(unlink)
SYSCALL(link)
SYSCALL(mkdir)
SYSCALL(close)
SYSCALL(ping)
SYSCALL(udp_open)
SYSCALL(udp_bind)
SYSCALL(udp_sendto)
SYSCALL(udp_recvfrom)
SYSCALL(udp_close)
SYSCALL(netinfo)
SYSCALL(netconfig)
SYSCALL(tcp_open)
SYSCALL(tcp_bind)
SYSCALL(tcp_listen)
SYSCALL(tcp_accept)
SYSCALL(tcp_connect)
SYSCALL(tcp_send)
SYSCALL(tcp_recv)
SYSCALL(tcp_close)
SYSCALL(dns_resolve)

.section .note.GNU-stack,"",@progbits
