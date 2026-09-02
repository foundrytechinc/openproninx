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
