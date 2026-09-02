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

#endif /* ifndef PRONINX_X86_64_SYSCALL_H */
