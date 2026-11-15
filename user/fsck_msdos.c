#include "inc/dir.h"
#include "kern/fat32.h"
#include "kern/fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <math.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <fnu/sys.h>
#include <err.h>


static int dev;
static int fix;
static int preen;
static int force;
static int verbose;
static int problems;
static void complain(const char *what) {
  printf("fsck_msdos: %s\n", what);
  problems++;
}

static int readblock(uint b, void *buf) {
  int64_t at = lseek(dev, (int64_t)b * BSIZE, SEEK_SET);
  int n;

  if (at < 0) {
    warn("block %d", b);
    return -1;
  }
  n = (int)read(dev, buf, BSIZE);
  if (n != BSIZE) {
    dprintf(2, "fsck_msdos: block %d read %d bytes\n", b, n);
    return -1;
  }
  return 0;
}

static int writeblock(uint b, const void *buf) {
  if (lseek(dev, (int64_t)b * BSIZE, SEEK_SET) < 0)
    return -1;
  return write(dev, (void *)buf, BSIZE) == BSIZE ? 0 : -1;
}


#define FATMAX 0x100000
#define ROOT_FIXED 1

static struct fat32_bpb bpb;
static uint fat_type, fat_size, fat_start, data_start, spc, max_cluster;
static uint root_cluster, root_sector, root_sectors;
static uchar *reached;
static uchar reachbuf[FATMAX / 8];

static char fatbuf[BSIZE];
static uint fatsec = 0xFFFFFFFF;
static int fatdirty;

static int fat_flush(void) {
  uint i;

  if (!fatdirty || fatsec == 0xFFFFFFFF)
    return 0;
  fatdirty = 0;
  for (i = 0; i < bpb.fat_count; i++)
    if (writeblock(fatsec + i * fat_size, fatbuf) < 0)
      return -1;
  return 0;
}

static int fat_load(uint sector) {
  if (fatsec == sector)
    return 0;
  if (fat_flush() < 0)
    return -1;
  if (readblock(sector, fatbuf) < 0) {
    fatsec = 0xFFFFFFFF;
    return -1;
  }
  fatsec = sector;
  return 0;
}

static uint fat_byte(uint off) {
  if (fat_load(fat_start + off / BSIZE) < 0)
    return 0xFF;
  return (uchar)fatbuf[off % BSIZE];
}

static void fat_byte_set(uint off, uint v) {
  if (fat_load(fat_start + off / BSIZE) < 0)
    return;
  fatbuf[off % BSIZE] = (char)v;
  fatdirty = 1;
}

static uint fat_entry(uint c) {
  uint w;

  if (fat_type == 32) {
    if (fat_load(fat_start + (c * 4) / BSIZE) < 0)
      return 0x0FFFFFFF;
    return *(uint *)(fatbuf + (c * 4) % BSIZE) & 0x0FFFFFFF;
  }
  if (fat_type == 16) {
    w = fat_byte(c * 2) | (fat_byte(c * 2 + 1) << 8);
    return w >= 0xFFF7 ? 0x0FFFFFF0 | (w & 0xF) : w;
  }
  w = fat_byte(c + c / 2) | (fat_byte(c + c / 2 + 1) << 8);
  w = (c & 1) ? w >> 4 : w & 0xFFF;
  return w >= 0xFF7 ? 0x0FFFFFF0 | (w & 0xF) : w;
}

static int fat_clean(void) {
  if (fat_type == 32)
    return (fat_entry(1) & 0x08000000) != 0;
  if (fat_type == 16)
    return ((fat_byte(2) | (fat_byte(3) << 8)) & 0x8000) != 0;
  return 0;
}

static void fat_set(uint c, uint value) {
  uint *e, w, off;

  if (fat_type == 32) {
    if (fat_load(fat_start + (c * 4) / BSIZE) < 0)
      return;
    e = (uint *)(fatbuf + (c * 4) % BSIZE);
    *e = (*e & 0xF0000000) | (value & 0x0FFFFFFF);
    fatdirty = 1;
    return;
  }
  if (fat_type == 16) {
    w = value >= 0x0FFFFFF7 ? 0xFFF0 | (value & 0xF) : value & 0xFFFF;
    fat_byte_set(c * 2, w & 0xFF);
    fat_byte_set(c * 2 + 1, w >> 8);
    return;
  }
  w = value >= 0x0FFFFFF7 ? 0xFF0 | (value & 0xF) : value & 0xFFF;
  off = c + c / 2;
  if (c & 1) {
    fat_byte_set(off, (fat_byte(off) & 0x0F) | ((w << 4) & 0xFF));
    fat_byte_set(off + 1, w >> 4);
  } else {
    fat_byte_set(off, w & 0xFF);
    fat_byte_set(off + 1, (fat_byte(off + 1) & 0xF0) | (w >> 8));
  }
}

static uint cluster_sector(uint c) {
  return data_start + (c - 2) * spc;
}

static int dir_run(uint c, uint *sec, uint *count) {
  if (c == ROOT_FIXED) {
    *sec = root_sector;
    *count = root_sectors;
    return 1;
  }
  if (c < 2 || c > max_cluster)
    return 0;
  *sec = cluster_sector(c);
  *count = spc;
  return 1;
}

static uint dir_next(uint c) { return c == ROOT_FIXED ? 0x0FFFFFFF : fat_entry(c); }

static void fat_mark(uint c, const char *who) {
  if (reached[c / 8] & (1 << (c % 8))) {
    printf("fsck_msdos: cluster %d is in two chains, the second being %s\n", c, who);
    problems++;
    return;
  }
  reached[c / 8] |= (uchar)(1 << (c % 8));
}

static uint fat_chain(uint first, const char *who) {
  uint c = first, n = 0;

  while (c >= 2 && c <= max_cluster) {
    if (n++ > max_cluster) {
      printf("fsck_msdos: the chain of %s does not end\n", who);
      problems++;
      return n;
    }
    fat_mark(c, who);
    c = fat_entry(c);
  }
  if (c != 0 && c < 0x0FFFFFF8 && (c < 2 || c > max_cluster)) {
    printf("fsck_msdos: the chain of %s runs into cluster %d, which is not one\n",
           who, c);
    problems++;
  }
  return n;
}

static void fat_truncate(uint first, uint keep) {
  uint c = first, next, n = 0;

  while (c >= 2 && c <= max_cluster) {
    next = fat_entry(c);
    if (++n == keep) {
      fat_set(c, 0x0FFFFFFF);
      c = next;
      break;
    }
    c = next;
  }
  if (keep == 0)
    c = first;
  while (c >= 2 && c <= max_cluster) {
    next = fat_entry(c);
    fat_set(c, 0);
    c = next;
  }
}

static void fat_walk(uint dir_cluster, const char *what __unused) {
  char block[BSIZE];
  uint c = dir_cluster, steps = 0, sec, count;

  while (dir_run(c, &sec, &count) && steps++ <= max_cluster) {
    uint i;

    for (i = 0; i < count; i++) {
      uint j;

      if (readblock(sec + i, block) < 0)
        return;
      for (j = 0; j + sizeof(struct fat32_dirent) <= BSIZE;
           j += sizeof(struct fat32_dirent)) {
        struct fat32_dirent *de = (struct fat32_dirent *)(block + j);
        uint child;
        char name[16];
        uint k, n = 0;

        if (de->name[0] == 0)
          return;
        if (de->name[0] == 0xE5 || de->attr == FAT32_ATTR_LONG_NAME ||
            (de->attr & FAT32_ATTR_VOLUME_ID))
          continue;
        if (de->name[0] == '.')
          continue;
        for (k = 0; k < 11 && n + 1 < sizeof(name); k++)
          if (de->name[k] != ' ')
            name[n++] = (char)de->name[k];
        name[n] = 0;
        child = de->first_cluster_low;
        if (fat_type == 32)
          child |= (uint)de->first_cluster_high << 16;
        if (child == 0)
          continue;
        if (child < 2 || child > max_cluster) {
          printf("fsck_msdos: %s starts at cluster %d, which is not one\n", name,
                 child);
          problems++;
          continue;
        }
        if (de->attr & FAT32_ATTR_DIRECTORY) {
          fat_chain(child, name);
          fat_walk(child, name);
        } else {
          uint got = fat_chain(child, name);
          uint want = (de->file_size + spc * BSIZE - 1) / (spc * BSIZE);

          if (got < want) {
            printf("fsck_msdos: %s says %d bytes and has room for %d\n", name,
                   de->file_size, got * spc * BSIZE);
            problems++;
          } else if (got > want) {
            printf("fsck_msdos: %s says %d bytes and holds %d clusters, %d too "
                   "many\n",
                   name, de->file_size, got, got - want);
            problems++;
            if (fix)
              fat_truncate(child, want);
          }
        }
      }
    }
    c = dir_next(c);
  }
}

static uchar pointedbuf[FATMAX / 8];
static uint lost_dir;

static uint fat_alloc(void) {
  char zero[BSIZE];
  uint c, i;

  for (c = 2; c <= max_cluster; c++)
    if (fat_entry(c) == 0)
      break;
  if (c > max_cluster)
    return 0;
  fat_set(c, 0x0FFFFFFF);
  reached[c / 8] |= (uchar)(1 << (c % 8));
  memset(zero, 0, sizeof(zero));
  for (i = 0; i < spc; i++)
    if (writeblock(cluster_sector(c) + i, zero) < 0)
      return 0;
  return c;
}

static int fat_dir_add(uint dir_cluster, const struct fat32_dirent *entry) {
  char block[BSIZE];
  uint c = dir_cluster, last = 0, steps = 0, sec, count;

  while (dir_run(c, &sec, &count) && steps++ <= max_cluster) {
    uint i, j;

    for (i = 0; i < count; i++) {
      if (readblock(sec + i, block) < 0)
        return -1;
      for (j = 0; j + sizeof(*entry) <= BSIZE; j += sizeof(*entry)) {
        uchar first = (uchar)block[j];
        if (first != 0 && first != 0xE5)
          continue;
        memmove(block + j, entry, sizeof(*entry));
        return writeblock(sec + i, block);
      }
    }
    last = c;
    c = dir_next(c);
  }
  if (last == 0 || last == ROOT_FIXED || (c = fat_alloc()) == 0)
    return -1;
  fat_set(last, c);
  if (readblock(cluster_sector(c), block) < 0)
    return -1;
  memmove(block, entry, sizeof(*entry));
  return writeblock(cluster_sector(c), block);
}

static int fat_lost_dir(void) {
  struct fat32_dirent de;
  char block[BSIZE];
  uint c = root_cluster, steps = 0, sec, count;

  if (lost_dir != 0)
    return 0;
  while (dir_run(c, &sec, &count) && steps++ <= max_cluster) {
    uint i, j;

    for (i = 0; i < count; i++) {
      if (readblock(sec + i, block) < 0)
        return -1;
      for (j = 0; j + sizeof(de) <= BSIZE; j += sizeof(de)) {
        memmove(&de, block + j, sizeof(de));
        if (de.name[0] == 0)
          goto make;
        if ((de.attr & FAT32_ATTR_DIRECTORY) && de.attr != FAT32_ATTR_LONG_NAME &&
            memcmp(de.name, "LOST    DIR", 11) == 0) {
          lost_dir = de.first_cluster_low;
          if (fat_type == 32)
            lost_dir |= (uint)de.first_cluster_high << 16;
          return lost_dir >= 2 && lost_dir <= max_cluster ? 0 : -1;
        }
      }
    }
    c = dir_next(c);
  }
make:
  printf("fsck_msdos: no LOST.DIR directory%s\n", fix ? ", created" : "");
  if (!fix || (c = fat_alloc()) == 0)
    return -1;
  memset(&de, 0, sizeof(de));
  memset(de.name, ' ', 11);
  de.attr = FAT32_ATTR_DIRECTORY;
  de.name[0] = '.';
  de.first_cluster_high = (ushort)(c >> 16);
  de.first_cluster_low = (ushort)(c & 0xFFFF);
  if (fat_dir_add(c, &de) < 0)
    return -1;
  de.name[1] = '.';
  de.first_cluster_high = de.first_cluster_low = 0;
  if (fat_dir_add(c, &de) < 0)
    return -1;
  memmove(de.name, "LOST    DIR", 11);
  de.first_cluster_high = (ushort)(c >> 16);
  de.first_cluster_low = (ushort)(c & 0xFFFF);
  if (fat_dir_add(root_cluster, &de) < 0)
    return -1;
  lost_dir = c;
  return 0;
}

static uint fat_lost_length(uint head) {
  uint c = head, n = 0, last = 0;

  while (c >= 2 && c <= max_cluster && !(reached[c / 8] & (1 << (c % 8)))) {
    reached[c / 8] |= (uchar)(1 << (c % 8));
    n++;
    last = c;
    c = fat_entry(c);
  }
  if (c < 0x0FFFFFF8 && last != 0 && fix)
    fat_set(last, 0x0FFFFFFF);
  return n;
}

static void fat_reconnect(void) {
  uint c, head;

  memset(pointedbuf, 0, sizeof(pointedbuf));
  for (c = 2; c <= max_cluster; c++) {
    uint e = fat_entry(c);
    if (e >= 2 && e <= max_cluster)
      pointedbuf[e / 8] |= (uchar)(1 << (e % 8));
  }
  for (head = 2; head <= max_cluster; head++) {
    struct fat32_dirent de;
    uint length, n;
    char name[12];

    if (fat_entry(head) == 0 || (reached[head / 8] & (1 << (head % 8))) ||
        (pointedbuf[head / 8] & (1 << (head % 8))))
      continue;
    length = fat_lost_length(head);
    printf("fsck_msdos: lost cluster chain at cluster %d, %d cluster%s%s\n", head, length,
           length == 1 ? "" : "s", fix ? ", reconnected" : "");
    problems++;
    if (!fix)
      continue;
    if (fat_lost_dir() < 0) {
      printf("fsck_msdos: nowhere to put it\n");
      continue;
    }
    memset(&de, 0, sizeof(de));
    memset(de.name, ' ', 11);
    for (n = 0, c = head; c != 0 && n < 8; c /= 10, n++)
      name[n] = (char)('0' + c % 10);
    for (c = 0; c < n; c++)
      de.name[c] = (uchar)name[n - 1 - c];
    de.attr = FAT32_ATTR_ARCHIVE;
    de.first_cluster_high = (ushort)(head >> 16);
    de.first_cluster_low = (ushort)(head & 0xFFFF);
    de.file_size = length * spc * BSIZE;
    if (fat_dir_add(lost_dir, &de) < 0)
      printf("fsck_msdos: LOST.DIR could not take it\n");
  }
}

static void fsinfo_check(uint free_count, uint next_free) {
  char block[BSIZE];
  uint said, hint;
  int changed = 0;

  if (fat_type != 32 || bpb.fs_info == 0 || bpb.fs_info == 0xFFFF)
    return;
  if (readblock(bpb.fs_info, block) < 0) {
    complain("FSInfo cannot be read");
    return;
  }
  if (*(uint *)block != 0x41615252 || *(uint *)(block + 484) != 0x61417272 ||
      *(uint *)(block + 508) != 0xAA550000) {
    printf("fsck_msdos: FSInfo carries no valid signature\n");
    problems++;
    if (!fix)
      return;
    memset(block, 0, BSIZE);
    *(uint *)block = 0x41615252;
    *(uint *)(block + 484) = 0x61417272;
    *(uint *)(block + 508) = 0xAA550000;
    changed = 1;
  }
  said = *(uint *)(block + 488);
  hint = *(uint *)(block + 492);
  if (said != 0xFFFFFFFF && said != free_count) {
    printf("fsck_msdos: FSInfo says %d clusters are free, %d are\n", said,
           free_count);
    problems++;
    changed = 1;
  }
  if (hint != 0xFFFFFFFF && (hint < 2 || hint > max_cluster)) {
    printf("fsck_msdos: FSInfo points at cluster %d to allocate next, which is not "
           "one\n",
           hint);
    problems++;
    changed = 1;
  }
  if (fix && changed) {
    *(uint *)(block + 488) = free_count;
    *(uint *)(block + 492) = next_free != 0 ? next_free : 0xFFFFFFFF;
    if (writeblock(bpb.fs_info, block) < 0)
      complain("FSInfo could not be written back");
  }
}

static void fat_check(void) {
  uint c, lost = 0, free_count = 0, next_free = 0, mismatch = 0, total;

  spc = bpb.sectors_per_cluster;
  fat_start = bpb.reserved_sectors;
  fat_size = bpb.fat_size_16 != 0 ? bpb.fat_size_16 : bpb.fat_size_32;
  root_sectors = (bpb.root_entry_count * 32 + BSIZE - 1) / BSIZE;
  root_sector = fat_start + bpb.fat_count * fat_size;
  data_start = root_sector + root_sectors;
  total = bpb.total_sectors_16 != 0 ? bpb.total_sectors_16 : bpb.total_sectors_32;
  if (bpb.bytes_per_sector != BSIZE || spc == 0 || bpb.fat_count == 0 ||
      fat_size == 0 || total <= data_start) {
    complain("the boot sector does not describe a FAT volume");
    return;
  }
  max_cluster = (total - data_start) / spc + 1;
  fat_type = max_cluster - 1 < 4085 ? 12 : max_cluster - 1 < 65525 ? 16 : 32;
  if (fat_type == 32 && (bpb.fat_size_32 == 0 || bpb.root_cluster < 2)) {
    complain("the boot sector does not describe a FAT32 volume");
    return;
  }
  root_cluster = fat_type == 32 ? bpb.root_cluster : ROOT_FIXED;
  if (max_cluster >= FATMAX) {
    dprintf(2, "fsck_msdos: the volume has more clusters than this can check\n");
    exit(1);
  }
  printf("FAT%d: %d clusters of %d bytes, %d tables at sector %d\n", fat_type,
         max_cluster - 1, spc * BSIZE, bpb.fat_count, fat_start);
  if (preen && !force && fat_clean()) {
    printf("FILESYSTEM CLEAN; SKIPPING CHECKS\n");
    return;
  }

  reached = reachbuf;
  memset(reached, 0, sizeof(reachbuf));
  if (fat_type == 32)
    fat_chain(root_cluster, "the root");
  fat_walk(root_cluster, "the root");

  fat_reconnect();
  for (c = 2; c <= max_cluster; c++) {
    if (fat_entry(c) == 0)
      continue;
    if (reached[c / 8] & (1 << (c % 8)))
      continue;
    lost++;
    if (fix)
      fat_set(c, 0);
  }
  if (lost != 0) {
    printf("fsck_msdos: %d clusters are allocated and nothing uses them%s\n", lost,
           fix ? ", freed" : "");
    problems++;
  }
  if (fat_flush() < 0)
    complain("the table could not be written back");

  for (c = 2; c <= max_cluster; c++) {
    if (fat_entry(c) == 0) {
      if (next_free == 0)
        next_free = c;
      free_count++;
    }
  }

  if (bpb.fat_count > 1) {
    uint s, i;

    for (s = 0; s < fat_size; s++) {
      char a[BSIZE], b[BSIZE];

      if (readblock(fat_start + s, a) < 0)
        break;
      for (i = 1; i < bpb.fat_count; i++) {
        if (readblock(fat_start + i * fat_size + s, b) < 0)
          break;
        if (memcmp(a, b, BSIZE) == 0)
          continue;
        mismatch++;
        if (fix)
          writeblock(fat_start + i * fat_size + s, a);
      }
    }
    if (mismatch != 0) {
      printf("fsck_msdos: the copies of the table disagree over %d sectors%s\n",
             mismatch, fix ? ", corrected from the first" : "");
      problems++;
    }
  }

  fsinfo_check(free_count, next_free);
}

static int mounted(const char *path) {
  struct fnu_fsstat *table;
  struct stat st;
  int n, i, busy = 0;

  if (stat(path, &st) < 0 || !S_ISBLK(st.st_mode) ||
      (n = fnu_volumes(&table)) < 0)
    return 0;
  for (i = 0; i < n; i++)
    if (table[i].dev == st.st_rdev && !(table[i].flags & FNU_MNT_RDONLY))
      busy = 1;
  fnu_table_free(table);
  return busy;
}

static void usage(void) {
  dprintf(2, "usage: fsck_msdos [-fnpvy] device\n");
  exit(1);
}

int main(int argc, char **argv) {
  char block[BSIZE];
  const char *path;
  int i;

  for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
    const char *o;

    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (o = argv[i] + 1; *o != 0; o++) {
      if (*o == 'y' || *o == 'Y')
        fix = 1;
      else if (*o == 'n' || *o == 'N')
        fix = 0;
      else if (*o == 'p')
        fix = preen = 1;
      else if (*o == 'f')
        force = 1;
      else if (*o == 'v')
        verbose = 1;
      else
        usage();
    }
  }
  if (i != argc - 1)
    usage();
  path = argv[i];
  if (fix && mounted(path)) {
    dprintf(2, "fsck_msdos: %s is mounted; unmount it before repairing\n", path);
    exit(1);
  }
  if ((dev = open(path, fix ? O_RDWR : O_RDONLY)) < 0) {
    warn("%s", path);
    exit(1);
  }

  if (readblock(0, block) == 0) {
    memmove(&bpb, block, sizeof(bpb));
    if ((uchar)block[510] == 0x55 && (uchar)block[511] == 0xAA &&
        (bpb.fat_size_16 != 0 || bpb.fat_size_32 != 0) &&
        bpb.bytes_per_sector == BSIZE && bpb.sectors_per_cluster != 0 &&
        ((bpb.fat_size_16 == 0 && bpb.boot_signature == 0x29) ||
         (bpb.fat_size_16 == 0 && bpb.boot_signature == 0x28) ||
         (bpb.fat_size_16 != 0 && ((uchar)block[38] == 0x29 || (uchar)block[38] == 0x28)))) {
      fat_check();
      goto done;
    }
  }

  dprintf(2, "fsck_msdos: %s carries no FAT\n", path);
  exit(1);

done:
  // preen asks for a person
  if (problems == 0) {
    if (!preen)
      printf("%s: clean\n", path);
  } else if (preen && !fix) {
    printf("%s: UNEXPECTED INCONSISTENCY; RUN fsck MANUALLY.\n", path);
    close(dev);
    exit(8);
  } else
    printf("%s: %d problem%s%s\n", path, problems, problems == 1 ? "" : "s",
           fix ? " put right" : "; run with -y to correct them");
  if (fix)
    fsync(dev);
  close(dev);
  exit(problems != 0 && !fix ? 1 : 0);
}
