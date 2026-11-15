// UFS2
#include "boot/ufs2read.h"

#define MAGIC 0x19540119u
#define ROOTINO 2
#define DINODE 256
#define NDADDR 12
#define NIADDR 3
#define IFMT 0170000
#define IFDIR 0040000
#define IFREG 0100000

#define SB_IBLKNO 16
#define SB_BSIZE 48
#define SB_FSIZE 52
#define SB_FRAG 56
#define SB_BSHIFT 80
#define SB_FSHIFT 84
#define SB_NINDIR 116
#define SB_INOPB 120
#define SB_IPG 184
#define SB_FPG 188
#define SB_MAGIC 1372
#define SB_SBLOCKLOC 1000
#define DI_MODE 0
#define DI_SIZE 16
#define DI_DB 112
#define DI_IB 208

static uint32 rd32(const uint8 *p) {
  return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
         ((uint32)p[3] << 24);
}
static uint64 rd64(const uint8 *p) {
  return (uint64)rd32(p) | ((uint64)rd32(p + 4) << 32);
}
static uint16 rd16(const uint8 *p) { return (uint16)(p[0] | (p[1] << 8)); }

static void copy(uint8 *d, const uint8 *s, uint32 n) {
  while (n-- > 0)
    *d++ = *s++;
}

static const uint32 sblock_try[] = {65536, 262144};

static uint32 log2of(uint32 v) {
  uint32 s = 0;

  while ((1u << s) < v)
    s++;
  return (1u << s) == v ? s : 0;
}

static int partread(struct ufs2rd *u, uint64 off, uint8 *dst, uint32 n) {
  const struct bootdisk *d = u->disk;
  uint32 bmask = (1u << d->shift) - 1;

  while (n > 0) {
    uint64 at = (u->start << d->shift) + off;
    uint32 in = (uint32)(at & bmask), take = UFS2RD_VBLK - in, count;

    if (take > n)
      take = n;
    count = (in + take + bmask) >> d->shift;
    if (d->read(d->ctx, at >> d->shift, count, u->buf) < 0)
      return -1;
    copy(dst, u->buf + in, take);
    off += take;
    dst += take;
    n -= take;
  }
  return 0;
}

int ufs2rd_mount(struct ufs2rd *u) {
  uint8 sb[SB_MAGIC + 4];
  uint32 nindir, i;

  if (u->disk->shift < 9 || (1u << u->disk->shift) > UFS2RD_VBLK)
    return -1;
  for (i = 0; i < sizeof(sblock_try) / sizeof(sblock_try[0]); i++)
    if (partread(u, sblock_try[i], sb, sizeof(sb)) == 0 &&
        rd32(sb + SB_MAGIC) == MAGIC && rd64(sb + SB_SBLOCKLOC) == sblock_try[i])
      break;
  if (i == sizeof(sblock_try) / sizeof(sblock_try[0]))
    return -1;
  u->bsize = rd32(sb + SB_BSIZE);
  u->fsize = rd32(sb + SB_FSIZE);
  u->frag = rd32(sb + SB_FRAG);
  u->bshift = rd32(sb + SB_BSHIFT);
  u->fshift = rd32(sb + SB_FSHIFT);
  u->iblkno = rd32(sb + SB_IBLKNO);
  u->ipg = rd32(sb + SB_IPG);
  u->fpg = rd32(sb + SB_FPG);
  u->inopb = rd32(sb + SB_INOPB);
  nindir = rd32(sb + SB_NINDIR);
  u->nshift = log2of(nindir);
  if (u->bsize < 4096 || u->bsize > 65536 || (1u << u->bshift) != u->bsize ||
      u->fsize < UFS2RD_DIRBLK || (1u << u->fshift) != u->fsize ||
      u->frag != u->bsize / u->fsize || u->ipg == 0 || u->fpg == 0 ||
      u->inopb != u->bsize / DINODE || nindir != u->bsize / 8 || u->nshift == 0)
    return -1;
  return 0;
}

static int dinode(struct ufs2rd *u, uint32 ino, uint8 *dp) {
  uint32 cg = ino / u->ipg, idx = ino % u->ipg;
  uint64 frag = (uint64)u->fpg * cg + u->iblkno + (uint64)(idx / u->inopb) * u->frag;

  return partread(u, (frag << u->fshift) + (uint64)(idx % u->inopb) * DINODE, dp, DINODE);
}

static int bmap(struct ufs2rd *u, const uint8 *dp, uint64 lbn, uint64 *out) {
  uint64 span, blk;
  uint8 raw[8];
  int level, l;

  if (lbn < NDADDR) {
    *out = rd64(dp + DI_DB + lbn * 8);
    return 0;
  }
  lbn -= NDADDR;
  for (level = 1; level <= NIADDR; level++) {
    span = (uint64)1 << (u->nshift * level);
    if (lbn < span)
      break;
    lbn -= span;
  }
  if (level > NIADDR)
    return -1;
  blk = rd64(dp + DI_IB + (level - 1) * 8);
  for (l = level - 1; l >= 0 && blk != 0; l--) {
    uint64 idx = (lbn >> (u->nshift * l)) & ((1u << u->nshift) - 1);

    if (partread(u, (blk << u->fshift) + idx * 8, raw, 8) < 0)
      return -1;
    blk = rd64(raw);
  }
  *out = blk;
  return 0;
}

static int fileread(struct ufs2rd *u, const uint8 *dp, uint64 off, uint8 *dst,
                    uint32 n) {
  while (n > 0) {
    uint64 lbn = off >> u->bshift, frag;
    uint32 in = (uint32)(off & (u->bsize - 1)), take = u->bsize - in;

    if (take > n)
      take = n;
    if (take > UFS2RD_VBLK)
      take = UFS2RD_VBLK;
    if (bmap(u, dp, lbn, &frag) < 0)
      return -1;
    if (frag == 0) {
      uint32 i;

      for (i = 0; i < take; i++)
        dst[i] = 0;
    } else if (partread(u, (frag << u->fshift) + in, dst, take) < 0) {
      return -1;
    }
    off += take;
    dst += take;
    n -= take;
  }
  return 0;
}

int ufs2rd_read(struct ufs2rd *u, uint32 ino, uint64 off, void *dst, uint32 n) {
  uint8 dp[DINODE];

  if (dinode(u, ino, dp) < 0 || off > rd64(dp + DI_SIZE) ||
      n > rd64(dp + DI_SIZE) - off)
    return -1;
  return fileread(u, dp, off, dst, n);
}

// a name in a directory
static int dirfind(struct ufs2rd *u, uint32 dir, const char *name, uint32 len,
                   uint32 *ino) {
  uint8 dp[DINODE], *chunk = u->dir;
  uint64 off, size;
  uint32 o, reclen, i;

  if (dinode(u, dir, dp) < 0 || (rd16(dp + DI_MODE) & IFMT) != IFDIR)
    return -1;
  size = rd64(dp + DI_SIZE);
  for (off = 0; off + UFS2RD_DIRBLK <= size; off += UFS2RD_DIRBLK) {
    if (fileread(u, dp, off, chunk, UFS2RD_DIRBLK) < 0)
      return -1;
    for (o = 0; o + 8 <= UFS2RD_DIRBLK; o += reclen) {
      reclen = rd16(chunk + o + 4);
      if (reclen < 8 || o + reclen > UFS2RD_DIRBLK)
        break;
      if (rd32(chunk + o) == 0 || chunk[o + 7] != len || 8 + len > reclen)
        continue;
      for (i = 0; i < len && chunk[o + 8 + i] == (uint8)name[i]; i++)
        ;
      if (i == len) {
        *ino = rd32(chunk + o);
        return 0;
      }
    }
  }
  return -1;
}

int ufs2rd_lookup(struct ufs2rd *u, const char *path, uint32 *ino, uint64 *size) {
  uint8 dp[DINODE];
  uint32 at = ROOTINO, len;

  while (*path != 0) {
    while (*path == '/')
      path++;
    for (len = 0; path[len] != 0 && path[len] != '/'; len++)
      ;
    if (len == 0)
      break;
    if (len > 255 || dirfind(u, at, path, len, &at) < 0)
      return -1;
    path += len;
  }
  if (dinode(u, at, dp) < 0 || (rd16(dp + DI_MODE) & IFMT) != IFREG)
    return -1;
  *ino = at;
  *size = rd64(dp + DI_SIZE);
  return 0;
}
