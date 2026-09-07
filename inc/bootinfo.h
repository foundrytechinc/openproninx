#ifndef PRONINX_X86_64_BOOTINFO_H
#define PRONINX_X86_64_BOOTINFO_H

#include "inc/types.h"

// handed to the kernel in %rdi

#define FNU_BOOTINFO_MAGIC 0x42554e46u
#define FNU_BOOTINFO_VERSION 3

#define FNU_MEM_MAX 128
#define FNU_MODULE_MAX 4
#define FNU_CMDLINE_MAX 256
#define FNU_MODULE_NAME 16
#define FNU_EFI_RANGE_MAX 96

// e820 and coreboot agree on 1..5
#define FNU_MEM_RAM 1
#define FNU_MEM_RESERVED 2
#define FNU_MEM_ACPI_RECLAIM 3
#define FNU_MEM_ACPI_NVS 4
#define FNU_MEM_UNUSABLE 5

#define FNU_FIRMWARE_BIOS 1
#define FNU_FIRMWARE_COREBOOT 2
#define FNU_FIRMWARE_UEFI 3

#define FNU_CONSOLE_NONE 0
#define FNU_CONSOLE_VGA_TEXT 1
#define FNU_CONSOLE_FRAMEBUFFER 2

#define FNU_FB_SOURCE_DEFAULT 0   // nothing said anything, built-in fallback
#define FNU_FB_SOURCE_EDID 1      // the panel's own preferred timing
#define FNU_FB_SOURCE_FIRMWARE 2  // the mode the bios had already programmed
#define FNU_FB_SOURCE_COREBOOT 3  // coreboot handed over a live framebuffer
#define FNU_FB_SOURCE_BUILD 4     // VBE_WIDTH/VBE_HEIGHT at build time

#define FNU_SERIAL_NONE 0
#define FNU_SERIAL_IO 1
#define FNU_SERIAL_MMIO 2

struct fnu_memrange {
  uint64 base;
  uint64 length;
  uint32 type;
  uint32 pad;
};

struct fnu_framebuffer {
  uint64 base;
  uint32 width;
  uint32 height;
  uint32 pitch;
  uint8 bpp;
  uint8 red_pos;
  uint8 red_size;
  uint8 green_pos;
  uint8 green_size;
  uint8 blue_pos;
  uint8 blue_size;
  uint8 source; // where the size came from, FNU_FB_SOURCE_*
  uint8 pad[4]; // i386 aligns uint64 on 4, keep every struct a multiple of 8
};

struct fnu_serial {
  uint64 base;
  uint32 kind;
  uint32 baud;
  uint32 regwidth;
  uint32 pad;
};

// mapped one to one before any runtime service call: EFI_MEMORY_RUNTIME plus
// the types FreeBSD keeps in EFI_ALLOWED_TYPES_MASK.
#define FNU_EFI_BS_CODE 3
#define FNU_EFI_BS_DATA 4
#define FNU_EFI_RT_CODE 5
#define FNU_EFI_RT_DATA 6
#define FNU_EFI_FIRMWARE 10

#define FNU_EFI_MEMORY_WB 0x8
#define FNU_EFI_MEMORY_RO 0x00020000ULL

struct fnu_efi_range {
  uint64 base;
  uint64 pages;
  uint64 attribute;
  uint32 type;
  uint32 pad;
};

struct fnu_module {
  uint64 base;
  uint64 size;
  char name[FNU_MODULE_NAME];
};

struct fnu_bootinfo {
  uint32 magic;
  uint32 version;
  uint32 size;
  uint32 firmware;

  uint32 memranges;
  uint32 console;
  struct fnu_memrange mem[FNU_MEM_MAX];

  struct fnu_framebuffer fb;
  struct fnu_serial serial;

  uint64 acpi_rsdp;

  uint64 boot_partition_lba;
  uint32 boot_drive;
  uint32 modules;
  struct fnu_module module[FNU_MODULE_MAX];

  uint64 efi_system_table;
  uint64 efi_runtime_services;
  uint32 efi_ranges;
  uint32 pad2;
  struct fnu_efi_range efi_rt[FNU_EFI_RANGE_MAX];

  char cmdline[FNU_CMDLINE_MAX];
};

#endif
