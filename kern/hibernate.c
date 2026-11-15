// HIBERNATE
#include "blockdev.h"
#include "buf.h"
#include "defs.h"
#include "driver.h"
#include "hibernate.h"
#include "inc/bootinfo.h"
#include "kclock.h"
#include "acpi/acpi.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "sleeplock.h"
#include "ulogfmt.h"
#include "usbdi.h"
#include "uvm.h"
#include "lz4.h"

// the piglet
#define PG_IOBUF 0
#define IOBUF MAXPHYS
#define NCHUNK_MAX (HIB_CHUNKTABLE / sizeof(struct hib_chunk))
#define PG_FCHUNKS (PG_IOBUF + IOBUF)
#define PG_LZSTATE (PG_FCHUNKS + PGROUNDUP(NCHUNK_MAX * sizeof(uint32_t)))
#define PG_LZOUT (PG_LZSTATE + PGROUNDUP(LZ4_STREAM_MINSIZE))
#define PG_SKIP (PG_LZOUT + 2 * PGSIZE)
#define PG_SIG (PG_SKIP + PGSIZE)
#define PG_UNPACK (PG_SIG + HIB_SIG_BYTES)
#define PG_STACK (HIB_CHUNK - 8 * PGSIZE)
#define PG_CHUNKS HIB_CHUNK
#define PG_PT (HIB_CHUNK + HIB_CHUNKTABLE)
#define PT_PAGES ((HIB_CHUNK - HIB_CHUNKTABLE) / PGSIZE)
#define PG_BOUNCE (2 * HIB_CHUNK)
#define BOUNCE (2 * HIB_CHUNK)

#define MAX_RLE ((int)(HIB_CHUNK / PGSIZE))
#define SIG_VERSION 6
#define CHUNK_EMPTY 0x1 // no page of it is saved
#define SECT 512
#define GB (1ULL << 30)
#define MB2 (1ULL << 21)
#define SPOOL_ROW (PGSIZE / sizeof(uint64_t))
#define SPOOL_SLACK 256 // pages: the records' own words

// a chunk is records, each a word and what it says
#define REC_TYPE 0xc0000000u
#define REC_SKIP 0x40000000u // so many free pages
#define REC_LZ4 0x80000000u  // a page, LZ4, so many bytes
#define REC_RAW 0xc0000000u  // a page as it is

_Static_assert(sizeof(struct hib_sig) <= HIB_SIG_BYTES, "hib_sig");

static const char sig_magic[8] = {'F', 'N', 'U', 'H', 'I', 'B', 'E', 'R'};

extern char data[];
void hib_switch(uint64_t pml4, uint64_t stack, void (*fn)(void *), void *arg)
    __attribute__((noreturn));
void hib_resume_jump(void) __attribute__((noreturn));

static struct {
  uint dev;
  uint64_t sectors;
  uint8_t *freemap;
  uint64_t pages;
  uint64_t piglet;
  uchar *pv;
  uint64_t **index; // the spool's pages, SPOOL_ROW addresses to an index page
  uint64_t nindex, nspool;
  int spool_ours;   // its pages are still allocated, not freed by the rebuild
  uint64_t lo, end; // the image in fnu-swap
  uint64_t hi;      // and the end of the run it went into
  uint64_t keep;    // pages left free for the devices and the write
  int held;         // and kept from the pager
} hib;

static uint64_t ram_end(void) {
  const struct fnu_bootinfo *bi = bootinfo();
  uint64_t end = 0;
  uint i;

  for (i = 0; i < bi->memranges; i++)
    if ((bi->mem[i].type == FNU_MEM_RAM || bi->mem[i].type == FNU_MEM_ACPI_NVS) &&
        bi->mem[i].base + bi->mem[i].length > end)
      end = bi->mem[i].base + bi->mem[i].length;
  return PGROUNDUP(end);
}

static int describe(struct hib_sig *s) {
  const struct fnu_bootinfo *bi = bootinfo();
  uint i, j, n = 0;

  memset(s, 0, sizeof(*s));
  memmove(s->magic, sig_magic, sizeof(s->magic));
  s->version = SIG_VERSION;
  for (i = 0; i < bi->memranges; i++) {
    uint64_t lo = PGROUNDUP(bi->mem[i].base);
    uint64_t hi = PGROUNDDOWN(bi->mem[i].base + bi->mem[i].length);

    if ((bi->mem[i].type != FNU_MEM_RAM && bi->mem[i].type != FNU_MEM_ACPI_NVS) ||
        hi <= lo)
      continue;
    if (n == HIB_RANGES)
      return -1;
    // in order of address
    for (j = n; j > 0 && s->ranges[j - 1].base > lo; j--)
      s->ranges[j] = s->ranges[j - 1];
    s->ranges[j].base = lo;
    s->ranges[j].end = hi;
    n++;
  }
  s->nranges = n;
  s->piglet_pa = hibernate_piglet();
  s->text_changed = (uint32_t)kernel_text_check("since boot", &s->text_first);
  s->kern_crc = kernel_text_crc();
  for (i = 0; i < bi->efi_ranges; i++)
    if (bi->efi_rt[i].attribute & FNU_EFI_MEMORY_RUNTIME) {
      s->efi_crc = ulog_crc(s->efi_crc, (const uchar *)&bi->efi_rt[i].base,
                            sizeof(bi->efi_rt[i].base));
      s->efi_crc = ulog_crc(s->efi_crc, (const uchar *)&bi->efi_rt[i].pages,
                            sizeof(bi->efi_rt[i].pages));
    }
  return 0;
}

static uint32_t sig_sum(struct hib_sig *s) {
  uint32_t keep = s->crc, sum;

  s->crc = 0;
  sum = ulog_crc(0, (const uchar *)s, sizeof(*s));
  s->crc = keep;
  return sum;
}

static uint64_t tail_start(uint64_t sectors) {
  return sectors - MSGBUF_DUMP_SECTORS - HIB_TAIL_SECTORS;
}

// THE SUSPENDING KERNEL

int hibernate_prepare(void) {
  if ((hib.piglet = hibernate_piglet()) == 0) {
    printf("hibernate: no piglet: less than %d MB of memory\n",
            (int)(HIB_MIN_MEMORY >> 20));
    return -1;
  }
  hib.pv = P2V(hib.piglet);
  if ((hib.dev = storage_swap_device()) == 0) {
    printf("hibernate: the boot disk has no fnu-swap\n");
    return -1;
  }
  hib.sectors = blockdev_device_size(hib.dev);
  if (hib.sectors < (MSGBUF_DUMP_SECTORS + HIB_TAIL_SECTORS) * 4) {
    printf("hibernate: fnu-swap is too small\n");
    return -1;
  }
  hib.pages = ram_end() / PGSIZE;
  if (hib.pages > BOUNCE * 8 ||
      (hib.pages + GB / PGSIZE - 1) / (GB / PGSIZE) > PT_PAGES - 4) {
    printf("hibernate: more memory than the piglet can map\n");
    return -1;
  }
  if ((hib.freemap = contig_alloc((hib.pages + 7) / 8)) == 0) {
    printf("hibernate: no memory for the map of free pages\n");
    return -1;
  }
  return 0;
}

static uint64_t spool_page(uint64_t i) {
  return hib.index[i / SPOOL_ROW][i % SPOOL_ROW];
}

static void spool_free(void) {
  uint64_t i;

  if (hib.spool_ours)
    for (i = 0; i < hib.nspool; i++)
      page_free((char *)P2V(spool_page(i)));
  for (i = 0; i < hib.nindex; i++)
    page_free((char *)hib.index[i]);
  if (hib.index != 0)
    contig_free(hib.index);
  hib.index = 0;
  hib.nindex = hib.nspool = 0;
  hib.spool_ours = 0;
}

// free memory up to the used; bad compression means shrink and retry
int hibernate_reserve(void) {
  uint64_t total = get_total_ram() / PGSIZE, used, zero, want, i;
  int min, targ;
  char *p, *bufbase;

  // the buffer cache goes empty and zeroed, the piglet is not saved
  bufmem_region(&bufbase, &zero);
  zero = zero / PGSIZE + HIB_PIGLET / PGSIZE;
  pageout_targets(&min, &targ);
  hib.keep = 2 * (uint64_t)targ;
  used = total - (uint64_t)kalloc_nfree();
  used = used > zero ? used - zero : 0;
  want = used + used / 64 + zero / 256 + SPOOL_SLACK;
  hib.nindex = (want + SPOOL_ROW - 1) / SPOOL_ROW;
  if ((hib.index = contig_alloc(hib.nindex * sizeof(uint64_t *))) == 0) {
    hib.nindex = 0;
    return -1;
  }
  memset(hib.index, 0, hib.nindex * sizeof(uint64_t *));
  hib.spool_ours = 1;
  for (i = 0; i < want && (uint64_t)kalloc_nfree() > hib.keep; i++) {
    if (hib.index[i / SPOOL_ROW] == 0 &&
        (hib.index[i / SPOOL_ROW] = (uint64_t *)page_alloc()) == 0)
      break;
    if ((p = page_alloc()) == 0)
      break;
    hib.index[i / SPOOL_ROW][i % SPOOL_ROW] = V2P(p);
    hib.nspool = i + 1;
  }
  for (i = 0; i < hib.nindex && hib.index[i] != 0; i++)
    ;
  hib.nindex = i;
  return hib.nspool > 0 ? 0 : -1;
}

// overran: page that much out, if swap holds it
int hibernate_shrink(void) {
  uint64_t need, goal, total, used, outs, ins, swapfree;

  spool_free();
  need = (hib.end - hib.lo) * SECT / PGSIZE;
  need += need / 16 + SPOOL_SLACK;
  goal = need + hib.keep;
  swap_stats(&total, &used, &outs, &ins);
  swapfree = (total - used) / PGSIZE;
  if (goal <= (uint64_t)kalloc_nfree())
    return 0;
  if (goal - (uint64_t)kalloc_nfree() + need > swapfree) {
    printf("hibernate: %lu MB paged out and the image besides want more than "
           "the %lu MB of fnu-swap free\n",
           (unsigned long)((goal - (uint64_t)kalloc_nfree()) >> 8),
           (unsigned long)(swapfree >> 8));
    return 0;
  }
  printf("hibernate: paging out %lu MB to make room\n",
         (unsigned long)((goal - (uint64_t)kalloc_nfree()) >> 8));
  return uvm_hibernate_shrink((int)goal);
}

void hibernate_unprepare(void) {
  if (hib.held)
    swap_hib_hold(hib.dev, hib.lo, hib.end, 0);
  hib.held = 0;
  spool_free();
  if (hib.freemap != 0)
    contig_free(hib.freemap);
  hib.freemap = 0;
}

static int is_free(uint64_t pa) {
  uint64_t pg = pa / PGSIZE;

  if (pa >= hib.piglet && pa < hib.piglet + HIB_PIGLET)
    return 1;
  return pg < hib.pages && (hib.freemap[pg / 8] & (1 << (pg % 8)));
}

static int rle_at(uint64_t pa, uint64_t end) {
  int n = 0;

  while (pa < end && n < MAX_RLE && is_free(pa)) {
    pa += PGSIZE;
    n++;
  }
  return n;
}

struct writer {
  uint64_t blk, hi;
  uint used;        // bytes of the buffer
};

// into the spool at its place in fnu-swap; past the end only counted
static int w_put(struct writer *w, uint bytes) {
  uint n = (bytes + SECT - 1) / SECT * SECT;
  uint64_t off = (w->blk - hib.lo) * SECT, k, at;

  if (bytes == 0)
    return 0;
  if (n > bytes)
    memset(hib.pv + PG_IOBUF + bytes, 0, n - bytes);
  for (k = 0; w->blk + n / SECT <= w->hi && off + n <= hib.nspool * PGSIZE &&
              k < n;
       k += at) {
    at = PGSIZE - (off + k) % PGSIZE;
    if (at > n - k)
      at = n - k;
    memmove((uchar *)P2V(spool_page((off + k) / PGSIZE)) + (off + k) % PGSIZE,
            hib.pv + PG_IOBUF + k, at);
  }
  w->blk += n / SECT;
  return 0;
}

static int w_bytes(struct writer *w, const void *p, uint n) {
  const uchar *from = p;
  uint at;
  int e;

  while (n > 0) {
    at = IOBUF - w->used < n ? IOBUF - w->used : n;
    memmove(hib.pv + PG_IOBUF + w->used, from, at);
    w->used += at;
    from += at;
    n -= at;
    if (w->used == IOBUF) {
      if ((e = w_put(w, IOBUF)) < 0)
        return e;
      w->used = 0;
    }
  }
  return 0;
}

// a page on its own
static int w_page(struct writer *w, const void *page) {
  uchar *out = hib.pv + PG_LZOUT;
  uint32_t rec;
  int n, e;

  n = LZ4_compress_fast_extState(hib.pv + PG_LZSTATE, (const char *)page,
                                 (char *)out, PGSIZE, LZ4_COMPRESSBOUND(PGSIZE),
                                 1);
  if (n <= 0 || n >= PGSIZE) {
    rec = REC_RAW;
    if ((e = w_bytes(w, &rec, sizeof(rec))) < 0)
      return e;
    return w_bytes(w, page, PGSIZE);
  }
  rec = REC_LZ4 | (uint32_t)n;
  if ((e = w_bytes(w, &rec, sizeof(rec))) < 0)
    return e;
  return w_bytes(w, out, (uint)n);
}

static int write_chunks(struct hib_sig *s, struct writer *w) {
  struct hib_chunk *c = (struct hib_chunk *)(hib.pv + PG_CHUNKS);
  uint64_t n = 0, i, pa, start = w->blk;
  uint32_t rec;
  uint r;
  int e, skip;

  memset(c, 0, HIB_CHUNKTABLE);
  for (r = 0; r < s->nranges; r++)
    // HIB_CHUNK boundaries. the resume reads it that way. ???
    for (pa = s->ranges[r].base; pa < s->ranges[r].end; pa = c[n++].end) {
      if (n == NCHUNK_MAX)
        return -1;
      c[n].base = pa;
      c[n].end = (pa & ~(HIB_CHUNK - 1)) + HIB_CHUNK;
      if (c[n].end > s->ranges[r].end)
        c[n].end = s->ranges[r].end;
    }
  s->nchunks = n;
  for (i = 0; i < n; i++) {
    c[i].offset = w->blk - start;
    c[i].flags = CHUNK_EMPTY;
    w->used = 0;
    for (pa = c[i].base; pa < c[i].end;) {
      if ((skip = rle_at(pa, c[i].end)) != 0) {
        rec = REC_SKIP | (uint32_t)skip;
        if ((e = w_bytes(w, &rec, sizeof(rec))) < 0)
          return e;
        pa += (uint64_t)skip * PGSIZE;
        continue;
      }
      if ((e = w_page(w, P2V(pa))) < 0)
        return e;
      c[i].flags = 0;
      pa += PGSIZE;
    }
    if ((e = w_put(w, w->used)) < 0)
      return e;
    c[i].csize = (w->blk - start - c[i].offset) * SECT;
  }
  s->image_offset = start;
  s->image_sectors = w->blk - start;
  return 0;
}

// interrupts off, the image goes in the spool
int hibernate_suspend(void) {
  struct hib_sig *s = (struct hib_sig *)(hib.pv + PG_SIG);
  struct writer w;
  uint64_t lo, hi, tail = tail_start(hib.sectors), i, pg;
  int e;

  if (describe(s) < 0)
    return -1;
  bio_hibernate();
  contig_zero_free();
  kalloc_freemap(hib.freemap, hib.pages);
  for (i = 0; i < hib.nspool; i++)
    if ((pg = spool_page(i) / PGSIZE) < hib.pages)
      hib.freemap[pg / 8] |= (uint8_t)(1 << (pg % 8));
  if (swap_hib_region(hib.dev, tail, &lo, &hi) < 0)
    lo = hi = 0;
  hib.lo = w.blk = lo;
  hib.hi = w.hi = hi;
  w.used = 0;
  e = write_chunks(s, &w);
  hib.end = w.blk;
  if (e < 0)
    return e;
  if (hib.end > hib.hi)
    return HIB_NOSWAP;
  if ((hib.end - hib.lo) * SECT > hib.nspool * PGSIZE)
    return HIB_NOMEM;
  s->chunktable_offset = tail;
  s->crc = sig_sum(s);
  swap_hib_hold(hib.dev, hib.lo, hib.end, 1);
  hib.held = 1;
  return 0;
}

static int raw_io(uint dev, uint64_t sector, void *data, uint bytes, int write) {
  static struct buf b;

  memset(&b, 0, sizeof(b));
  initsleeplock(&b.lock, "hibernate");
  acquiresleep(&b.lock);
  b.dev = dev;
  b.blockno = sector;
  b.addr = data;
  b.bcount = bytes;
  b.flags = write ? B_DIRTY : 0;
  blockdev_rw(&b);
  releasesleep(&b.lock);
  return (b.flags & B_ERROR) ? -1 : 0;
}

static int write_from(uint64_t sector, uchar *p, uint64_t bytes, uint max) {
  uint64_t done;
  uint n;

  for (done = 0; done < bytes; done += n) {
    n = bytes - done > max ? max : (uint)(bytes - done);
    if (raw_io(hib.dev, sector + done / SECT, p + done, n, 1) < 0)
      return -1;
  }
  return 0;
}

// devices up, written through the block layer; the signature goes last
int hibernate_write(void) {
  struct hib_sig *s = (struct hib_sig *)(hib.pv + PG_SIG);
  uchar *buf = hib.pv + PG_IOBUF;
  uint64_t bytes = (hib.end - hib.lo) * SECT, done, k, at;
  uint max = blockdev_maxphys(hib.dev) / SECT * SECT, n, said = 0, pct;

  if (max == 0 || max > IOBUF)
    max = IOBUF;
  printf("hibernate: writing %lu MB of image\n", (unsigned long)(bytes >> 20));
  for (done = 0; done < bytes; done += n) {
    n = bytes - done > max ? max : (uint)(bytes - done);
    for (k = 0; k < n; k += at) {
      at = PGSIZE - (done + k) % PGSIZE;
      if (at > n - k)
        at = n - k;
      memmove(buf + k,
              (uchar *)P2V(spool_page((done + k) / PGSIZE)) + (done + k) % PGSIZE, at);
    }
    if (raw_io(hib.dev, hib.lo + done / SECT, buf, n, 1) < 0)
      return -1;
    // progress, every tenth
    if ((pct = (uint)((done + n) * 100 / bytes)) >= said + 10) {
      said = pct - pct % 10;
      printf("hibernate: %u%% written\n", said);
    }
  }
  s->asleep_wall = acpi_pm_trace_on() ? time_second() : 0;
  s->crc = sig_sum(s);
  if (write_from(s->chunktable_offset, hib.pv + PG_CHUNKS, HIB_CHUNKTABLE, max) < 0 ||
      blockdev_flush(hib.dev) < 0 ||
      write_from(s->chunktable_offset + HIB_CHUNKTABLE / SECT, hib.pv + PG_SIG,
                 HIB_SIG_BYTES, max) < 0 ||
      blockdev_flush(hib.dev) < 0)
    return -1;
  return 0;
}

void hibernate_made(void) {
  printf("hibernate: the image is made, %lu MB\n",
         (unsigned long)(((hib.end - hib.lo) * SECT) >> 20));
}

// why there is no image, said once the console is back
void hibernate_explain(int err) {
  if (err == HIB_NOSWAP)
    printf("hibernate: the image is %lu MB, the longest free run of fnu-swap "
           "%lu MB\n",
           (unsigned long)(((hib.end - hib.lo) * SECT) >> 20),
           (unsigned long)(((hib.hi - hib.lo) * SECT) >> 20));
  else if (err == HIB_NOMEM)
    printf("hibernate: the image is %lu MB, %lu MB was set aside for it\n",
           (unsigned long)(((hib.end - hib.lo) * SECT) >> 20),
           (unsigned long)(hib.nspool >> 8));
  else
    printf("hibernate: the image could not be made\n");
}

// the resuming kernel left the signature in the piglet
void hibernate_resumed(void) {
  const struct hib_sig *s = (const struct hib_sig *)(hib.pv + PG_SIG);

  if (s->asleep_wall != 0)
    clock_asleep_at(s->asleep_wall);
  kalloc_rebuild(hib.freemap, hib.pages);
  hib.spool_ours = 0;
}

// THE RESUMING KERNEL

static int read_to(uint dev, uint64_t sector, uint64_t pa, uint64_t bytes) {
  uchar *bounce = P2V(hibernate_piglet() + PG_BOUNCE);
  uint max = blockdev_maxphys(dev);
  uint64_t done;

  max = max / SECT * SECT;
  if (max == 0 || max > BOUNCE)
    max = MAXPHYS;
  for (done = 0; done < bytes;) {
    uint n = bytes - done > max ? max : (uint)(bytes - done);
    uint io = (n + SECT - 1) / SECT * SECT;

    if (raw_io(dev, sector + done / SECT, bounce, io, 0) < 0)
      return -1;
    memmove(P2V(pa + done), bounce, n);
    done += n;
  }
  return 0;
}

struct unpack {
  uint64_t piglet;
  uint64_t image;
  uint64_t nchunks;
};

static uint64_t resume_pt(uint64_t piglet, uint64_t pages) {
  uchar *pt = P2V(piglet + PG_PT);
  uint64_t *pml4 = (uint64_t *)pt, *lo = (uint64_t *)(pt + PGSIZE),
           *hi = (uint64_t *)(pt + 2 * PGSIZE), *pdk = (uint64_t *)(pt + 3 * PGSIZE);
  uint64_t base = piglet + PG_PT, gbs = (pages * PGSIZE + GB - 1) / GB, g, i;

  memset(pt, 0, (4 + gbs) * PGSIZE);
  pml4[0] = (base + PGSIZE) | PTE_P | PTE_W;
  pml4[511] = (base + 2 * PGSIZE) | PTE_P | PTE_W;
  hi[510] = (base + 3 * PGSIZE) | PTE_P | PTE_W;
  for (i = 0; i < 512; i++)
    pdk[i] = (i * MB2) | PTE_P | PTE_W | PTE_PS;
  for (g = 0; g < gbs; g++) {
    uint64_t *pd = (uint64_t *)(pt + (4 + g) * PGSIZE);

    lo[g] = (base + (4 + g) * PGSIZE) | PTE_P | PTE_W;
    for (i = 0; i < 512; i++)
      pd[i] = (g * GB + i * MB2) | PTE_P | PTE_W | PTE_PS;
  }
  return base;
}

static void stop(void) {
  for (;;)
    __asm __volatile("cli; hlt");
}

static void unpack_chunk(uint64_t piglet, const struct hib_chunk *c) {
  uchar *pv = (uchar *)piglet, *p = pv + PG_BOUNCE, *end = p + c->csize, *out;
  uint64_t dest = c->base;
  uint32_t rec, n;

  while (dest < c->end) {
    if ((uint64_t)(end - p) < sizeof(rec))
      stop();
    memmove(&rec, p, sizeof(rec));
    p += sizeof(rec);
    n = rec & ~REC_TYPE;
    if ((rec & REC_TYPE) == REC_SKIP) {
      if (n == 0 || n > (uint32_t)MAX_RLE)
        stop();
      dest += (uint64_t)n * PGSIZE;
      continue;
    }
    if (dest >= piglet && dest < piglet + HIB_PIGLET)
      out = pv + PG_SKIP;
    else
      out = (uchar *)(dest < GB ? KERNBASE + dest : dest);
    // lz4 scribbles ahead and fixes it later
    // unpack aside or we eat our own code. хуй знает почему так
    if ((rec & REC_TYPE) == REC_RAW && (uint64_t)(end - p) >= PGSIZE) {
      memmove(out, p, PGSIZE);
      p += PGSIZE;
    } else if ((rec & REC_TYPE) == REC_LZ4 && n < PGSIZE &&
               (uint64_t)(end - p) >= n &&
               LZ4_decompress_safe((const char *)p, (char *)(pv + PG_LZOUT),
                                   (int)n, PGSIZE) == PGSIZE) {
      memmove(out, pv + PG_LZOUT, PGSIZE);
      p += n;
    } else
      stop();
    dest += PGSIZE;
  }
}

static void unpack(void *arg) {
  struct unpack *u = arg;
  uchar *pv = (uchar *)u->piglet;
  struct hib_chunk *c = (struct hib_chunk *)(pv + PG_CHUNKS);
  uint32_t *order = (uint32_t *)(pv + PG_FCHUNKS);
  uint64_t img = u->image, i;

  for (i = 0; i < u->nchunks; i++) {
    const struct hib_chunk *ch = &c[order[i]];

    memmove(pv + PG_BOUNCE, (void *)img, ch->csize);
    unpack_chunk(u->piglet, ch);
    img += ch->csize;
  }
  hib_resume_jump();
}

// the highest run of npages set in map
static int top_run(const uint8_t *map, uint64_t pages, uint64_t npages,
                   uint64_t *pa) {
  uint64_t pg, run = 0;

  for (pg = pages; pg-- > 0;) {
    if (!(map[pg / 8] & (1 << (pg % 8)))) {
      run = 0;
      continue;
    }
    if (++run == npages) {
      *pa = pg * PGSIZE;
      return 0;
    }
  }
  return -1;
}

// the pig. keep it off unread image
static int find_pig(const struct hib_chunk *c, uint64_t nchunks, uint64_t pages,
                    uint64_t npages, uint64_t *pa) {
  uint8_t *map = P2V(hibernate_piglet() + PG_BOUNCE);
  uint64_t i, pg;
  int tries;

  for (tries = 0; tries < 4; tries++) {
    kalloc_freemap(map, pages);
    for (i = 0; i < nchunks; i++)
      for (pg = c[i].base / PGSIZE; !(c[i].flags & CHUNK_EMPTY) &&
                                    pg < c[i].end / PGSIZE && pg < pages;
           pg++)
        map[pg / 8] &= (uint8_t)~(1 << (pg % 8));
    if (top_run(map, pages, npages, pa) < 0) {
      kalloc_freemap(map, pages);
      if (top_run(map, pages, npages, pa) < 0)
        return -1;
    }
    if (kalloc_claim(*pa, npages) == 0)
      return 0;
  }
  return -1;
}

// order matters: a chunk may only trample the ones after it
static int unpack_order(const struct hib_chunk *c, uint64_t nchunks,
                        uint32_t *order, uint64_t image_start,
                        uint64_t image_end) {
  uint64_t i, n = 0, at = image_start;

  for (i = 0; i < nchunks; i++)
    if (c[i].end <= image_start || c[i].base >= image_end)
      order[n++] = (uint32_t)i;
  for (i = 0; i < nchunks; i++)
    if (!(c[i].end <= image_start || c[i].base >= image_end))
      order[n++] = (uint32_t)i;
  for (i = 0; i < n; i++) {
    const struct hib_chunk *ch = &c[order[i]];

    at += ch->csize;
    if (!(ch->flags & CHUNK_EMPTY) && ch->base < image_end && ch->end > at)
      return -1;
  }
  return 0;
}

static int same_machine(const struct hib_sig *disk, const struct hib_sig *mine) {
  uint i;

  if (disk->nranges != mine->nranges) {
    printf("hibernate: the memory changed since the image was written\n");
    return 0;
  }
  for (i = 0; i < mine->nranges; i++)
    if (disk->ranges[i].base != mine->ranges[i].base ||
        disk->ranges[i].end != mine->ranges[i].end) {
      printf("hibernate: the memory changed since the image was written\n");
      return 0;
    }
  if (disk->kern_crc != mine->kern_crc) {
    printf("hibernate: the image is of another kernel (crc 0x%x, this one 0x%x)\n",
           disk->kern_crc, mine->kern_crc);
    if (disk->text_changed != 0)
      printf("hibernate: the kernel that wrote it had %u pieces of its text changed, the "
             "first at 0x%lx\n",
             disk->text_changed, (unsigned long)disk->text_first);
    return 0;
  }
  if (disk->efi_crc != mine->efi_crc) {
    printf("hibernate: the firmware's runtime is elsewhere now\n");
    return 0;
  }
  if (disk->piglet_pa != mine->piglet_pa) {
    printf("hibernate: the piglet is at 0x%lx, the image wants it at 0x%lx\n",
            mine->piglet_pa, disk->piglet_pa);
    return 0;
  }
  return 1;
}

void hibernate_resume(uint dev) {
  static union {
    struct hib_sig s;
    uchar b[HIB_SIG_BYTES];
  } block __aligned(512);
  static struct hib_sig disk, mine;
  uint64_t piglet = hibernate_piglet(), sectors, tail, total = 0, pig, pages;
  uint64_t pig_pages, pig_end, image_end, img, i;
  struct hib_chunk *c;
  uint32_t *order;
  struct unpack *u;

  if (dev == 0 || piglet == 0 ||
      (sectors = blockdev_device_size(dev)) < (MSGBUF_DUMP_SECTORS + HIB_TAIL_SECTORS) * 4)
    return;
  tail = tail_start(sectors);
  if (raw_io(dev, tail + HIB_CHUNKTABLE / SECT, &block, sizeof(block), 0) < 0 ||
      memcmp(block.s.magic, sig_magic, sizeof(sig_magic)) != 0)
    return;
  disk = block.s;
  memset(&block, 0, sizeof(block));
  if (raw_io(dev, tail + HIB_CHUNKTABLE / SECT, &block, sizeof(block), 1) < 0)
    printf("hibernate: the signature could not be cleared\n");
  if (disk.version != SIG_VERSION || disk.crc != sig_sum(&disk) ||
      disk.nchunks > NCHUNK_MAX) {
    printf("hibernate: the image's signature is damaged\n");
    return;
  }
  if (describe(&mine) < 0 || !same_machine(&disk, &mine))
    return;

  c = (struct hib_chunk *)P2V(piglet + PG_CHUNKS);
  order = (uint32_t *)P2V(piglet + PG_FCHUNKS);
  if (read_to(dev, disk.chunktable_offset, piglet + PG_CHUNKS, HIB_CHUNKTABLE) < 0)
    return;
  for (i = 0; i < disk.nchunks; i++) {
    if (c[i].csize > BOUNCE || total + c[i].csize < total)
      return;
    total += c[i].csize;
  }
  pages = ram_end() / PGSIZE;
  pig_pages = PGROUNDUP(total + HIB_CHUNK) / PGSIZE;
  if (pages > BOUNCE * 8 ||
      (pages + GB / PGSIZE - 1) / (GB / PGSIZE) > PT_PAGES - 4 ||
      find_pig(c, disk.nchunks, pages, pig_pages, &pig) < 0) {
    printf("hibernate: no room to read the image into\n");
    return;
  }
  pig_end = pig + pig_pages * PGSIZE;
  image_end = pig_end & ~(HIB_CHUNK - 1);
  if (unpack_order(c, disk.nchunks, order, image_end - total, image_end) < 0) {
    printf("hibernate: the image would be unpacked over itself\n");
    for (i = 0; i < pig_pages; i++)
      page_free((char *)P2V(pig + i * PGSIZE));
    return;
  }
  printf("hibernate: reading %d MB of image\n", (int)(total >> 20));

  img = image_end - total;
  for (i = 0; i < disk.nchunks; i++) {
    const struct hib_chunk *ch = &c[order[i]];

    if (read_to(dev, disk.image_offset + ch->offset, img, ch->csize) < 0) {
      printf("hibernate: the image could not be read\n");
      for (i = 0; i < pig_pages; i++)
        page_free((char *)P2V(pig + i * PGSIZE));
      return;
    }
    img += ch->csize;
  }

  printf("hibernate: unpacking\n");
  // like acpi_sleep: devices sleep, cpus still run
  sched_peg_curproc(&cpus[0]);
  usbd_suspend();
  driver_suspend_all();
  cpu_park_others();
  memmove(P2V(piglet + PG_SIG), &disk, sizeof(disk));
  u = (struct unpack *)P2V(piglet + PG_UNPACK);
  u->piglet = piglet;
  u->image = image_end - total;
  u->nchunks = disk.nchunks;
  spinlock_enter();
  lapic_suspend();
  hib_switch(resume_pt(piglet, pages), piglet + PG_STACK + 8 * PGSIZE - 16, unpack,
             (void *)(piglet + PG_UNPACK));
}
