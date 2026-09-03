#ifndef PRONINX_X86_64_SYSCALL_H
#define PRONINX_X86_64_SYSCALL_H

#define SYS_exit 0
#define SYS_read 1
#define SYS_write 2
#define SYS_open 3
#define SYS_close 4
#define SYS_fork 5
#define SYS_wait 6
#define SYS_kill 7
#define SYS_exec 8
#define SYS_sbrk 9
#define SYS_malloc 10
#define SYS_free 11
#define SYS_stat 12
#define SYS_pipe 13
#define SYS_info 14
#define SYS_reboot 15
#define SYS_procinfo 16
#define SYS_ioctl 17
#define SYS_getdents 18
#define SYS_link 19
#define SYS_mkdir 20
#define SYS_unlink 21
#define SYS_dup 22
#define SYS_mknod 23
#define SYS_chdir 24

// Non-ABI syscalls
#define SYS_getpid 25
#define SYS_sleep 26
#define SYS_waitpid 27
#define SYS_ping 28
#define SYS_udp_open 29
#define SYS_udp_bind 30
#define SYS_udp_sendto 31
#define SYS_udp_recvfrom 32
#define SYS_udp_close 33
#define SYS_netinfo 34
#define SYS_netconfig 35
#define SYS_tcp_open 36
#define SYS_tcp_bind 37
#define SYS_tcp_listen 38
#define SYS_tcp_accept 39
#define SYS_tcp_connect 40
#define SYS_tcp_send 41
#define SYS_tcp_recv 42
#define SYS_tcp_close 43
#define SYS_dns_resolve 44
#define SYS_getuid 45
#define SYS_login 46
#define SYS_doas_auth 47
#define SYS_setuid 48
#define SYS_useradd 49
#define SYS_passwd 50
#define SYS_users 51
#define SYS_setforeground 52
#define SYS_chmod 53
#define SYS_chown 54

#endif /* ifndef PRONINX_X86_64_SYSCALL_H */
