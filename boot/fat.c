#include "boot.h"

#define ATTR_LFN 0x0f
#define ATTR_DIR 0x10
#define EOC 0x0ffffff8u

static uint64 part_start;
static uint32 fat_start;
static uint32 data_start;
static uint32 sec_per_clu;
static uint32 root_clu;

static uint8 secbuf[SECTOR];
static uint32 fat_cached = 0xffffffffu;
static uint8 fatbuf[SECTOR];

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
  data_start = reserved + nfat * fatsz;
  fat_cached = 0xffffffffu;
  return 0;
}

static uint64 clu_lba(uint32 clu) {
  return part_start + data_start + (uint64)(clu - 2) * sec_per_clu;
}

static uint32 fat_next(uint32 clu) {
  uint32 sec = fat_start + clu / (SECTOR / 4);

  if (sec != fat_cached) {
    if (disk_read(part_start + sec, 1, fatbuf) < 0)
      return EOC;
    fat_cached = sec;
  }
  return rd32(fatbuf + (clu % (SECTOR / 4)) * 4) & 0x0fffffffu;
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

// finds name in the directory chain, returns its cluster
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

int fat_load(const char *path, uint32 linear, uint32 limit, uint32 *size) {
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
  // whole clusters are copied, so the last one must fit too
  if (limit != 0 && fsize + sec_per_clu * SECTOR > limit)
    return -1;

  {
    uint32 left = fsize;
    while (left > 0 && clu < EOC && clu >= 2) {
      uint32 n = sec_per_clu;
      if (disk_read_far(clu_lba(clu), n, linear) < 0)
        return -1;
      linear += n * SECTOR;
      left = left > n * SECTOR ? left - n * SECTOR : 0;
      clu = fat_next(clu);
    }
    if (left > 0)
      return -1;
  }

  if (size)
    *size = fsize;
  return 0;
}
