#ifndef PRONINX_X86_64_SYSCALL_H
#define PRONINX_X86_64_SYSCALL_H

// THE SYSTEM'S CALLS

// processes
#define SYS_exit           0
#define SYS_fork           1
#define SYS_exec           2
#define SYS_wait           3
#define SYS_signal         4
#define SYS_procids        5
#define SYS_session_new    6
#define SYS_pgroup_set     7
#define SYS_ident_get      8
#define SYS_ident_set      9
#define SYS_niceness      10
#define SYS_createmask    11
#define SYS_limit         12
#define SYS_usage         13
#define SYS_timer         14
#define SYS_sleep         15
#define SYS_halt          16
#define SYS_reboot        17
#define SYS_poweroff      18
#define SYS_ptrace        19

// signals
#define SYS_sigact        20
#define SYS_sigmask       21
#define SYS_sigraised     22
#define SYS_sigtake       23
#define SYS_sigpause      24
#define SYS_sigreturn     25
#define SYS_altstack      26

// threads
#define SYS_thread_start  30
#define SYS_thread_exit   31
#define SYS_thread_tcb    32
#define SYS_thread_id     33
#define SYS_thread_yield  34
#define SYS_futex         35

// descriptors
#define SYS_read          40
#define SYS_write         41
#define SYS_seek          42
#define SYS_close         43
#define SYS_dup           44
#define SYS_pipe          45
#define SYS_ioctl         46
#define SYS_fdstate_get   47
#define SYS_fdstate_set   48
#define SYS_ready         49
#define SYS_flush         50
#define SYS_readdir       51
#define SYS_eventq        52
#define SYS_events        53

// names
#define SYS_open          60
#define SYS_getattr       61
#define SYS_setattr       62
#define SYS_make          63
#define SYS_remove        64
#define SYS_link          65
#define SYS_rename        66
#define SYS_readlink      67
#define SYS_chdir         68
#define SYS_access        69
#define SYS_pathconf      70
#define SYS_pathof        71
#define SYS_mount         72
#define SYS_unmount       73
#define SYS_fsstat        74
#define SYS_swapon        75
#define SYS_swapoff       76
#define SYS_unveil        77

// memory
#define SYS_map           80
#define SYS_unmap         81
#define SYS_protect       82
#define SYS_memsync       83
#define SYS_memuse        84

// sockets
#define SYS_socket        90
#define SYS_socketpair    91
#define SYS_bind          92
#define SYS_listen        93
#define SYS_accept        94
#define SYS_connect       95
#define SYS_send          96
#define SYS_recv          97
#define SYS_shutdown      98
#define SYS_sockaddr      99
#define SYS_sockopt_set  100
#define SYS_sockopt_get  101

// the machine
#define SYS_info         110
#define SYS_procinfo     111
#define SYS_hwinfo       112
#define SYS_diskinfo     113
#define SYS_netinfo      114
#define SYS_netconfig    115
#define SYS_flock        116
#define SYS_rtctime      118
#define SYS_gettime      119
#define SYS_random       120
#define SYS_sysctl       121
#define SYS_sysctlnode   122
#define SYS_pledge       123
#define SYS_sendsyslog   124
#define SYS_settime      125
#define SYS_slewtime     126

#define NSYSCALL 128

#endif
