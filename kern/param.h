#ifndef PRONINX_X86_64_PARAM_H
#define PRONINX_X86_64_PARAM_H

#define KSTACKSIZE 16384
#define ISTSTACKSIZE 4096
#define MAXCPU 256
#define MAXLOGNAME 33
#define NDFILE 32
#define NDEV 17
#define MAXPHYS 131072
#define MSGBUF_DUMP_SECTORS 256
#define BCLUSTER (MAXPHYS / BSIZE)
#define HIDIRTY (nbuf / 2)
#define LODIRTY (nbuf / 4)
#define NCARGS (32 * 1024)
#define NBUF 8192
#define MINBUF 256
#define BUFHASH 2048
#define FSSIZE 32768
#define MAXPATHDEPTH 64
#define HZ 100
#define UART_CONSOLE_SPEED 115200
#define MAXSYMLINKS 32
#define NGROUPS 16
#define DFLSSIZ (8UL * 1024 * 1024)
#define MAXSSIZ (32UL * 1024 * 1024)

#define PVM 4
#define PINOD 8
#define PRIBIO 16
#define PZERO 22
#define PSOCK 24
#define PWAIT 32
#define PLOCK 36
#define PPAUSE 40
#define PUSER 50
#define PCATCH 0x100
#define PNORELOCK 0x200
#define PRIMASK 0x0ff

#endif
