# SMP

All application processors (APs) found in the MADT, or in the MP table when
there is no MADT, are started at boot and run the scheduler.

## Starting an AP

1. `lowmem.c` gives a page under 1 MB for the trampoline. The first 64 KB
   and the BIOS area at the top are left alone.
2. `entryother.S` is copied there. It is linked at 0, takes its base from
   `%cs` and patches its GDT pointers and far jumps.
3. The BSP sends INIT, INIT deassert and two STARTUPs (`lapicstartap`).
   Only CPUs older than Intel family 6, AMD 0Fh and Hygon 18h get the
   10 ms wait after INIT; the rest get 10 us.
4. The AP goes real mode, then 32 bit with PAE and the low page tables, then
   long mode (with NXE if the CPU has it), and jumps to `mpentry`, which
   loads the kernel page table and calls `mp_startup()`.
5. `mp_startup()` sets up x2APIC if chosen, MTRRs, `%gs`, GDT, TSC_ADJUST,
   the local APIC and IDT, then enters the scheduler.

Every step writes its number into the trampoline header. An AP that does not
come up within 5 seconds is skipped and the boot prints the step it reached,
the APIC error register and the SMI count. If no AP starts at all, the BSP
tries once more with an INIT/STARTUP broadcast.

During boot a performance counter NMI fires twice a second. If the console
is silent for 20 seconds it prints where the boot CPU is.

The S3 wake starts the APs again through the same page.

## Scheduler

`kern/sched.c` follows DragonFly's `usched_dfly`.

- Each CPU has 32 run queues, four priorities each. Locks are per run queue,
  per thread (`p->lock`) and per sleep hash chain. `proctree_lock` covers
  the process tree, signals and job control. Lock order: `proctree_lock`,
  sleep chain, thread, run queue, callout.
- User priority is `PUSER` plus a nice part and an `estcpu` part. Two busy
  threads at nice 0 and nice +20 split the CPU about 90/10. `estcpu` decays
  like in 4.4BSD, using the load of the thread's own CPU.
- A thread that wakes up goes to the least loaded CPU, preferring its own
  CPU and the waker's. Cores are filled before hyperthreads (CPUID 0xB,
  0x8000001E, or leaves 1 and 4).
- Time slice is `kern.sched.rrinterval` (100 ms). A better thread preempts
  at the next trap or by IPI. Idle CPUs pull work from the busiest one, and
  every eight ticks one CPU rebalances. Tunables are `kern.sched.weight*` and
  `kern.sched.features`, named as in DragonFly.
- A CPU parked for S3 or S4 hands its queue to the others.

## Per-CPU data

`struct cpu_info` is at `%gs:0`. Each core writes its address into
`IA32_GS_BASE` when it starts, and the trap entry uses `swapgs`. The NMI,
#DB, #DF and #MC handlers run on their own stacks and read the MSR to see
whose `%gs` is loaded.

An idle CPU sleeps in MWAIT or `hlt` until an interrupt. Every CPU has its
own local APIC timer. The CPU that gets furthest advances `ticks` and runs
the callouts. A TLB shootdown also flushes the CPU that starts it and waits
for all others.

## Panic

Shutdown and panic stop the other CPUs with an IPI. One CPU panics; a second
panic on another CPU just stops that CPU. The machine reboots after
`kern.panic_reboot_wait_time` seconds (default 15, -1 waits for a key). A
key on the console stops the countdown.

AP stacks come from `vmem_alloc` and are kept for S3.
