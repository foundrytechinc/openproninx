#ifndef PRONINX_X86_64_MMU_H
#define PRONINX_X86_64_MMU_H

// control registers
#define CR0_PE 0x00000001
#define CR0_WP 0x00010000
#define CR0_PG 0x80000000
#define CR0_NW 0x20000000
#define CR0_CD 0x40000000
#define CR4_PAE 0x00000020
#define CR4_MCE 0x00000040
#define CR4_PGE 0x00000080
#define CR4_SMEP 0x00100000
#define CR4_SMAP 0x00200000

// eflags
#define FL_CF 0x00000001
#define FL_TF 0x00000100
#define FL_IF 0x00000200

#define SEG_KCODE 1
#define SEG_KDATA 2
#define SEG_UCODE 3
#define SEG_UDATA 4
#define SEG_TSS 5

#define NSEGS 7

#ifndef LOCORE

#include "inc/types.h"

// segment descriptor
struct segdesc {
  uint lim_15_0 : 16;
  uint base_15_0 : 16;
  uint base_23_16 : 8;
  uint type : 4;
  uint s : 1;
  uint dpl : 2;
  uint p : 1;
  uint lim_19_16 : 4;
  uint avl : 1;
  uint l : 1;
  uint db : 1;
  uint g : 1;
  uint base_31_24 : 8;
};

#define SEG(type, base, lim, dpl)                                              \
  (struct segdesc){((lim) >> 12) & 0xffff,                                     \
                   (uint)(base) & 0xffff,                                      \
                   ((uint)(base) >> 16) & 0xff,                                \
                   type,                                                       \
                   1,                                                          \
                   dpl,                                                        \
                   1,                                                          \
                   (uint)(lim) >> 28,                                          \
                   0,                                                          \
                   1,                                                          \
                   0,                                                          \
                   1,                                                          \
                   (uint)(base) >> 24}

// tss descriptor
struct tssdesc {
  uint32_t lim_15_0 : 16;
  uint32_t base_15_0 : 16;
  uint32_t base_23_16 : 8;
  uint32_t type : 4;
  uint32_t s : 1;
  uint32_t dpl : 2;
  uint32_t p : 1;
  uint32_t lim_19_16 : 4;
  uint32_t avl : 1;
  uint32_t l : 1;
  uint32_t db : 1;
  uint32_t g : 1;
  uint32_t base_31_24 : 8;
  uint32_t base_63_32;
  uint32_t padding1;
};

#define TSSDESC64(type, base, lim, dpl)                                        \
  (struct tssdesc){(lim) & 0xffff,                                             \
                   (uint32_t)((uint64_t)(base) & 0xffff),                      \
                   (uint32_t)(((uint64_t)(base) >> 16) & 0xff),                \
                   type,                                                       \
                   0,                                                          \
                   dpl,                                                        \
                   1,                                                          \
                   (uint32_t)(lim) >> 16,                                      \
                   0,                                                          \
                   0,                                                          \
                   0,                                                          \
                   0,                                                          \
                   (uint32_t)(((uint64_t)(base) >> 24) & 0xff),                \
                   (uint32_t)((uint64_t)(base) >> 32),                         \
                   0}

#endif

#define DPL_USER 0x3

#define STA_X 0x8
#define STA_W 0x2
#define STA_R 0x2

#define STS_T64A 0x9
#define STS_IG64 0xE
#define STS_TG64 0xF

#define PTX4(va) ((((uintptr_t)va) >> 12 >> 9 >> 9 >> 9) & 0777)
#define PTX3(va) ((((uintptr_t)va) >> 12 >> 9 >> 9) & 0777)
#define PTX2(va) ((((uintptr_t)va) >> 12 >> 9) & 0777)
#define PTX1(va) ((((uintptr_t)va) >> 12) & 0777)

#define NPTENTRIES 512
#define PGSIZE 4096

#define PGROUNDUP(sz) (((sz) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE - 1))

// page table entry flags
#define PTE_P 0x001
#define PTE_W 0x002
#define PTE_U 0x004
#define PTE_PWT 0x008
#define PTE_PCD 0x010
#define PTE_A 0x020
#define PTE_D 0x040
#define PTE_PS 0x080
#define PTE_NX 0x8000000000000000ull

#define PGFLT_PRESENT 0x1
#define PGFLT_WRITE 0x2
#define PGFLT_USER 0x4
#define PGFLT_RESERVED 0x8
#define PGFLT_FETCH 0x10

#define PTE_FLAGS(pte) ((uintptr_t)(pte) & 0xFFF)
#define PTE_ADDR(pte) (((((uintptr_t)(pte)) >> 12) & 0xffffffffffff) << 12)
// swapped out: slot where the frame was
#define PTE_SLOT(pte) (PTE_ADDR(pte) >> 12)
#define SLOT_PTE(s) ((pte_t)(s) << 12)

#ifndef LOCORE

#include "inc/types.h"

struct taskstate {
  uint32_t padding1;
  uint32_t rsp0_31_0;
  uint32_t rsp0_63_32;
  uint32_t rsp1_31_0;
  uint32_t rsp1_63_32;
  uint32_t rsp2_31_0;
  uint32_t rsp2_63_32;
  uint32_t padding2;
  uint32_t padding3;
  uint32_t ist1_31_0;
  uint32_t ist1_63_32;
  uint32_t ist2_31_0;
  uint32_t ist2_63_32;
  uint32_t ist3_31_0;
  uint32_t ist3_63_32;
  uint32_t ist4_31_0;
  uint32_t ist4_63_32;
  uint32_t ist5_31_0;
  uint32_t ist5_63_32;
  uint32_t ist6_31_0;
  uint32_t ist6_63_32;
  uint32_t ist7_31_0;
  uint32_t ist7_63_32;
  uint32_t padding4;
  uint32_t padding5;
  uint16_t padding6;
  uint16_t iomb;
};

struct gatedesc {
  uint32_t off_15_0 : 16;
  uint32_t cs : 16;
  uint32_t ist : 3;
  uint32_t args : 2;
  uint32_t rsv1 : 3;
  uint32_t type : 4;
  uint32_t s : 1;
  uint32_t dpl : 2;
  uint32_t p : 1;
  uint32_t off_31_16 : 16;
  uint32_t off_63_32;
  uint32_t padding1;
};

#define SETGATE(gate, istrap, sel, off, d)                                     \
  {                                                                            \
    (gate).off_15_0 = (uint32_t)((off) & 0xffff);                              \
    (gate).cs = (sel);                                                         \
    (gate).ist = 0;                                                            \
    (gate).args = 0;                                                           \
    (gate).rsv1 = 0;                                                           \
    (gate).type = (istrap) ? STS_TG64 : STS_IG64;                              \
    (gate).s = 0;                                                              \
    (gate).dpl = (d);                                                          \
    (gate).p = 1;                                                              \
    (gate).off_31_16 = (uint32_t)((((off) >> 16) & 0xffff));                   \
    (gate).off_63_32 = (uint32_t)(((off) >> 32));                              \
    (gate).padding1 = 0;                                                       \
  }

#endif

#endif
