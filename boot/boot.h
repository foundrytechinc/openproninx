#ifndef PRONINX_BOOT_H
#define PRONINX_BOOT_H

#include "inc/bootinfo.h"
#include "inc/types.h"

#define SECTOR 512

struct bios_dap {
  uint8 size;
  uint8 zero;
  uint16 count;
  uint16 off;
  uint16 seg;
  uint64 lba;
};

int bios_putc(int c);
int bios_disk_extcheck(int drive);
int bios_disk_read(int drive, struct bios_dap *dap);
int bios_disk_params(int drive, void *buf);
int bios_e820(uint32 *cont, void *buf);
int bios_vbe_info(void *buf);
int bios_vbe_mode_info(int mode, void *buf);
int bios_vbe_set_mode(int mode);
int bios_vbe_get_mode(void);
int bios_vbe_edid(void *buf);
uint32 vtophys(void *p);
void flat_copy(void *dst, uint32 src, uint32 n);
void flat_write(uint32 dst, const void *src, uint32 n);
void flat_move(uint32 dst, uint32 src, uint32 n);
void flat_zero(uint32 dst, uint32 n);
void enter_long(uint64 entry, uint32 cr3, uint32 info)
    __attribute__((noreturn));

void putstr(const char *s);
void panic(const char *s) __attribute__((noreturn));
void *memcpy(void *d, const void *s, uint32 n);
void *memset(void *d, int c, uint32 n);
int memcmp(const void *a, const void *b, uint32 n);

int disk_read(uint64 lba, uint32 count, void *dst);
int disk_read_far(uint64 lba, uint32 count, uint32 linear);

int gpt_find(const uint8 type[16], uint64 *start, uint64 *count);

int fat_mount(uint64 lba);
int fat_load(const char *path, uint32 linear, uint32 limit, uint32 *size);

int coreboot_probe(struct fnu_bootinfo *bi);
void e820_map(struct fnu_bootinfo *bi);
void video_setup(struct fnu_bootinfo *bi);
uint64 acpi_scan(void);

extern int boot_drive;
extern uint64 boot_part_lba;

#endif
