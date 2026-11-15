// PARTITIONS

#include "blockdev.h"
#include "buf.h"
#include "defs.h"
#include "devfs.h"
#include "inc/abi.h"
#include "inc/stat.h"
#include "param.h"
#include "sleeplock.h"
#include "mutex.h"
#include "malloc.h"

#define PART_FIRST 16
#define PART_MAX 256
#define EBR_HOPS PART_MAX
#define GPT_HEADER_LBA 1
#define GPT_SIGNATURE "EFI PART"
#define MBR_PROTECTIVE 0xEE

struct partition {
  uint parent;
  uint64_t first_lba;
  uint64_t sectors;
  char uuid[37];
  char type[37];
  char label[37];
  int index;
  int used;
  int seen; // seen again by the probe
  int held; // gone, but a mount or the swap still names it: not given again
};

static struct partition *parts;
static uint nparts;
static struct mutex partlock;
static int part_major;

static uint64_t part_size(uint unit) {
  uint64_t sectors = 0;

  if (unit >= nparts)
    return 0;
  mtx_enter(&partlock);
  if (parts[unit].used)
    sectors = parts[unit].sectors;
  mtx_leave(&partlock);
  return sectors;
}

static int part_removable(uint unit) {
  uint parent = 0;

  if (unit >= nparts)
    return 0;
  mtx_enter(&partlock);
  if (parts[unit].used)
    parent = parts[unit].parent;
  mtx_leave(&partlock);
  return parent != 0 ? blockdev_removable(parent) : 0;
}

static void part_strategy(struct buf *b) {
  uint unit = minor(b->dev);
  uint parent;
  uint64_t first, sectors;
  struct buf raw;

  if (unit >= nparts) {
    b->flags |= B_ERROR;
    return;
  }
  mtx_enter(&partlock);
  parent = parts[unit].parent;
  first = parts[unit].first_lba;
  sectors = parts[unit].sectors;
  if (!parts[unit].used)
    sectors = 0;
  mtx_leave(&partlock);

  if (sectors == 0 || b->blockno >= sectors ||
      b->blockno + bufcount(b) / BSIZE > sectors) {
    b->flags |= B_ERROR;
    return;
  }

  memset(&raw, 0, sizeof(raw));
  initsleeplock(&raw.lock, "partition");
  acquiresleep(&raw.lock);
  raw.flags = b->flags & B_DIRTY;
  raw.dev = parent;
  raw.blockno = first + b->blockno;
  raw.addr = bufdata(b);
  raw.bcount = bufcount(b);
  blockdev_rw(&raw);
  b->flags = (b->flags & ~(B_DIRTY | B_VALID | B_ERROR)) |
             (raw.flags & (B_DIRTY | B_VALID | B_ERROR));
  releasesleep(&raw.lock);
}

static uint part_maxphys(uint unit) {
  uint parent;

  if (unit >= nparts)
    return BSIZE;
  mtx_enter(&partlock);
  parent = parts[unit].used ? parts[unit].parent : 0;
  mtx_leave(&partlock);
  return parent == 0 ? BSIZE : blockdev_maxphys(parent);
}

static uint part_parent(uint unit) {
  uint parent;

  if (unit >= nparts)
    return 0;
  mtx_enter(&partlock);
  parent = parts[unit].used ? parts[unit].parent : 0;
  mtx_leave(&partlock);
  return parent;
}

static int part_memory(uint unit) {
  uint parent = part_parent(unit);

  return parent != 0 && blockdev_memory(parent);
}

static int part_flush(uint unit) {
  uint parent = part_parent(unit);

  return parent == 0 ? -1 : blockdev_flush(parent);
}

static struct blockdrv part_blockdrv = {
    .name = "partition",
    .prefix = 0,
    .units = PART_FIRST,
    .strategy = part_strategy,
    .maxphys = part_maxphys,
    .size = part_size,
    .removable = part_removable,
    .flush = part_flush,
    .memory = part_memory,
};

static uint32_t read32(const uchar *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t read64(const uchar *p) {
  return (uint64_t)read32(p) | ((uint64_t)read32(p + 4) << 32);
}

// the GPT crc
static uint32_t crc32_run(const uchar *data, uint len, uint32_t running) {
  uint i, k;

  for (i = 0; i < len; i++) {
    running ^= data[i];
    for (k = 0; k < 8; k++)
      running = (running >> 1) ^ (0xEDB88320u & (uint32_t) - (int32_t)(running & 1));
  }
  return running;
}

static uint32_t crc32(const uchar *data, uint len, uint32_t seed) {
  return ~crc32_run(data, len, ~seed);
}

static uint32_t crc32_more(const uchar *data, uint len, uint32_t sofar) {
  return ~crc32_run(data, len, ~sofar);
}

static int is_zero(const uchar *p, uint n) {
  uint i;

  for (i = 0; i < n; i++)
    if (p[i] != 0)
      return 0;
  return 1;
}

// partlock held
static int grow(void) {
  uint n = nparts * 2 > PART_MAX ? PART_MAX : nparts * 2;
  struct partition *p;

  if (n <= nparts ||
      (p = malloc(n * sizeof(*p), M_DEVBUF, M_NOWAIT | M_ZERO)) == 0)
    return -1;
  memmove(p, parts, nparts * sizeof(*p));
  free(parts, M_DEVBUF);
  parts = p;
  nparts = n;
  part_blockdrv.units = n;
  return 0;
}

static const int guid_order[16] = {3, 2, 1, 0, 5, 4, 7, 6,
                                   8, 9, 10, 11, 12, 13, 14, 15};

static void guid_text(const uchar *raw, char *out) {
  static const char hex[] = "0123456789abcdef";
  int i, n = 0;

  for (i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      out[n++] = '-';
    out[n++] = hex[raw[guid_order[i]] >> 4];
    out[n++] = hex[raw[guid_order[i]] & 15];
  }
  out[n] = 0;
}

// UTF-16
static void label_text(const uchar *raw, char *out, int size) {
  int i, n = 0;
  char c;

  for (i = 0; i < 36 && n < size - 1; i++) {
    if (raw[i * 2] == 0 && raw[i * 2 + 1] == 0)
      break;
    if (raw[i * 2 + 1] != 0)
      c = '_';
    else
      c = (char)raw[i * 2];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '@' || c == '+' ||
          c == '.'))
      c = '_';
    out[n++] = c;
  }
  out[n] = 0;
}

static void alias(uint device, const char *dir, const char *what, int unique) {
  if (what[0] != 0 &&
      devfs_add_alias(dir, what, FNU_TYPE_DISK, (short)major(device),
                      (short)minor(device), 0640) == 0 && unique)
    printf("partition: %s/%s is on another disk too; it names the first\n",
            dir, what);
}

static void unalias(uint device, const char *dir, const char *what) {
  devfs_remove_alias(dir, what, (short)major(device), (short)minor(device));
}

// already here and unchanged
static int same_as_before(uint parent, uint64_t first, uint64_t sectors,
                          const char *uuid) {
  int unit;

  for (unit = 0; unit < (int)nparts; unit++) {
    if (!parts[unit].used || parts[unit].parent != parent)
      continue;
    if (parts[unit].first_lba != first || parts[unit].sectors != sectors)
      continue;
    if (strncmp((char *)parts[unit].uuid, (char *)uuid,
                sizeof(parts[unit].uuid)) != 0)
      continue;
    return unit;
  }
  return -1;
}

static void claim(uint parent, int index, uint64_t first, uint64_t sectors,
                  const uchar *entry) {
  char disk[DEVFS_NAME_MAX + 1];
  char name[DEVFS_NAME_MAX + 1];
  char uuid[37];
  uint64_t whole = blockdev_device_size(parent);
  uint device;
  int unit, n;

  if (sectors == 0 || first == 0 || first >= whole || sectors > whole - first)
    return;
  if (blockdev_name(parent, disk, sizeof(disk)) < 0)
    return;

  uuid[0] = 0;
  if (entry != 0)
    guid_text(entry + 16, uuid);

  mtx_enter(&partlock);
  if ((unit = same_as_before(parent, first, sectors, uuid)) >= 0) {
    parts[unit].seen = 1;
    mtx_leave(&partlock);
    return;
  }
  for (unit = 0; unit < (int)nparts; unit++)
    if (!parts[unit].used && !parts[unit].held)
      break;
  if (unit == (int)nparts && grow() < 0) {
    mtx_leave(&partlock);
    printf("partition: no room for %sp%d\n", disk, index);
    return;
  }
  memset(&parts[unit], 0, sizeof(parts[unit]));
  parts[unit].parent = parent;
  parts[unit].first_lba = first;
  parts[unit].sectors = sectors;
  strlcpy(parts[unit].uuid, uuid, sizeof(parts[unit].uuid));
  parts[unit].index = index;
  if (entry != 0) {
    guid_text(entry, parts[unit].type);
    label_text(entry + 56, parts[unit].label, sizeof(parts[unit].label));
  }
  parts[unit].used = 1;
  parts[unit].seen = 1;
  mtx_leave(&partlock);

  for (n = 0; disk[n] != 0 && n < DEVFS_NAME_MAX - 4; n++)
    name[n] = disk[n];
  name[n++] = 'p';
  if (index >= 100)
    name[n++] = (char)('0' + index / 100);
  if (index >= 10)
    name[n++] = (char)('0' + index / 10 % 10);
  name[n++] = (char)('0' + index % 10);
  name[n] = 0;
  device = makedev((uint)part_major, (uint)unit);
  blockdev_publish_as(device, name);
  alias(device, "part-by-uuid", parts[unit].uuid, 1);
  alias(device, "part-by-label", parts[unit].label, 0);
  blockdev_serno(device, parent, index);
}

static int read_header(struct blockdev *disk, uint64_t lba, uchar *header) {
  uint32_t size, stored, computed;

  if (blockdev_read(disk, lba * FNU_BLOCKDEV_SECTOR_SIZE, header,
                    FNU_BLOCKDEV_SECTOR_SIZE) < 0)
    return -1;
  if (memcmp(header, GPT_SIGNATURE, 8) != 0)
    return -1;
  size = read32(header + 12);
  if (size < 92 || size > FNU_BLOCKDEV_SECTOR_SIZE)
    return -1;
  if (read64(header + 24) != lba)
    return -1;
  stored = read32(header + 16);
  memset(header + 16, 0, 4);
  computed = crc32(header, size, 0);
  memset(header + 16, 0, 4);
  if (stored != computed)
    return -1;
  return 0;
}

// the array crc
static int check_entries(struct blockdev *disk, uint64_t entry_lba,
                         uint32_t count, uint32_t size, uint32_t want) {
  uchar entry[128];
  uint32_t crc = 0;
  uint32_t i;

  if (size != sizeof(entry))
    return -1;
  for (i = 0; i < count; i++) {
    if (blockdev_read(disk, entry_lba * FNU_BLOCKDEV_SECTOR_SIZE + i * size,
                      entry, sizeof(entry)) < 0)
      return -1;
    crc = i == 0 ? crc32(entry, sizeof(entry), 0)
                 : crc32_more(entry, sizeof(entry), crc);
  }
  return crc == want ? 0 : -1;
}

static int load_table(struct blockdev *disk, uint64_t lba, uchar *header,
                      uint64_t *entry_lba, uint32_t *count, uint32_t *size) {
  if (read_header(disk, lba, header) < 0)
    return -1;
  *entry_lba = read64(header + 72);
  *count = read32(header + 80);
  *size = read32(header + 84);
  if (*size < 128 || *size > 4096 || *count == 0 || *count > 128)
    return -1;
  if (*entry_lba == 0 || *entry_lba >= disk->sector_count)
    return -1;
  return check_entries(disk, *entry_lba, *count, *size, read32(header + 88));
}

static int scan_gpt(uint parent) {
  struct blockdev disk;
  uchar header[FNU_BLOCKDEV_SECTOR_SIZE];
  uchar entry[128];
  uint64_t entry_lba, first, last, sectors;
  uint32_t count, size;
  uint i, found = 0;

  sectors = blockdev_device_size(parent);
  disk.device = parent;
  disk.first_lba = 0;
  disk.sector_count = sectors;

  if (load_table(&disk, GPT_HEADER_LBA, header, &entry_lba, &count, &size) <
      0) {
    if (sectors == 0 ||
        load_table(&disk, sectors - 1, header, &entry_lba, &count, &size) < 0)
      return -1;
    printf("partition: the primary GPT does not hold up, using the backup\n");
  }

  for (i = 0; i < count; i++) {
    if (blockdev_read(&disk, entry_lba * FNU_BLOCKDEV_SECTOR_SIZE + i * size,
                      entry, sizeof(entry)) < 0)
      break;
    if (is_zero(entry, 16))
      continue;
    first = read64(entry + 32);
    last = read64(entry + 40);
    if (last < first)
      continue;
    claim(parent, (int)i + 1, first, last - first + 1, entry);
    found++;
  }
  return found > 0 ? 0 : -1;
}

#define MBR_EXTENDED 0x05
#define MBR_EXTENDED_LBA 0x0F
#define MBR_EXTENDED_LINUX 0x85

static int mbr_extended(uchar type) {
  return type == MBR_EXTENDED || type == MBR_EXTENDED_LBA ||
         type == MBR_EXTENDED_LINUX;
}

static int scan_ebr(uint parent, struct blockdev *disk, uint64_t ext_lba,
                    uint64_t ext_sectors) {
  uchar sector[FNU_BLOCKDEV_SECTOR_SIZE];
  uint64_t at = ext_lba, next;
  const uchar *e;
  int index = 5, found = 0, hops;

  for (hops = 0; hops < EBR_HOPS && at != 0; hops++) {
    if (at >= disk->sector_count ||
        blockdev_read(disk, at * FNU_BLOCKDEV_SECTOR_SIZE, sector,
                      sizeof(sector)) < 0 ||
        sector[510] != 0x55 || sector[511] != 0xAA)
      break;
    e = sector + 0x1BE;
    if (e[4] != 0 && !mbr_extended(e[4])) {
      claim(parent, index, at + read32(e + 8), read32(e + 12), 0);
      found++;
    }
    index++;
    e = sector + 0x1BE + 16;
    next = read32(e + 8);
    if (!mbr_extended(e[4]) || next == 0 || next >= ext_sectors)
      break;
    at = ext_lba + next;
  }
  return found;
}

static int scan_mbr(uint parent) {
  struct blockdev disk;
  uchar sector[FNU_BLOCKDEV_SECTOR_SIZE];
  const uchar *e;
  uint i, found = 0;

  disk.device = parent;
  disk.first_lba = 0;
  disk.sector_count = blockdev_device_size(parent);
  if (blockdev_read(&disk, 0, sector, sizeof(sector)) < 0)
    return -1;
  if (sector[510] != 0x55 || sector[511] != 0xAA)
    return -1;

  for (i = 0; i < 4; i++) {
    e = sector + 0x1BE + i * 16;
    if (e[4] == 0 || e[4] == MBR_PROTECTIVE)
      continue;
    if (mbr_extended(e[4])) {
      found += (uint)scan_ebr(parent, &disk, read32(e + 8), read32(e + 12));
      continue;
    }
    claim(parent, (int)i + 1, read32(e + 8), read32(e + 12), 0);
    found++;
  }
  return found > 0 ? 0 : -1;
}

void partition_init(void) {
  mtx_init(&partlock, "partition");
  parts = malloc(PART_FIRST * sizeof(*parts), M_DEVBUF, M_WAITOK | M_ZERO);
  nparts = PART_FIRST;
  part_major = blockdev_register(&part_blockdrv);
  devfs_add("part-by-uuid", FNU_TYPE_DIR, 0, 0, 0555);
  devfs_add("part-by-label", FNU_TYPE_DIR, 0, 0, 0555);
  devfs_add("fs-by-uuid", FNU_TYPE_DIR, 0, 0, 0555);
  devfs_add("fs-by-label", FNU_TYPE_DIR, 0, 0, 0555);
  devfs_add("serno", FNU_TYPE_DIR, 0, 0, 0555);
}

void partition_scan(uint parent) {
  if (part_major == 0 || major(parent) == (uint)part_major)
    return;
  if (scan_gpt(parent) == 0)
    return;
  scan_mbr(parent);
}

static int same_uuid(const char *a, const char *b) {
  int i;

  for (i = 0; a[i] != 0 && b[i] != 0; i++)
    if ((a[i] | 0x20) != (b[i] | 0x20))
      return 0;
  return a[i] == 0 && b[i] == 0;
}

uint partition_by_uuid(const char *uuid) {
  uint device = 0;
  int unit;

  mtx_enter(&partlock);
  for (unit = 0; unit < (int)nparts; unit++)
    if (parts[unit].used && parts[unit].uuid[0] != 0 &&
        same_uuid(parts[unit].uuid, uuid)) {
      device = makedev((uint)part_major, (uint)unit);
      break;
    }
  mtx_leave(&partlock);
  return device;
}

uint partition_labelled(const char *label, int *count) {
  uint device = 0;
  int unit;

  *count = 0;
  mtx_enter(&partlock);
  for (unit = 0; unit < (int)nparts; unit++)
    if (parts[unit].used &&
        strncmp(parts[unit].label, label, sizeof(parts[unit].label)) == 0 &&
        (*count)++ == 0)
      device = makedev((uint)part_major, (uint)unit);
  mtx_leave(&partlock);
  return device;
}

int partition_is(uint device) { return part_major != 0 && major(device) == (uint)part_major; }

uint partition_disk(uint device) {
  if (part_major == 0 || major(device) != (uint)part_major)
    return 0;
  return part_parent(minor(device));
}

#define UFS_TYPE "516e7cb6-6ecf-11d6-8ff8-00022d09712b"
#define SWAP_TYPE "516e7cb5-6ecf-11d6-8ff8-00022d09712b"

static uint by_type(uint disk, const char *type, const char *label) {
  int unit, best = -1, named = 0, is_named;

  mtx_enter(&partlock);
  for (unit = 0; unit < (int)nparts; unit++) {
    if (!parts[unit].used || parts[unit].parent != disk ||
        strncmp(parts[unit].type, type, sizeof(parts[unit].type)) != 0)
      continue;
    is_named = strncmp(parts[unit].label, label, sizeof(parts[unit].label)) == 0;
    if (best < 0 || (is_named && !named) ||
        (is_named == named && parts[unit].index < parts[best].index)) {
      best = unit;
      named = is_named;
    }
  }
  mtx_leave(&partlock);
  return best < 0 ? 0 : makedev((uint)part_major, (uint)best);
}

uint partition_root(uint disk) { return by_type(disk, UFS_TYPE, "fnu-root"); }

uint partition_swap(uint disk) { return by_type(disk, SWAP_TYPE, "fnu-swap"); }

static int on_disk(int unit, uint parent, int unseen);

static void unclaim(int unit) {
  uint device = makedev((uint)part_major, (uint)unit);
  char uuid[37], label[37];
  int busy = blockdev_busy(device);

  mtx_enter(&partlock);
  strlcpy(uuid, parts[unit].uuid, sizeof(uuid));
  strlcpy(label, parts[unit].label, sizeof(label));
  mtx_leave(&partlock);
  unalias(device, "part-by-uuid", uuid);
  unalias(device, "part-by-label", label);
  blockdev_withdraw(device);
  mtx_enter(&partlock);
  parts[unit].used = 0;
  parts[unit].held = busy;
  mtx_leave(&partlock);
}

// a partition something still names (mount, swap)
int partition_busy_disk(uint disk) {
  int unit, busy = 0;

  for (unit = 0; unit < (int)nparts && !busy; unit++)
    if (on_disk(unit, disk, 0))
      busy = blockdev_busy(makedev((uint)part_major, (uint)unit));
  return busy;
}

// gone partition released: its number may be reused
void partition_release(uint device) {
  uint unit = minor(device), parent = 0;
  int i, others = 0;

  if (part_major == 0 || major(device) != (uint)part_major || unit >= nparts)
    return;
  mtx_enter(&partlock);
  if (parts[unit].held && !parts[unit].used) {
    parts[unit].held = 0;
    parent = parts[unit].parent;
    for (i = 0; i < (int)nparts; i++)
      if (parts[i].held && parts[i].parent == parent)
        others = 1;
  }
  mtx_leave(&partlock);
  if (parent != 0 && !others)
    blockdev_release(parent);
}

static int on_disk(int unit, uint parent, int unseen) {
  int r;

  mtx_enter(&partlock);
  r = parts[unit].used && parts[unit].parent == parent && !(unseen && parts[unit].seen);
  mtx_leave(&partlock);
  return r;
}

void partition_reprobe(uint parent) {
  int unit;

  if (part_major == 0 || major(parent) == (uint)part_major)
    return;

  mtx_enter(&partlock);
  for (unit = 0; unit < (int)nparts; unit++)
    if (parts[unit].used && parts[unit].parent == parent)
      parts[unit].seen = 0;
  mtx_leave(&partlock);

  partition_scan(parent);

  for (unit = 0; unit < (int)nparts; unit++)
    if (on_disk(unit, parent, 1))
      unclaim(unit);
}

void partition_forget(uint parent) {
  int unit;

  for (unit = 0; unit < (int)nparts; unit++)
    if (on_disk(unit, parent, 0))
      unclaim(unit);
}
