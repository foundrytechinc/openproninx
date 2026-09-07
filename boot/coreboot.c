/*
 * coreboot table walk, adapted from libpayload.
 *
 * Copyright (C) 2008 Advanced Micro Devices, Inc.
 * Copyright (C) 2009 coresystems GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "boot.h"

#define LB_TAG_MEMORY 0x0001
#define LB_TAG_SERIAL 0x000f
#define LB_TAG_FORWARD 0x0011
#define LB_TAG_FRAMEBUFFER 0x0012
#define LB_TAG_ACPI_RSDP 0x0043

#define TABLE_MAX 8192

struct lb_header {
  uint8 sig[4];
  uint32 header_bytes;
  uint32 header_checksum;
  uint32 table_bytes;
  uint32 table_checksum;
  uint32 table_entries;
};

struct lb_record {
  uint32 tag;
  uint32 size;
};

struct lb_range {
  uint64 start;
  uint64 size;
  uint32 type;
};

struct lb_framebuffer {
  uint32 tag;
  uint32 size;
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
  uint8 rsvd_pos;
  uint8 rsvd_size;
  uint8 orientation;
  uint8 flags;
  uint8 pad;
};

struct lb_serial {
  uint32 tag;
  uint32 size;
  uint32 type;
  uint32 baseaddr;
  uint32 baud;
  uint32 regwidth;
};

struct lb_forward {
  uint32 tag;
  uint32 size;
  uint64 forward;
};

struct lb_rsdp {
  uint32 tag;
  uint32 size;
  uint64 pointer;
};

_Static_assert(sizeof(struct lb_header) == 24, "lb_header");
_Static_assert(sizeof(struct lb_range) == 20, "lb_range");
_Static_assert(sizeof(struct lb_framebuffer) == 40, "lb_framebuffer");

static uint8 table[TABLE_MAX];

static uint16 ipchksum(const void *p, uint32 n) {
  const uint8 *b = p;
  uint32 sum = 0, i;

  for (i = 0; i + 1 < n; i += 2)
    sum += (uint32)b[i] | ((uint32)b[i + 1] << 8);
  if (i < n)
    sum += b[i];
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return (uint16)~sum;
}

static int find_header(uint32 base, uint32 len, struct lb_header *out,
                       uint32 *at) {
  uint32 off;

  for (off = 0; off < len; off += 16) {
    flat_copy(out, base + off, sizeof(*out));
    if (out->sig[0] != 'L' || out->sig[1] != 'B' || out->sig[2] != 'I' ||
        out->sig[3] != 'O')
      continue;
    if (out->header_bytes < sizeof(*out) || out->table_bytes > TABLE_MAX)
      continue;
    if (ipchksum(out, sizeof(*out)) != 0)
      continue;
    *at = base + off;
    return 0;
  }
  return -1;
}

static uint32 memtype(uint32 t) {
  return (t >= FNU_MEM_RAM && t <= FNU_MEM_UNUSABLE) ? t : FNU_MEM_RESERVED;
}

static int walk(uint32 at, struct lb_header *h, struct fnu_bootinfo *bi,
                uint32 *forward);

int coreboot_probe(struct fnu_bootinfo *bi) {
  struct lb_header h;
  uint32 at, forward;
  int hops;

  if (find_header(0x00000000, 0x1000, &h, &at) < 0 &&
      find_header(0x000f0000, 0x1000, &h, &at) < 0)
    return 0;

  for (hops = 0; hops < 4; hops++) {
    forward = 0;
    if (walk(at, &h, bi, &forward) < 0)
      return 0;
    if (forward == 0)
      return 1;
    if (find_header(forward, 0x1000, &h, &at) < 0)
      return 0;
  }
  return 0;
}

static int walk(uint32 at, struct lb_header *h, struct fnu_bootinfo *bi,
                uint32 *forward) {
  uint32 off = 0, i;

  flat_copy(table, at + h->header_bytes, h->table_bytes);
  if (ipchksum(table, h->table_bytes) != h->table_checksum)
    return -1;

  for (i = 0;
       i < h->table_entries && off + sizeof(struct lb_record) <= h->table_bytes;
       i++) {
    struct lb_record *r = (struct lb_record *)(table + off);

    if (r->size < sizeof(*r) || off + r->size > h->table_bytes)
      return -1;

    switch (r->tag) {
    case LB_TAG_FORWARD:
      if (r->size < sizeof(struct lb_forward))
        return -1;
      *forward = (uint32)((struct lb_forward *)r)->forward;
      return 0;

    case LB_TAG_MEMORY: {
      uint32 n = (r->size - sizeof(*r)) / sizeof(struct lb_range);
      struct lb_range *m = (struct lb_range *)(table + off + sizeof(*r));
      uint32 k;
      for (k = 0; k < n && bi->memranges < FNU_MEM_MAX; k++) {
        bi->mem[bi->memranges].base = m[k].start;
        bi->mem[bi->memranges].length = m[k].size;
        bi->mem[bi->memranges].type = memtype(m[k].type);
        bi->memranges++;
      }
      break;
    }

    case LB_TAG_FRAMEBUFFER: {
      struct lb_framebuffer *f = (struct lb_framebuffer *)r;
      if (r->size < sizeof(struct lb_framebuffer))
        break;
      if (f->base == 0 || f->width == 0 || f->height == 0)
        break;
      bi->fb.base = f->base;
      bi->fb.width = f->width;
      bi->fb.height = f->height;
      bi->fb.pitch = f->pitch;
      bi->fb.bpp = f->bpp;
      bi->fb.red_pos = f->red_pos;
      bi->fb.red_size = f->red_size;
      bi->fb.green_pos = f->green_pos;
      bi->fb.green_size = f->green_size;
      bi->fb.blue_pos = f->blue_pos;
      bi->fb.blue_size = f->blue_size;
      bi->fb.source = FNU_FB_SOURCE_COREBOOT;
      bi->console = FNU_CONSOLE_FRAMEBUFFER;
      break;
    }

    case LB_TAG_SERIAL: {
      struct lb_serial *s = (struct lb_serial *)r;
      if (r->size < sizeof(struct lb_serial))
        break;
      bi->serial.kind = s->type == 2 ? FNU_SERIAL_MMIO : FNU_SERIAL_IO;
      bi->serial.base = s->baseaddr;
      bi->serial.baud = s->baud;
      bi->serial.regwidth = s->regwidth;
      break;
    }

    case LB_TAG_ACPI_RSDP:
      if (r->size < sizeof(struct lb_rsdp))
        break;
      bi->acpi_rsdp = ((struct lb_rsdp *)r)->pointer;
      break;

    default:
      break;
    }
    off += r->size;
  }
  return 0;
}
