#ifndef PRONINX_X86_64_X86_H
#define PRONINX_X86_64_X86_H

#include "inc/types.h"

static inline uchar inb(ushort port) {
  uchar data;
  __asm__ volatile("in %1,%0" : "=a"(data) : "d"(port));
  return data;
}

static inline ushort inw(ushort port) {
  ushort data;
  __asm__ volatile("in %1,%0" : "=a"(data) : "d"(port));
  return data;
}

static inline uint inl(ushort port) {
  uint data;
  __asm__ volatile("in %1,%0" : "=a"(data) : "d"(port));
  return data;
}

static inline void insl(int port, void *addr, int cnt) {
  __asm__ volatile("cld; rep insl"
                   : "=D"(addr), "=c"(cnt)
                   : "d"(port), "0"(addr), "1"(cnt)
                   : "memory", "cc");
}

static inline void outb(ushort port, uchar data) {
  __asm__ volatile("out %0,%1" : : "a"(data), "d"(port));
}

static inline void outw(ushort port, ushort data) {
  __asm__ volatile("out %0,%1" : : "a"(data), "d"(port));
}

static inline void outl(ushort port, uint data) {
  __asm__ volatile("out %0,%1" : : "a"(data), "d"(port));
}

static inline void outsl(int port, const void *addr, int cnt) {
  __asm__ volatile("cld; rep outsl"
                   : "=S"(addr), "=c"(cnt)
                   : "d"(port), "0"(addr), "1"(cnt)
                   : "cc");
}

struct segdesc;

static inline void lgdt(struct segdesc *p, uint16_t size) {
  volatile uint16_t pd[5];

  pd[0] = size - 1;
  pd[1] = ((uintptr_t)p) & 0xffff;
  pd[2] = (((uintptr_t)p) >> 16) & 0xffff;
  pd[3] = (((uintptr_t)p) >> 32) & 0xffff;
  pd[4] = (((uintptr_t)p) >> 48) & 0xffff;

  __asm__ volatile("lgdt (%0)" : : "r"(pd));
}

struct gatedesc;

static inline void lidt(struct gatedesc *p, uint16_t size) {
  volatile uint16_t pd[5];

  pd[0] = size - 1;
  pd[1] = ((uintptr_t)p) & 0xffff;
  pd[2] = (((uintptr_t)p) >> 16) & 0xffff;
  pd[3] = (((uintptr_t)p) >> 32) & 0xffff;
  pd[4] = (((uintptr_t)p) >> 48) & 0xffff;

  __asm__ volatile("lidt (%0)" : : "r"(pd));
}

static inline void ltr(uint16_t sel) {
  __asm__ volatile("ltr %0" : : "r"(sel));
}

static inline void cli(void) { __asm__ volatile("cli"); }

static inline void sti(void) { __asm__ volatile("sti"); }

static inline void lcr3(uintptr_t val) {
  __asm__ volatile("movq %0,%%cr3" : : "r"(val) : "memory");
}

static inline uintptr_t rcr3(void) {
  uintptr_t val;
  __asm__ volatile("movq %%cr3,%0" : "=r"(val));
  return val;
}

static inline uintptr_t rcr2(void) {
  uintptr_t val;
  __asm__ volatile("movq %%cr2,%0" : "=r"(val));
  return val;
}

static inline uintptr_t rcr4(void) {
  uintptr_t val;
  __asm__ volatile("movq %%cr4,%0" : "=r"(val));
  return val;
}

static inline void lcr4(uintptr_t val) {
  __asm__ volatile("movq %0,%%cr4" : : "r"(val));
}

static inline uint xchg(volatile uint *addr, uint newval) {
  uint result;
  __asm__ volatile("lock; xchgl %0, %1"
                   : "+m"(*addr), "=a"(result)
                   : "1"(newval)
                   : "cc");
  return result;
}

static inline uint readeflags(void) {
  ulong eflags;
  __asm__ volatile("pushfq; pop %0" : "=r"(eflags));
  return (uint)eflags;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
  uint32_t low = (uint32_t)val;
  uint32_t high = (uint32_t)(val >> 32);
  __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return ((uint64_t)high << 32) | low;
}

static inline uint64_t rdtsc(void) {
  uint32_t low, high;
  __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
  return ((uint64_t)high << 32) | low;
}

static inline void cpuid_count(uint32_t leaf, uint32_t sub, uint32_t *a,
                               uint32_t *b, uint32_t *c, uint32_t *d) {
  __asm__ volatile("cpuid"
                   : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                   : "a"(leaf), "c"(sub));
}

static inline void hlt(void) { __asm__ volatile("hlt"); }

// Layout of the trap frame built on the stack by the
// hardware and by trapasm.S, and passed to trap().
struct trapframe {
  // registers
  uint64_t rax;
  uint64_t rbx;
  uint64_t rcx;
  uint64_t rdx;
  uint64_t rbp;
  uint64_t rsi;
  uint64_t rdi;
  uint64_t r8;
  uint64_t r9;
  uint64_t r10;
  uint64_t r11;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;

  uint64_t trapno;

  // below here defined by x86-64 hardware
  uint64_t err;
  uint64_t rip;
  uint16_t cs;
  uint16_t padding_cs1;
  uint32_t padding_cs2;
  uint64_t rflags;

  // below here only when crossing rings, such as from user to kernel
  uint64_t rsp;
  uint16_t ss;
  uint16_t padding_ss1;
  uint32_t padding_ss2;
};

typedef struct trapframe trapframe_t __attribute__((aligned(16)));

#endif /* ifndef PRONINX_X86_64_X86_H */
