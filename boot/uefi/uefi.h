#ifndef PRONINX_BOOT_UEFI_UEFI_H
#define PRONINX_BOOT_UEFI_UEFI_H

#include "boot/bootdisk.h"
#include "boot/uefi/efi.h"
#include "inc/bootinfo.h"
#include "inc/types.h"

extern struct efi_system_table *ST;
extern struct efi_boot_services *BS;
extern efi_handle IMAGE;

void putstr(const char *s);
void putdec(uint64 v);
void puthex(uint64 v);
void panic(const char *s) __dead;

void *memcpy(void *d, const void *s, uint64 n);
void *memset(void *d, int c, uint64 n);
int memcmp(const void *a, const void *b, uint64 n);

int guid_eq(const struct efi_guid *a, const struct efi_guid *b);

int file_mount(void);
int file_partition_guid(uint8 out[16]);
int file_bootdisk(struct bootdisk *d);
struct efi_file *file_open(const char *path);
uint64 file_length(struct efi_file *f);
int file_pread(struct efi_file *f, uint64 off, void *dst, uint64 n,
               uint64 *got);
void file_close(struct efi_file *f);

efi_phys boot_alloc(uint64 bytes);

void video_setup(struct fnu_bootinfo *bi);
void pci_roms(struct fnu_bootinfo *bi);

void memory_map(struct fnu_bootinfo *bi, uint64 keep, uint64 keeplen,
                const void *map, uint64 bytes,
                uint64 descsize);
uint64 paging_setup(void);
void enter_kernel(uint64 entry, uint64 cr3, uint64 info)
    __dead;

#endif
