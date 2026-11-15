#include "boot.h"

#define ATTR_LFN 0x0f
#define ATTR_DIR 0x10
#define EOC 0x0ffffff8u
#define FATWIN 64 // fat sectors read at once

static uint64 part_start;
static uint32 fat_start;
static uint32 fat_size;
static uint32 data_start;
static uint32 sec_per_clu;
static uint32 root_clu;

static uint8 secbuf[SECTOR];
static uint32 fat_first, fat_count;
static uint8 fatbuf[FATWIN * SECTOR];

static uint16 rd16(const uint8 *p) { return (uint16)(p[0] | (p[1] << 8)); }

static uint32 rd32(const uint8 *p) {
  return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
         ((uint32)p[3] << 24);
}

int fat_mount(uint64 lba) {
  uint32 nfat, fatsz, reserved;

  if (disk_read(lba, 1, secbuf) < 0)
    return -1;
  if (rd16(secbuf + 11) != SECTOR)
    return -1;

  sec_per_clu = secbuf[13];
  reserved = rd16(secbuf + 14);
  nfat = secbuf[16];
  fatsz = rd32(secbuf + 36);
  root_clu = rd32(secbuf + 44);
  if (sec_per_clu == 0 || fatsz == 0 || root_clu < 2)
    return -1;

  part_start = lba;
  fat_start = reserved;
  fat_size = fatsz;
  data_start = reserved + nfat * fatsz;
  fat_count = 0;
  return 0;
}

static uint64 clu_lba(uint32 clu) {
  return part_start + data_start + (uint64)(clu - 2) * sec_per_clu;
}

static uint32 fat_next(uint32 clu) {
  uint32 sec = clu / (SECTOR / 4), n;

  if (sec >= fat_size)
    return EOC;
  if (fat_count == 0 || sec < fat_first || sec >= fat_first + fat_count) {
    n = fat_size - sec < FATWIN ? fat_size - sec : FATWIN;
    if (disk_read(part_start + fat_start + sec, n, fatbuf) < 0) {
      fat_count = 0;
      return EOC;
    }
    fat_first = sec;
    fat_count = n;
  }
  return rd32(fatbuf + (clu - fat_first * (SECTOR / 4)) * 4) & 0x0fffffffu;
}

// "kernel" -> "KERNEL     "
static void short_name(const char *s, uint32 n, char out[11]) {
  uint32 i, j;

  for (i = 0; i < 11; i++)
    out[i] = ' ';
  for (i = 0, j = 0; i < n && j < 11; i++) {
    char c = s[i];
    if (c == '.') {
      j = 8;
      continue;
    }
    if (c >= 'a' && c <= 'z')
      c = (char)(c - 'a' + 'A');
    out[j++] = c;
  }
}

static int dir_find(uint32 dir, const char *name, uint32 len, uint32 *clu,
                    uint32 *size, int *isdir) {
  char want[11];
  uint32 s, i;

  short_name(name, len, want);

  while (dir < EOC && dir >= 2) {
    for (s = 0; s < sec_per_clu; s++) {
      if (disk_read(clu_lba(dir) + s, 1, secbuf) < 0)
        return -1;
      for (i = 0; i < SECTOR; i += 32) {
        uint8 *e = secbuf + i;
        if (e[0] == 0x00)
          return -1;
        if (e[0] == 0xe5 || (e[11] & ATTR_LFN) == ATTR_LFN)
          continue;
        if (memcmp(e, want, 11) != 0)
          continue;
        *clu = ((uint32)rd16(e + 20) << 16) | rd16(e + 26);
        *size = rd32(e + 28);
        *isdir = (e[11] & ATTR_DIR) != 0;
        return 0;
      }
    }
    dir = fat_next(dir);
  }
  return -1;
}

static int lookup(const char *path, uint32 *cluster, uint32 *size) {
  uint32 clu = root_clu, fsize = 0;
  int isdir = 1;

  while (*path) {
    const char *b;
    uint32 len;

    while (*path == '/')
      path++;
    if (*path == 0)
      break;
    b = path;
    while (*path && *path != '/')
      path++;
    len = (uint32)(path - b);

    if (!isdir)
      return -1;
    if (dir_find(clu, b, len, &clu, &fsize, &isdir) < 0)
      return -1;
  }
  if (isdir)
    return -1;
  *cluster = clu;
  *size = fsize;
  return 0;
}

// what a load of the file takes: whole clusters
int fat_file_size(const char *path, uint32 *size) {
  uint32 clu, fsize, clusz = sec_per_clu * SECTOR;

  if (lookup(path, &clu, &fsize) < 0)
    return -1;
  *size = (fsize + clusz - 1) / clusz * clusz;
  return 0;
}

int fat_load(const char *path, uint32 linear, uint32 limit, uint32 *size) {
  uint32 clu, fsize;

  if (lookup(path, &clu, &fsize) < 0)
    return -1;
  if (limit != 0 && (fsize + sec_per_clu * SECTOR - 1) / (sec_per_clu * SECTOR) *
                            (sec_per_clu * SECTOR) > limit)
    return -1;

  {
    uint32 left = fsize, clusz = sec_per_clu * SECTOR;

    // a run of adjacent clusters is one read
    while (left > 0 && clu < EOC && clu >= 2) {
      uint32 first = clu, run = 0, bytes;

      do {
        run++;
        clu = fat_next(clu);
      } while (clu == first + run && run * clusz < left);
      if (disk_read_far(clu_lba(first), run * sec_per_clu, linear) < 0)
        return -1;
      bytes = run * clusz;
      linear += bytes;
      left = left > bytes ? left - bytes : 0;
    }
    if (left > 0)
      return -1;
  }

  if (size)
    *size = fsize;
  return 0;
}
