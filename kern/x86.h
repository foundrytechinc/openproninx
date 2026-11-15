#ifndef PRONINX_X86_64_X86_H
#define PRONINX_X86_64_X86_H

#include "inc/types.h"

static inline uchar inb(ushort port) {
  uchar data;
  __asm __volatile("in %1,%0" : "=a"(data) : "d"(port));
  return data;
}

static inline ushort inw(ushort port) {
  ushort data;
  __asm __volatile("in %1,%0" : "=a"(data) : "d"(port));
  return data;
}

static inline uint inl(ushort port) {
  uint data;
  __asm __volatile("in %1,%0" : "=a"(data) : "d"(port));
  return data;
}

static inline void insl(int port, void *addr, int cnt) {
  __asm __volatile("cld; rep insl"
                   : "=D"(addr), "=c"(cnt)
                   : "d"(port), "0"(addr), "1"(cnt)
                   : "memory", "cc");
}

static inline void outb(ushort port, uchar data) {
  __asm __volatile("out %0,%1" : : "a"(data), "d"(port));
}

static inline void outw(ushort port, ushort data) {
  __asm __volatile("out %0,%1" : : "a"(data), "d"(port));
}

static inline void outl(ushort port, uint data) {
  __asm __volatile("out %0,%1" : : "a"(data), "d"(port));
}

static inline void outsl(int port, const void *addr, int cnt) {
  __asm __volatile("cld; rep outsl"
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

  __asm __volatile("lgdt (%0)" : : "r"(pd));
}

struct gatedesc;

static inline void lidt(struct gatedesc *p, uint16_t size) {
  volatile uint16_t pd[5];

  pd[0] = size - 1;
  pd[1] = ((uintptr_t)p) & 0xffff;
  pd[2] = (((uintptr_t)p) >> 16) & 0xffff;
  pd[3] = (((uintptr_t)p) >> 32) & 0xffff;
  pd[4] = (((uintptr_t)p) >> 48) & 0xffff;

  __asm __volatile("lidt (%0)" : : "r"(pd));
}

static inline void ltr(uint16_t sel) {
  __asm __volatile("ltr %0" : : "r"(sel));
}

static inline void cli(void) { __asm __volatile("cli"); }

static inline void sti(void) { __asm __volatile("sti"); }

static inline void lcr3(uintptr_t val) {
  __asm __volatile("movq %0,%%cr3" : : "r"(val) : "memory");
}

static inline uintptr_t rcr3(void) {
  uintptr_t val;
  __asm __volatile("movq %%cr3,%0" : "=r"(val));
  return val;
}

static inline uintptr_t rcr0(void) {
  uintptr_t v;
  __asm __volatile("mov %%cr0,%0" : "=r"(v));
  return v;
}

static inline void lcr0(uintptr_t v) { __asm __volatile("mov %0,%%cr0" : : "r"(v)); }

static inline void wbinvd(void) { __asm __volatile("wbinvd" ::: "memory"); }

static inline uintptr_t rcr2(void) {
  uintptr_t val;
  __asm __volatile("movq %%cr2,%0" : "=r"(val));
  return val;
}

static inline uintptr_t rcr4(void) {
  uintptr_t val;
  __asm __volatile("movq %%cr4,%0" : "=r"(val));
  return val;
}

static inline void lcr4(uintptr_t val) {
  __asm __volatile("movq %0,%%cr4" : : "r"(val));
}

#define mb() __asm __volatile("mfence" : : : "memory")

static inline uint atomic_fetchadd_int(volatile uint *p, uint v) {
  __asm __volatile("lock; xaddl %0,%1" : "+r"(v), "+m"(*p) : : "cc");
  return v;
}

static inline ulong atomic_fetchadd_long(volatile ulong *p, ulong v) {
  __asm __volatile("lock; xaddq %0,%1" : "+r"(v), "+m"(*p) : : "cc");
  return v;
}

static inline uint xchg(volatile uint *addr, uint newval) {
  uint result;
  __asm __volatile("lock; xchgl %0, %1"
                   : "+m"(*addr), "=a"(result)
                   : "1"(newval)
                   : "cc");
  return result;
}

static inline uint readeflags(void) {
  ulong eflags;
  __asm __volatile("pushfq; pop %0" : "=r"(eflags));
  return (uint)eflags;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
  uint32_t low = (uint32_t)val;
  uint32_t high = (uint32_t)(val >> 32);
  __asm __volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
  uint32_t low, high;
  __asm __volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return ((uint64_t)high << 32) | low;
}

static inline uint64_t rdtsc(void) {
  uint32_t low, high;
  __asm __volatile("rdtsc" : "=a"(low), "=d"(high));
  return ((uint64_t)high << 32) | low;
}

#define MSR_FS_BASE 0xC0000100
#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

static inline void cpuid_count(uint32_t leaf, uint32_t sub, uint32_t *a,
                               uint32_t *b, uint32_t *c, uint32_t *d) {
  __asm __volatile("cpuid"
                   : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                   : "a"(leaf), "c"(sub));
}

static inline void hlt(void) { __asm __volatile("hlt"); }

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

  uint64_t err;
  uint64_t rip;
  uint16_t cs;
  uint16_t padding_cs1;
  uint32_t padding_cs2;
  uint64_t rflags;

  uint64_t rsp;
  uint16_t ss;
  uint16_t padding_ss1;
  uint32_t padding_ss2;
};

typedef struct trapframe trapframe_t __aligned(16);

#endif /* ifndef PRONINX_X86_64_X86_H */
