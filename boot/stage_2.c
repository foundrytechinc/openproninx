#include "elf.h"
#include "types.h"
#include "x86.h"

#define SECTSIZE 512
#define KERNEL_ELF_PHYS_ADDR 0x10000
#define KERNEL_START_SECTOR 32

void (*kernel_entry)(void);

void readseg(uchar *, uint, uint);
void stage_3(void);

void stage_2(void) {
  struct elfhdr *elf;
  struct proghdr *ph, *eph;
  uchar *pa;

  elf = (struct elfhdr *)KERNEL_ELF_PHYS_ADDR;

  readseg((uchar *)elf, 4096, 0);

  if (elf->magic != ELF_MAGIC)
    return;

  ph = (struct proghdr *)((uchar *)elf + elf->phoff);
  eph = ph + elf->phnum;
  for (; ph < eph; ph++) {
    pa = (uchar *)((uint32_t)ph->paddr);
    readseg(pa, ph->filesz, ph->off);
    if (ph->memsz > ph->filesz)
      stosb(pa + ph->filesz, 0, ph->memsz - ph->filesz);
  }

  kernel_entry = (void (*)(void))((uint32_t)elf->entry);

  stage_3();
}

void waitdisk(void) {
  while ((inb(0x1F7) & 0xC0) != 0x40)
    ;
}

void readsect(void *dst, uint offset) {
  waitdisk();
  outb(0x1F2, 1);
  outb(0x1F3, offset);
  outb(0x1F4, offset >> 8);
  outb(0x1F5, offset >> 16);
  outb(0x1F6, (offset >> 24) | 0xE0);
  outb(0x1F7, 0x20);

  waitdisk();
  insl(0x1F0, dst, SECTSIZE / 4);
}

void readseg(uchar *pa, uint count, uint offset) {
  uchar *epa;

  epa = pa + count;

  pa -= offset % SECTSIZE;

  offset = (offset / SECTSIZE) + KERNEL_START_SECTOR;

  for (; pa < epa; pa += SECTSIZE, offset++)
    readsect(pa, offset);
}
