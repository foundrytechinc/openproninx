#ifndef PRONINX_X86_64_SCHED_H
#define PRONINX_X86_64_SCHED_H

#include "inc/types.h"

struct proc;
struct cpu_info;

// SCHEDULER
#define SCHED_NQS 32
#define SCHED_PPQ (128 / SCHED_NQS)
#define PPQMASK (SCHED_PPQ - 1)
#define MAXPRI 127
#define NULLPRI 255

#define NOLEND 255

#define PRIO_MIN (-20)
#define PRIO_MAX 20
#define PRIO_RANGE (PRIO_MAX - PRIO_MIN + 1)

// nice +20 takes 1/NICE_SHARE of a cpu from nice 0
#define NICE_PRIS 40
#define NICE_SHARE 10
#define EST_PRIS (NICE_PRIS / 2 * NICE_SHARE / (NICE_SHARE - 2))
#define ESTCPUPPQ 512
#define ESTCPUMAX (ESTCPUPPQ * 20)
#define ESTCPULIM(v) ((v) < ESTCPUMAX ? (v) : ESTCPUMAX)
#define ESTCPUFREQ HZ
// a second of running = the cap. uload is demand
#define ESTCPUKICK (ESTCPUMAX / ESTCPUFREQ + 1)
// decay in tenths of a second
#define ESTCPUSTEP (ESTCPUFREQ / 10)
#define ESTCPUSTEPS (ESTCPUFREQ / ESTCPUSTEP)

#define FSHIFT 11
#define FSCALE (1 << FSHIFT)

void sched_init(void);
void sched_setrunqueue(struct proc *p, uint8_t prio);
struct proc *sched_chooseproc(struct cpu_info *c);
void sched_release(struct cpu_info *c);
int sched_wanted(void);
void sched_clock(struct cpu_info *c, struct proc *p, int usermode);
void sched_decay(struct proc *p);
void sched_second(struct proc *p);
void sched_wake(struct proc *p);
void sched_userret(struct proc *p);
void sched_resetpriority(struct proc *p);
void sched_fork(struct proc *parent, struct proc *child);
void sched_exit(struct proc *p);
void sched_lend(struct proc *p, uint8_t pri);
void sched_requeue(struct proc *p, int voluntary);
void sched_park(struct cpu_info *c);
void sched_notify(int i);
void sched_notify_flush(struct cpu_info *c);
void sched_loadavg_update(void);
void sched_loadavg(uint32_t out[3]);
uint8_t sched_priority(struct proc *p);

#endif
