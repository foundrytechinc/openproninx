#include "inc/abi.h"
#include "kern/ufs2.h"
#include "kern/ulogfmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <fnu/sys.h>

#define SBLOCK_UFS2 65536
#define DEV_BSIZE 512
#define NBBY 8
#define DIRBLKSIZ 512
#define NDADDR UFS2_DIRECT_BLOCKS
#define NIADDR UFS2_INDIRECT_BLOCKS
#define MAXDIRSIZE 0x7fffffff
#define LFNAME "lost+found"
#define LFMODE 0700

#define IFMT 0170000
#define IFIFO 0010000
#define IFCHR 0020000
#define IFDIR 0040000
#define IFBLK 0060000
#define IFREG 0100000
#define IFLNK 0120000
#define IFSOCK 0140000
#define IFWHT 0160000

// inode states
#define USTATE 0
#define FSTATE 1
#define DSTATE 2
#define DFOUND 3
#define DCLEAR 4
#define FCLEAR 5

static int dev;
static int fix;
static int preen;
static int force;
static int debug;
static int problems;
static int unfixed;
static int modified;

static uchar sb[UFS2_SUPERBLOCK_SIZE];
static uchar *cgbuf;
static int cgx = -1;
static int cgdirty;
static uchar *blk;

static uint bsize, fsize, frag, fragshift, bshift, fshift;
static uint ncg, ipg, fpg, inopb, nindir, cgsize, sbsize, contigsumsize;
static uint sblkno, cblkno, iblkno, dblkno, maxsymlinklen;
static uint64_t fssize, dsize, csaddr, cssize, maxfilesize, maxino;

static uchar *bmap;
static uchar *dupmap;
static uchar *state;
static uchar *itype;
static int32_t *lncnt;
static uint32_t *parent;
static uint32_t *dotdot;
static uint32_t *initedmax;
static uint64_t n_files, n_blks;
static uint32_t lfdir;

// duplicated fragments and their claimants
#define MAXDUPS 4096
static struct { uint64_t blk; int claims; } dups[MAXDUPS];
static int ndups;

static int dup_slot(uint64_t blk) {
  int i;

  for (i = 0; i < ndups; i++)
    if (dups[i].blk == blk)
      return i;
  return -1;
}

static uint32_t rd32(const uchar *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uchar *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint64_t rd64(const uchar *p) {
  return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static void wr16(uchar *p, uint16_t v) {
  p[0] = (uchar)v;
  p[1] = (uchar)(v >> 8);
}
static void wr32(uchar *p, uint32_t v) {
  p[0] = (uchar)v;
  p[1] = (uchar)(v >> 8);
  p[2] = (uchar)(v >> 16);
  p[3] = (uchar)(v >> 24);
}
static void wr64(uchar *p, uint64_t v) {
  wr32(p, (uint32_t)v);
  wr32(p + 4, (uint32_t)(v >> 32));
}
static void add32(uchar *p, int d) { wr32(p, rd32(p) + (uint32_t)d); }

#define SB(f) (sb + UFS2_SB_##f)
#define CG(f) (cgbuf + UFS2_CG_##f)
#define roundup(a, b) (howmany(a, b) * (b))
#define cgbase(c) ((uint64_t)fpg * (c))
#define cgsblock(c) (cgbase(c) + sblkno)
#define cgtod(c) (cgbase(c) + cblkno)
#define cgimin(c) (cgbase(c) + iblkno)
#define cgdmin(c) (cgbase(c) + dblkno)
#define dtog(d) ((uint)((d) / fpg))
#define dtogd(d) ((uint)((d) % fpg))
#define fragroundup(n) (roundup((uint64_t)(n), fsize))
#define numfrags(n) ((uint)((n) / fsize))
#define btodb(n) ((n) / DEV_BSIZE)
#define DIRECTSIZ(namlen) ((UFS2_DIR_HEADER_SIZE + (namlen) + 1 + 3) & ~3U)

static int testbit(const uchar *m, uint64_t b) { return (m[b / NBBY] >> (b % NBBY)) & 1; }
static void setbit_(uchar *m, uint64_t b) { m[b / NBBY] |= (uchar)(1 << (b % NBBY)); }
static void clrbit_(uchar *m, uint64_t b) { m[b / NBBY] &= (uchar)~(1 << (b % NBBY)); }

static int reply(const char *question) {
  if (fix) {
    printf(" (%s)\n", question);
    return 1;
  }
  printf("\n");
  unfixed++;
  return 0;
}

// I/O BY BYTE

static int rdbytes(int64_t off, void *buf, uint n) {
  if (lseek(dev, off, SEEK_SET) < 0 || read(dev, buf, (int)n) != (int)n) {
    dprintf(2, "fsck_ffs: cannot read %u bytes at %lld\n", n, off);
    return -1;
  }
  return 0;
}

static int wrbytes(int64_t off, const void *buf, uint n) {
  if (lseek(dev, off, SEEK_SET) < 0 || write(dev, (void *)buf, (int)n) != (int)n) {
    dprintf(2, "fsck_ffs: cannot write %u bytes at %lld\n", n, off);
    return -1;
  }
  modified = 1;
  return 0;
}

static int rdfrag(uint64_t fragno, uint off, void *buf, uint n) {
  return rdbytes((int64_t)fragno * fsize + off, buf, n);
}
static int wrfrag(uint64_t fragno, uint off, const void *buf, uint n) {
  return wrbytes((int64_t)fragno * fsize + off, buf, n);
}

static void cgflush(void) {
  if (cgx >= 0 && cgdirty) {
    wrfrag(cgtod((uint)cgx), 0, cgbuf, cgsize);
    cgdirty = 0;
  }
}

static int cgload(uint c) {
  if (cgx == (int)c)
    return 0;
  cgflush();
  cgx = -1;
  if (rdfrag(cgtod(c), 0, cgbuf, cgsize) < 0)
    return -1;
  cgx = (int)c;
  cgdirty = 0;
  return 0;
}

#define cg_inosused() (cgbuf + rd32(CG(iusedoff)))
#define cg_blksfree() (cgbuf + rd32(CG(freeoff)))
#define cg_clustersfree() (cgbuf + rd32(CG(clusteroff)))
#define cg_clustersum() (cgbuf + rd32(CG(clustersumoff)))

static uchar *iblk;
static uint64_t iblkat = ~(uint64_t)0;
static int iblkdirty;

// INODES

static void iflush(void) {
  if (iblkdirty && iblkat != ~(uint64_t)0)
    wrfrag(iblkat, 0, iblk, bsize);
  iblkdirty = 0;
}

static uchar *ginode(uint64_t ino) {
  uint64_t fragno;

  if (ino >= maxino)
    return 0;
  fragno = cgimin(ino / ipg) + (uint64_t)((ino % ipg) / inopb) * frag;
  if (fragno != iblkat) {
    iflush();
    iblkat = ~(uint64_t)0;
    if (rdfrag(fragno, 0, iblk, bsize) < 0) {
      dprintf(2, "fsck_ffs: cannot read the inodes at fragment %llu\n",
              (unsigned long long)fragno);
      exit(8);
    }
    iblkat = fragno;
  }
  return iblk + (ino % inopb) * UFS2_DINODE_SIZE;
}

static void inodirty(void) { iblkdirty = 1; }

#define DI_MODE(dp) rd16((dp) + UFS2_DINODE_MODE)
#define DI_NLINK(dp) ((int16_t)rd16((dp) + UFS2_DINODE_NLINK))
#define DI_SIZE(dp) rd64((dp) + UFS2_DINODE_SIZE_OFFSET)
#define DI_BLOCKS(dp) rd64((dp) + UFS2_DINODE_BLOCKS)
#define DI_DB(dp, i) rd64((dp) + UFS2_DINODE_DIRECT_OFFSET + (i) * 8)
#define DI_IB(dp, i) rd64((dp) + UFS2_DINODE_INDIRECT_OFFSET + (i) * 8)

static void clearinode(uchar *dp) {
  uint32_t gen = rd32(dp + UFS2_DINODE_GEN);

  memset(dp, 0, UFS2_DINODE_SIZE);
  wr32(dp + UFS2_DINODE_GEN, gen);
}

static int inline_link(const uchar *dp) {
  return (DI_MODE(dp) & IFMT) == IFLNK && DI_SIZE(dp) < maxsymlinklen;
}

static uchar dt_of(uint mode) {
  switch (mode & IFMT) {
  case IFIFO: return 1;
  case IFCHR: return 2;
  case IFDIR: return 4;
  case IFBLK: return 6;
  case IFREG: return 8;
  case IFLNK: return 10;
  case IFSOCK: return 12;
  case IFWHT: return 14;
  }
  return 0;
}

typedef int (*blkfn)(uint64_t ino, uint64_t fragno, uint nfrags, int level);

static int walk_indir(uint64_t ino, uint64_t blkno, uint level,
                      uint64_t *remain, blkfn fn, int *truncated);

static int walk_inode(uint64_t ino, const uchar *dp, blkfn fn) {
  uint64_t size = DI_SIZE(dp), remain, b;
  uint i, nfrags;
  int truncated = 0, r;

  if (inline_link(dp))
    return 0;
  remain = size;
  for (i = 0; i < NDADDR; i++) {
    b = DI_DB(dp, i);
    if (b != 0) {
      nfrags = remain >= bsize ? frag : numfrags(fragroundup(remain));
      if (nfrags == 0)
        nfrags = frag;
      if ((r = fn(ino, b, nfrags, 0)) < 0)
        return r;
    }
    remain = remain > bsize ? remain - bsize : 0;
  }
  for (i = 0; i < NIADDR; i++) {
    b = DI_IB(dp, i);
    if (b == 0)
      continue;
    if ((r = fn(ino, b, frag, (int)i + 1)) < 0)
      return r;
    if ((r = walk_indir(ino, b, i + 1, &remain, fn, &truncated)) < 0)
      return r;
  }
  return truncated;
}

static int walk_indir(uint64_t ino, uint64_t blkno, uint level,
                      uint64_t *remain, blkfn fn, int *truncated) {
  uchar *raw;
  uint i;
  uint64_t span = bsize, child;
  int r = 0, dirty = 0;

  for (i = 1; i < level; i++)
    span *= nindir;
  if (blkno >= fssize || (raw = malloc(bsize)) == 0)
    return -1;
  if (rdfrag(blkno, 0, raw, bsize) < 0) {
    free(raw);
    return -1;
  }
  for (i = 0; i < nindir; i++) {
    child = rd64(raw + i * 8);
    if (*remain == 0 && child != 0) {
      if (!*truncated) {
        printf("PARTIALLY TRUNCATED INODE I=%llu", ino);
        problems++;
        *truncated = 1;
        if (reply("SALVAGE"))
          *truncated = 2;
      }
      if (*truncated == 2) {
        wr64(raw + i * 8, 0);
        dirty = 1;
      }
      continue;
    }
    if (child != 0) {
      if ((r = fn(ino, child, frag, level > 1 ? (int)level - 1 : 0)) < 0)
        break;
      if (level > 1) {
        if ((r = walk_indir(ino, child, level - 1, remain, fn, truncated)) < 0)
          break;
        continue;
      }
    } else if (level > 1) {
      *remain = *remain > span ? *remain - span : 0;
      continue;
    }
    *remain = *remain > bsize ? *remain - bsize : 0;
  }
  if (dirty)
    wrfrag(blkno, 0, raw, bsize);
  free(raw);
  return r;
}

static int chkrange(uint64_t b, uint cnt) {
  uint c;

  if (b >= fssize || b + cnt > fssize || cnt == 0)
    return 1;
  c = dtog(b);
  if (b < cgdmin(c)) {
    if (b + cnt > cgsblock(c))
      return 1;
  } else if (b + cnt > cgbase(c + 1)) {
    return 1;
  }
  return 0;
}

static uint64_t counted;
static int badblk, dupblk;

// PASS 1

static int pass1check(uint64_t ino, uint64_t b, uint nfrags, int level __unused) {
  uint i;

  if (chkrange(b, nfrags)) {
    printf("BAD BLOCK %llu I=%llu\n", b, ino);
    problems++;
    badblk++;
    return badblk > 10 ? -1 : 0;
  }
  for (i = 0; i < nfrags; i++) {
    if (!testbit(bmap, b + i)) {
      n_blks++;
      setbit_(bmap, b + i);
    } else {
      int d = dup_slot(b + i);
      printf("DUP BLOCK %llu I=%llu\n", b + i, ino);
      problems++;
      setbit_(dupmap, b + i);
      if (d < 0 && ndups < MAXDUPS) {
        dups[ndups].blk = b + i;
        dups[ndups].claims = 0;
        ndups++;
      }
      dupblk++;
      if (dupblk > 10)
        return -1;
    }
  }
  counted += nfrags;
  return 0;
}

static void checkinode(uint64_t ino) {
  uchar *dp = ginode(ino);
  uint mode, i, ndb, j;
  uint64_t size, lndb;
  int r;

  if (dp == 0)
    return;
  mode = DI_MODE(dp) & IFMT;
  size = DI_SIZE(dp);
  if (mode == 0) {
    int partial = DI_MODE(dp) != 0 || size != 0;
    for (i = 0; i < NDADDR && !partial; i++)
      if (DI_DB(dp, i) != 0)
        partial = 1;
    for (i = 0; i < NIADDR && !partial; i++)
      if (DI_IB(dp, i) != 0)
        partial = 1;
    if (partial) {
      printf("PARTIALLY ALLOCATED INODE I=%llu", ino);
      problems++;
      if (reply("CLEAR")) {
        clearinode(dp);
        inodirty();
      }
    }
    state[ino] = USTATE;
    return;
  }
  if (size > maxfilesize || (mode == IFDIR && size > MAXDIRSIZE))
    goto unknown;
  lndb = howmany(size, bsize);
  if (lndb > 0x7fffffff)
    goto unknown;
  ndb = (uint)lndb;
  if (mode == IFBLK || mode == IFCHR)
    ndb++;
  if (mode == IFLNK && size < maxsymlinklen) {
    ndb = (uint)howmany(size, 8);
    if (ndb > NDADDR) {
      j = ndb - NDADDR;
      for (ndb = 1; j > 1; j--)
        ndb *= nindir;
      ndb += NDADDR;
    }
  }
  for (j = ndb; j < NDADDR; j++)
    if (DI_DB(dp, j) != 0)
      goto unknown;
  for (j = 0, i = ndb > NDADDR ? ndb - NDADDR : 0; i > 0; j++)
    i /= nindir;
  for (; j < NIADDR; j++)
    if (DI_IB(dp, j) != 0)
      goto unknown;
  if (mode != IFDIR && mode != IFREG && mode != IFLNK && mode != IFBLK &&
      mode != IFCHR && mode != IFIFO && mode != IFSOCK && mode != IFWHT)
    goto unknown;
  n_files++;
  lncnt[ino] = DI_NLINK(dp);
  if (mode == IFDIR)
    state[ino] = size == 0 ? DCLEAR : DSTATE;
  else
    state[ino] = FSTATE;
  itype[ino] = dt_of(mode);
  badblk = dupblk = 0;
  counted = 0;
  r = walk_inode(ino, dp, pass1check);
  dp = ginode(ino);
  if (dupblk > 0 || badblk > 0)
    state[ino] = mode == IFDIR ? DCLEAR : FCLEAR;
  if (r >= 0 && !inline_link(dp) && DI_BLOCKS(dp) != counted * (fsize / DEV_BSIZE)) {
    printf("INCORRECT BLOCK COUNT I=%llu (%llu should be %llu)", ino, DI_BLOCKS(dp),
           counted * (fsize / DEV_BSIZE));
    problems++;
    if (reply("CORRECT")) {
      wr64(dp + UFS2_DINODE_BLOCKS, counted * (fsize / DEV_BSIZE));
      inodirty();
    }
  }
  return;
unknown:
  printf("UNKNOWN FILE TYPE I=%llu", ino);
  problems++;
  state[ino] = FCLEAR;
  if (reply("CLEAR")) {
    state[ino] = USTATE;
    clearinode(dp);
    inodirty();
  }
}

static void pass1(void) {
  uint c, i;
  uint64_t d, end, ino;

  for (c = 0; c < ncg; c++) {
    end = cgdmin(c);
    for (d = c == 0 ? cgbase(c) : cgsblock(c); d < end; d++)
      setbit_(bmap, d);
  }
  for (d = csaddr, end = csaddr + howmany(cssize, fsize); d < end; d++)
    setbit_(bmap, d);
  for (c = 0; c < ncg; c++) {
    if (cgload(c) < 0) {
      dprintf(2, "fsck_ffs: cannot read cylinder group %u\n", c);
      exit(1);
    }
    if (rd32(CG(magic)) != UFS2_CG_MAGIC) {
      printf("CG %u: BAD MAGIC NUMBER\n", c);
      problems++;
      unfixed++;
      exit(1);
    }
    initedmax[c] = rd32(CG(initediblk));
    if (initedmax[c] > ipg)
      initedmax[c] = ipg;
    for (i = 0; i < initedmax[c]; i++) {
      ino = (uint64_t)c * ipg + i;
      if (ino < UFS2_ROOT_INO)
        continue;
      checkinode(ino);
    }
  }
  iflush();
}

// PASS 1B

static int pass1bcheck(uint64_t ino, uint64_t b, uint nfrags, int level __unused) {
  uint i;
  int d;

  for (i = 0; i < nfrags; i++) {
    if (!testbit(dupmap, b + i) || (d = dup_slot(b + i)) < 0)
      continue;
    dups[d].claims++;
    if (state[ino] == FSTATE || state[ino] == DSTATE) {
      printf("DUP BLOCK %llu I=%llu\n", b + i, ino);
      problems++;
      state[ino] = state[ino] == DSTATE ? DCLEAR : FCLEAR;
    }
  }
  return 0;
}

static void pass1b(void) {
  uint64_t ino;
  uchar *dp;
  int i;

  if (ndups == 0)
    return;
  for (i = 0; i < ndups; i++)
    dups[i].claims = 0;
  for (ino = UFS2_ROOT_INO; ino < maxino; ino++) {
    if (state[ino] == USTATE)
      continue;
    dp = ginode(ino);
    if (dp == 0)
      continue;
    walk_inode(ino, dp, pass1bcheck);
  }
}

// READING A FILE FOR PASS 2

static int bmap_of(const uchar *dp, uint64_t lbn, uint64_t *out) {
  uint level, idx[NIADDR], l;
  uint64_t base = NDADDR, span = nindir, rest, b;
  uchar raw[8];

  if (lbn < NDADDR) {
    *out = DI_DB(dp, lbn);
    return 0;
  }
  for (level = 1; level <= NIADDR; level++) {
    if (lbn < base + span)
      break;
    base += span;
    span *= nindir;
  }
  if (level > NIADDR)
    return -1;
  rest = lbn - base;
  for (l = level; l > 0; l--) {
    idx[l - 1] = (uint)(rest % nindir);
    rest /= nindir;
  }
  b = DI_IB(dp, level - 1);
  for (l = 0; l < level && b != 0; l++) {
    if (b >= fssize || rdfrag(b, idx[l] * 8, raw, 8) < 0)
      return -1;
    b = rd64(raw);
  }
  *out = b;
  return 0;
}

static int dirchunk(uint64_t ino, uint64_t off, uchar *chunk, int put) {
  uchar *dp = ginode(ino);
  uint64_t b;

  if (dp == 0 || bmap_of(dp, off / bsize, &b) < 0)
    return -1;
  if (b == 0)
    return put ? -1 : 1;
  if (b >= fssize)
    return -1;
  return put ? wrfrag(b, (uint)(off % bsize), chunk, DIRBLKSIZ)
             : rdfrag(b, (uint)(off % bsize), chunk, DIRBLKSIZ);
}

static int record_ok(const uchar *chunk, uint off) {
  uint reclen, namlen, i;

  if (off + UFS2_DIR_HEADER_SIZE > DIRBLKSIZ)
    return 0;
  reclen = rd16(chunk + off + 4);
  namlen = chunk[off + 7];
  if (reclen < UFS2_DIR_HEADER_SIZE || reclen % 4 != 0 || reclen > DIRBLKSIZ - off ||
      namlen > UFS2_MAX_NAME || DIRECTSIZ(namlen) > reclen)
    return 0;
  if (rd32(chunk + off) == 0)
    return 1;
  for (i = 0; i < namlen; i++)
    if (chunk[off + UFS2_DIR_HEADER_SIZE + i] == 0 ||
        chunk[off + UFS2_DIR_HEADER_SIZE + i] == '/')
      return 0;
  return chunk[off + UFS2_DIR_HEADER_SIZE + namlen] == 0;
}

static void put_entry(uchar *at, uint32_t ino, uint16_t reclen, uchar type,
                      const char *name) {
  uint namlen = (uint)strlen(name);

  memset(at, 0, reclen < DIRECTSIZ(namlen) ? DIRECTSIZ(namlen) : reclen);
  wr32(at, ino);
  wr16(at + 4, reclen);
  at[6] = type;
  at[7] = (uchar)namlen;
  memmove(at + UFS2_DIR_HEADER_SIZE, name, namlen);
}

static int name_is(const uchar *chunk, uint off, const char *s) {
  uint n = (uint)strlen(s);

  return chunk[off + 7] == n && memcmp(chunk + off + UFS2_DIR_HEADER_SIZE, s, n) == 0;
}

// PASS 2

static void pass2dir(uint64_t dir) {
  uchar chunk[DIRBLKSIZ], *dp;
  uint64_t off, size;
  uint entryno = 0, o, reclen;
  uint32_t ino;

  dp = ginode(dir);
  size = DI_SIZE(dp);
  if (size % DIRBLKSIZ != 0) {
    printf("DIRECTORY I=%llu: LENGTH %llu NOT MULTIPLE OF %d", dir, size, DIRBLKSIZ);
    problems++;
    size = roundup(size, DIRBLKSIZ);
    if (reply("ADJUST")) {
      wr64(dp + UFS2_DINODE_SIZE_OFFSET, size);
      inodirty();
    }
  }
  for (off = 0; off < size; off += DIRBLKSIZ) {
    int altered = 0, got = dirchunk(dir, off, chunk, 0);

    if (got < 0)
      return;
    if (got > 0) {
      printf("DIRECTORY I=%llu: CONTAINS EMPTY BLOCKS", dir);
      problems++;
      if (reply("ADJUST LENGTH")) {
        dp = ginode(dir);
        wr64(dp + UFS2_DINODE_SIZE_OFFSET, off & ~((uint64_t)bsize - 1));
        inodirty();
      }
      return;
    }
    for (o = 0; o < DIRBLKSIZ; o += reclen) {
      if (!record_ok(chunk, o)) {
        printf("DIRECTORY I=%llu CORRUPTED AT OFFSET %llu", dir, off + o);
        problems++;
        if (reply("SALVAGE")) {
          memset(chunk + o, 0, DIRBLKSIZ - o);
          wr16(chunk + o + 4, (uint16_t)(DIRBLKSIZ - o));
          altered = 1;
        }
        break;
      }
      reclen = rd16(chunk + o + 4);
      ino = rd32(chunk + o);
      if (off == 0 && o == 0) {
        if (ino != 0 && name_is(chunk, o, ".")) {
          if (ino != dir) {
            printf("BAD INODE NUMBER FOR '.' I=%llu", dir);
            problems++;
            if (reply("FIX")) {
              wr32(chunk + o, (uint32_t)dir);
              altered = 1;
            }
          }
          if (chunk[o + 6] != UFS2_DIRTYPE_DIR) {
            printf("BAD TYPE VALUE FOR '.' I=%llu", dir);
            problems++;
            if (reply("FIX")) {
              chunk[o + 6] = UFS2_DIRTYPE_DIR;
              altered = 1;
            }
          }
          lncnt[dir]--;
          entryno = 1;
          continue;
        }
        printf("MISSING '.' I=%llu", dir);
        problems++;
        if (ino != 0 && !name_is(chunk, o, "..")) {
          printf("\nCANNOT FIX, FIRST ENTRY IN DIRECTORY CONTAINS %.*s\n",
                 chunk[o + 7], chunk + o + UFS2_DIR_HEADER_SIZE);
          unfixed++;
        } else if (reclen < 2 * 12) {
          put_entry(chunk + o, (uint32_t)dir, (uint16_t)reclen, UFS2_DIRTYPE_DIR, ".");
          if (!reply("FIX")) {
            unfixed++;
            break;
          }
          altered = 1;
        } else {
          uchar rest[DIRBLKSIZ];
          memmove(rest, chunk + o, reclen);
          put_entry(chunk + o, (uint32_t)dir, 12, UFS2_DIRTYPE_DIR, ".");
          memmove(chunk + o + 12, rest, reclen - 12);
          wr16(chunk + o + 12 + 4, (uint16_t)(reclen - 12));
          if (!reply("FIX")) {
            unfixed++;
            break;
          }
          altered = 1;
        }
        lncnt[dir]--;
        entryno = 1;
        reclen = rd16(chunk + o + 4);
        continue;
      }
      if (entryno == 1) {
        if (ino != 0 && name_is(chunk, o, "..")) {
          dotdot[dir] = ino;
          if (chunk[o + 6] != UFS2_DIRTYPE_DIR) {
            printf("BAD TYPE VALUE FOR '..' I=%llu", dir);
            problems++;
            if (reply("FIX")) {
              chunk[o + 6] = UFS2_DIRTYPE_DIR;
              altered = 1;
            }
          }
          entryno = 2;
          continue;
        }
        printf("MISSING '..' I=%llu", dir);
        problems++;
        if (ino != 0) {
          printf("\nCANNOT FIX, SECOND ENTRY IN DIRECTORY CONTAINS %.*s\n",
                 chunk[o + 7], chunk + o + UFS2_DIR_HEADER_SIZE);
          unfixed++;
          dotdot[dir] = 0;
        } else if (reclen < 12) {
          printf("\nCANNOT FIX, INSUFFICIENT SPACE TO ADD '..'\n");
          unfixed++;
          dotdot[dir] = 0;
        } else {
          put_entry(chunk + o, 0, (uint16_t)reclen, UFS2_DIRTYPE_DIR, "..");
          dotdot[dir] = 0;
          if (reply("FIX"))
            altered = 1;
        }
        entryno = 2;
        continue;
      }
      entryno++;
      if (ino == 0)
        continue;
      if (name_is(chunk, o, ".") || name_is(chunk, o, "..")) {
        printf("EXTRA '%s' ENTRY I=%llu", name_is(chunk, o, ".") ? "." : "..", dir);
        problems++;
        if (reply("FIX")) {
          wr32(chunk + o, 0);
          altered = 1;
        }
        continue;
      }
      if (ino >= maxino) {
        printf("I OUT OF RANGE I=%d OWNER I=%llu", ino, dir);
        problems++;
        if (reply("REMOVE")) {
          wr32(chunk + o, 0);
          altered = 1;
        }
        continue;
      }
      switch (state[ino]) {
      case USTATE:
        printf("UNALLOCATED I=%d OWNER I=%llu", ino, dir);
        problems++;
        if (reply("REMOVE")) {
          wr32(chunk + o, 0);
          altered = 1;
        }
        continue;
      case DCLEAR:
      case FCLEAR:
        printf("%s I=%d OWNER I=%llu", state[ino] == FCLEAR ? "DUP/BAD" : "ZERO LENGTH DIRECTORY",
               ino, dir);
        problems++;
        if (reply("REMOVE")) {
          wr32(chunk + o, 0);
          altered = 1;
        }
        continue;
      case DSTATE:
      case DFOUND:
        if (parent[ino] != 0) {
          printf("I=%llu IS AN EXTRANEOUS HARD LINK TO DIRECTORY I=%d", dir, ino);
          problems++;
          if (reply("REMOVE")) {
            wr32(chunk + o, 0);
            altered = 1;
            continue;
          }
        } else {
          parent[ino] = (uint32_t)dir;
        }
        /* fall through */
      case FSTATE:
        if (chunk[o + 6] != itype[ino]) {
          printf("BAD TYPE VALUE I=%d OWNER I=%llu", ino, dir);
          problems++;
          if (reply("FIX")) {
            chunk[o + 6] = itype[ino];
            altered = 1;
          }
        }
        lncnt[ino]--;
        break;
      }
    }
    if (altered)
      dirchunk(dir, off, chunk, 1);
  }
}

static int set_dotdot(uint64_t dir, uint32_t to) {
  uchar chunk[DIRBLKSIZ];
  uint o = 0, reclen;

  if (dirchunk(dir, 0, chunk, 0) != 0)
    return -1;
  if (!record_ok(chunk, 0))
    return -1;
  reclen = rd16(chunk + 4);
  if (rd32(chunk) != 0 && name_is(chunk, 0, "."))
    o = reclen;
  if (!record_ok(chunk, o) || !name_is(chunk, o, ".."))
    return -1;
  wr32(chunk + o, to);
  dotdot[dir] = to;
  return dirchunk(dir, 0, chunk, 1);
}

static int remove_entry(uint64_t dir, uint32_t ino) {
  uchar chunk[DIRBLKSIZ], *dp = ginode(dir);
  uint64_t off, size;
  uint o, reclen;

  if (dp == 0)
    return -1;
  size = DI_SIZE(dp);
  for (off = 0; off < size; off += DIRBLKSIZ) {
    if (dirchunk(dir, off, chunk, 0) != 0)
      continue;
    for (o = 0; o < DIRBLKSIZ && record_ok(chunk, o); o += reclen) {
      reclen = rd16(chunk + o + 4);
      if (rd32(chunk + o) != ino || name_is(chunk, o, ".") || name_is(chunk, o, ".."))
        continue;
      wr32(chunk + o, 0);
      return dirchunk(dir, off, chunk, 1);
    }
  }
  return -1;
}

static void propagate(void) {
  int changed = 1;
  uint64_t ino;

  while (changed) {
    changed = 0;
    for (ino = UFS2_ROOT_INO; ino < maxino; ino++)
      if (state[ino] == DSTATE && parent[ino] != 0 && state[parent[ino]] == DFOUND) {
        state[ino] = DFOUND;
        changed = 1;
      }
  }
}

static void pass2(void) {
  uint64_t ino;

  switch (state[UFS2_ROOT_INO]) {
  case USTATE:
    printf("ROOT INODE UNALLOCATED\n");
    problems++;
    unfixed++;
    exit(1);
  case DCLEAR:
    printf("DUPS/BAD IN ROOT INODE\n");
    problems++;
    unfixed++;
    exit(1);
  case FSTATE:
  case FCLEAR: {
    uchar *dp = ginode(UFS2_ROOT_INO);
    printf("ROOT INODE NOT DIRECTORY");
    problems++;
    if (!reply("FIX"))
      exit(1);
    wr16(dp + UFS2_DINODE_MODE, (uint16_t)((DI_MODE(dp) & ~IFMT) | IFDIR));
    inodirty();
    break;
  }
  }
  state[UFS2_ROOT_INO] = DFOUND;
  parent[UFS2_ROOT_INO] = UFS2_ROOT_INO;
  for (ino = UFS2_ROOT_INO; ino < maxino; ino++)
    if (state[ino] == DSTATE || state[ino] == DFOUND)
      pass2dir(ino);
  for (ino = UFS2_ROOT_INO + 1; ino < maxino; ino++) {
    if ((state[ino] != DSTATE && state[ino] != DFOUND) || DI_SIZE(ginode(ino)) != 0)
      continue;
    state[ino] = DCLEAR;
    if (parent[ino] != 0) {
      printf("ZERO LENGTH DIRECTORY I=%llu OWNER I=%d", ino, parent[ino]);
      problems++;
      if (reply("REMOVE") && remove_entry(parent[ino], (uint32_t)ino) == 0)
        lncnt[ino]++;
    }
  }
  for (ino = UFS2_ROOT_INO; ino < maxino; ino++) {
    uint32_t dd;

    if (state[ino] != DSTATE && state[ino] != DFOUND)
      continue;
    dd = dotdot[ino];
    if (dd >= maxino || (state[dd] != DSTATE && state[dd] != DFOUND))
      dd = 0;
    if (dd != 0)
      lncnt[dd]--;
    if (parent[ino] == 0)
      continue;
    if (dd == parent[ino])
      continue;
    if (dotdot[ino] != 0)
      printf("BAD INODE NUMBER FOR '..' I=%llu (%d should be %d)", ino, dotdot[ino],
             parent[ino]);
    else
      printf("MISSING '..' I=%llu", ino);
    problems++;
    if (reply("FIX") && set_dotdot(ino, parent[ino]) == 0) {
      lncnt[parent[ino]]--;
      if (dd != 0)
        lncnt[dd]++;
    }
  }
  propagate();
  iflush();
}

static uint64_t allocblk(uint frags) {
  uint64_t i;
  uint j, k;

  if (frags == 0 || frags > frag)
    return 0;
  for (i = 0; i + frag <= fssize; i += frag) {
    for (j = 0; j + frags <= frag; j++) {
      if (testbit(bmap, i + j))
        continue;
      for (k = 1; k < frags; k++)
        if (testbit(bmap, i + j + k))
          break;
      if (k < frags) {
        j += k;
        continue;
      }
      if (chkrange(i + j, frags))
        break;
      for (k = 0; k < frags; k++)
        setbit_(bmap, i + j + k);
      n_blks += frags;
      return i + j;
    }
  }
  return 0;
}

static void freeblk(uint64_t b, uint frags) {
  uint k;
  int d;

  for (k = 0; k < frags; k++) {
    if (testbit(dupmap, b + k)) {
      if ((d = dup_slot(b + k)) >= 0 && --dups[d].claims > 0)
        continue;
      clrbit_(dupmap, b + k);
    }
    clrbit_(bmap, b + k);
    n_blks--;
  }
}

static int freeblkfn(uint64_t ino __unused, uint64_t b, uint nfrags, int level __unused) {
  if (!chkrange(b, nfrags))
    freeblk(b, nfrags);
  return 0;
}

static void clri(uint64_t ino, const char *why) {
  uchar *dp = ginode(ino);

  printf("%s %s I=%llu", why, (DI_MODE(dp) & IFMT) == IFDIR ? "DIR" : "FILE", ino);
  problems++;
  if (reply("CLEAR")) {
    n_files--;
    walk_inode(ino, dp, freeblkfn);
    dp = ginode(ino);
    clearinode(dp);
    inodirty();
    state[ino] = USTATE;
  }
}

static int init_inode_block(uint64_t ino) {
  uint c = (uint)(ino / ipg), i;
  uint64_t fragno = cgimin(c) + (uint64_t)((ino % ipg) / inopb) * frag;

  iflush();
  memset(blk, 0, bsize);
  for (i = 0; i < inopb; i++) {
    uint32_t gen = 0;
    while (gen == 0)
      getentropy(&gen, sizeof(gen));
    wr32(blk + i * UFS2_DINODE_SIZE + UFS2_DINODE_GEN, gen);
  }
  if (wrfrag(fragno, 0, blk, bsize) < 0)
    return -1;
  initedmax[c] += inopb;
  return 0;
}

static uint64_t allocino(uint16_t mode) {
  uint64_t ino, b;
  uchar *dp;
  struct fnu_timespec now;

  for (ino = UFS2_ROOT_INO + 1; ino < maxino; ino++)
    if (state[ino] == USTATE && ino % ipg < initedmax[ino / ipg])
      break;
  if (ino >= maxino) {
    uint c;
    for (c = 0, ino = maxino; c < ncg; c++)
      if (initedmax[c] < ipg) {
        ino = (uint64_t)c * ipg + initedmax[c];
        if (init_inode_block(ino) < 0)
          return 0;
        break;
      }
    if (ino >= maxino)
      return 0;
  }
  if ((b = allocblk(1)) == 0)
    return 0;
  dp = ginode(ino);
  clearinode(dp);
  fnu_gettime(&now, FNU_CLOCK_WALL);
  wr16(dp + UFS2_DINODE_MODE, mode);
  wr64(dp + UFS2_DINODE_DIRECT_OFFSET, b);
  wr64(dp + UFS2_DINODE_ATIME, (uint64_t)now.sec);
  wr64(dp + UFS2_DINODE_MTIME, (uint64_t)now.sec);
  wr64(dp + UFS2_DINODE_CTIME, (uint64_t)now.sec);
  wr64(dp + UFS2_DINODE_BIRTHTIME, (uint64_t)now.sec);
  wr64(dp + UFS2_DINODE_SIZE_OFFSET, fsize);
  wr64(dp + UFS2_DINODE_BLOCKS, btodb(fsize));
  inodirty();
  state[ino] = (mode & IFMT) == IFDIR ? DSTATE : FSTATE;
  itype[ino] = dt_of(mode);
  n_files++;
  return ino;
}

static int makeentry(uint64_t dir, uint32_t ino, const char *name) {
  uchar chunk[DIRBLKSIZ], *dp;
  uint64_t off, size, lastbn, newblk, oldblk;
  uint o, reclen, dsize, needed = DIRECTSIZ((uint)strlen(name));

  dp = ginode(dir);
  size = DI_SIZE(dp);
  if (size % DIRBLKSIZ) {
    size = roundup(size, DIRBLKSIZ);
    wr64(dp + UFS2_DINODE_SIZE_OFFSET, size);
    inodirty();
  }
  for (off = 0; off < size; off += DIRBLKSIZ) {
    if (dirchunk(dir, off, chunk, 0) != 0)
      return 0;
    for (o = 0; o < DIRBLKSIZ && record_ok(chunk, o); o += reclen) {
      reclen = rd16(chunk + o + 4);
      dsize = rd32(chunk + o) == 0 ? 0 : DIRECTSIZ(chunk[o + 7]);
      if (reclen - dsize < needed)
        continue;
      wr16(chunk + o + 4, (uint16_t)dsize);
      put_entry(chunk + o + dsize, ino, (uint16_t)(reclen - dsize), itype[ino], name);
      return dirchunk(dir, off, chunk, 1) == 0;
    }
  }
  dp = ginode(dir);
  lastbn = size / bsize;
  if (lastbn >= NDADDR - 1 || DI_DB(dp, lastbn) == 0 || size == 0)
    return 0;
  if ((newblk = allocblk(frag)) == 0)
    return 0;
  oldblk = DI_DB(dp, lastbn);
  wr64(dp + UFS2_DINODE_DIRECT_OFFSET + (lastbn + 1) * 8, oldblk);
  wr64(dp + UFS2_DINODE_DIRECT_OFFSET + lastbn * 8, newblk);
  wr64(dp + UFS2_DINODE_SIZE_OFFSET, size + bsize);
  wr64(dp + UFS2_DINODE_BLOCKS, DI_BLOCKS(dp) + btodb(bsize));
  inodirty();
  if (rdfrag(oldblk, 0, chunk, DIRBLKSIZ) < 0)
    return 0;
  memset(blk, 0, bsize);
  memmove(blk, chunk, DIRBLKSIZ);
  for (o = DIRBLKSIZ; o < bsize; o += DIRBLKSIZ)
    wr16(blk + o + 4, DIRBLKSIZ);
  if (wrfrag(newblk, 0, blk, bsize) < 0)
    return 0;
  memset(chunk, 0, DIRBLKSIZ);
  wr16(chunk + 4, DIRBLKSIZ);
  if (wrfrag(oldblk, 0, chunk, DIRBLKSIZ) < 0)
    return 0;
  printf("NO SPACE LEFT IN I=%llu (EXPANDED)\n", dir);
  return makeentry(dir, ino, name);
}

static uint64_t allocdir(uint64_t parentino, uint16_t mode) {
  uint64_t ino = allocino((uint16_t)(IFDIR | mode));
  uchar *dp, chunk[DIRBLKSIZ];
  uint o;

  if (ino == 0)
    return 0;
  dp = ginode(ino);
  memset(blk, 0, fsize);
  put_entry(blk, (uint32_t)ino, 12, UFS2_DIRTYPE_DIR, ".");
  put_entry(blk + 12, (uint32_t)parentino, DIRBLKSIZ - 12, UFS2_DIRTYPE_DIR, "..");
  for (o = DIRBLKSIZ; o < fsize; o += DIRBLKSIZ)
    wr16(blk + o + 4, DIRBLKSIZ);
  memset(chunk, 0, sizeof(chunk));
  if (wrfrag(DI_DB(dp, 0), 0, blk, fsize) < 0)
    return 0;
  wr16(dp + UFS2_DINODE_NLINK, 2);
  wr32(dp + UFS2_DINODE_UID, 0);
  wr32(dp + UFS2_DINODE_GID, 0);
  inodirty();
  lncnt[ino] = 0;
  parent[ino] = (uint32_t)parentino;
  dotdot[ino] = (uint32_t)parentino;
  state[ino] = state[parentino];
  dp = ginode(parentino);
  wr16(dp + UFS2_DINODE_NLINK, (uint16_t)(DI_NLINK(dp) + 1));
  inodirty();
  return ino;
}

static int find_in_root(const char *name, uint32_t *out) {
  uchar chunk[DIRBLKSIZ], *dp = ginode(UFS2_ROOT_INO);
  uint64_t off, size = DI_SIZE(dp);
  uint o, reclen;

  for (off = 0; off < size; off += DIRBLKSIZ) {
    if (dirchunk(UFS2_ROOT_INO, off, chunk, 0) != 0)
      return -1;
    for (o = 0; o < DIRBLKSIZ && record_ok(chunk, o); o += reclen) {
      reclen = rd16(chunk + o + 4);
      if (rd32(chunk + o) != 0 && name_is(chunk, o, name)) {
        *out = rd32(chunk + o);
        return 0;
      }
    }
  }
  return -1;
}

static int linkup(uint64_t orphan, uint32_t parentdir) {
  uchar *dp = ginode(orphan);
  int lostdir = (DI_MODE(dp) & IFMT) == IFDIR;
  char tempname[32];

  printf("UNREF %s I=%llu", lostdir ? "DIR" : "FILE", orphan);
  problems++;
  if (!reply("RECONNECT"))
    return 0;
  if (lfdir == 0) {
    if (find_in_root(LFNAME, &lfdir) < 0) {
      printf("NO %s DIRECTORY", LFNAME);
      if (reply("CREATE")) {
        lfdir = (uint32_t)allocdir(UFS2_ROOT_INO, LFMODE);
        if (lfdir != 0 && !makeentry(UFS2_ROOT_INO, lfdir, LFNAME)) {
          clri(lfdir, "UNUSABLE");
          lfdir = 0;
        }
      }
    }
    if (lfdir == 0) {
      printf("SORRY. CANNOT CREATE %s DIRECTORY\n", LFNAME);
      unfixed++;
      return 0;
    }
  }
  dp = ginode(lfdir);
  if ((DI_MODE(dp) & IFMT) != IFDIR || state[lfdir] != DFOUND) {
    printf("%s IS NOT A DIRECTORY\n", LFNAME);
    unfixed++;
    return 0;
  }
  snprintf(tempname, sizeof(tempname), "#%llu", orphan);
  if (!makeentry(lfdir, (uint32_t)orphan, tempname)) {
    printf("SORRY. NO SPACE IN %s DIRECTORY\n", LFNAME);
    unfixed++;
    return 0;
  }
  lncnt[orphan]--;
  if (lostdir) {
    if (set_dotdot(orphan, lfdir) < 0)
      return 0;
    dp = ginode(lfdir);
    wr16(dp + UFS2_DINODE_NLINK, (uint16_t)(DI_NLINK(dp) + 1));
    inodirty();
    parent[orphan] = lfdir;
    printf("DIR I=%llu CONNECTED. PARENT WAS I=%d\n", orphan, parentdir);
    if (parentdir != 0 && parentdir < maxino &&
        (state[parentdir] == DSTATE || state[parentdir] == DFOUND))
      lncnt[parentdir]++;
    state[orphan] = DFOUND;
    propagate();
  }
  return 1;
}

// PASS 3

static void pass3(void) {
  uint64_t ino, orphan;
  uint loops;

  for (ino = UFS2_ROOT_INO; ino < maxino; ino++) {
    if (state[ino] != DSTATE)
      continue;
    orphan = ino;
    for (loops = 0; parent[orphan] != 0 && state[parent[orphan]] == DSTATE && loops < maxino;
         loops++)
      orphan = parent[orphan];
    if (state[orphan] != DSTATE)
      continue;
    linkup(orphan, dotdot[orphan]);
  }
  iflush();
}

// PASS 4

static void adjust(uint64_t ino, int lcnt) {
  uchar *dp = ginode(ino);

  if (lcnt == DI_NLINK(dp) && lcnt > 0) {
    if (DI_SIZE(dp) == 0 || !linkup(ino, 0))
      clri(ino, "UNREF");
    return;
  }
  printf("LINK COUNT %s I=%llu COUNT %d SHOULD BE %d",
         (DI_MODE(dp) & IFMT) == IFDIR ? "DIR" : "FILE", ino, DI_NLINK(dp),
         DI_NLINK(dp) - lcnt);
  problems++;
  if (reply("ADJUST")) {
    wr16(dp + UFS2_DINODE_NLINK, (uint16_t)(DI_NLINK(dp) - lcnt));
    inodirty();
  }
}

static void pass4(void) {
  uint64_t ino;
  uchar *dp;

  for (ino = UFS2_ROOT_INO; ino < maxino; ino++) {
    switch (state[ino]) {
    case FSTATE:
    case DFOUND:
      if (lncnt[ino] != 0)
        adjust(ino, lncnt[ino]);
      else if (DI_NLINK(ginode(ino)) <= 0)
        clri(ino, "UNREF");
      break;
    case DSTATE:
      clri(ino, "UNREF");
      break;
    case DCLEAR:
      dp = ginode(ino);
      if (DI_SIZE(dp) == 0) {
        clri(ino, "ZERO LENGTH");
        break;
      }
      /* fall through */
    case FCLEAR:
      clri(ino, "BAD/DUP");
      break;
    }
  }
  iflush();
}

// PASS 5

static void fragacct(uint fragmap, uchar *frsum, int cnt) {
  uint pos = 0;

  if (fragmap == (1u << frag) - 1)
    return;
  while (pos < frag) {
    uint run = 0;
    while (pos < frag && (fragmap & (1u << pos))) {
      run++;
      pos++;
    }
    if (run > 0)
      add32(frsum + run * 4, cnt);
    pos++;
  }
}

static void pass5(void) {
  uchar *newcg, *cs;
  uint c, i, j, inomapsize, blkmapsize, mapsize;
  uint64_t d, dbase, dmax, ino;
  int64_t cstotal[4] = {0, 0, 0, 0};
  int sbchanged = 0;

  if ((newcg = malloc(cgsize)) == 0 || (cs = malloc((size_t)cssize)) == 0) {
    dprintf(2, "fsck_ffs: out of memory\n");
    exit(1);
  }
  if (rdfrag(csaddr, 0, cs, (uint)cssize) < 0)
    exit(1);
  dmax = (fssize + frag - 1) & ~(uint64_t)(frag - 1);
  for (d = fssize; d < dmax; d++)
    setbit_(bmap, d);
  inomapsize = howmany(ipg, NBBY);
  blkmapsize = howmany(fpg, NBBY);
  for (c = 0; c < ncg; c++) {
    uint iusedoff, freeoff, nextfreeoff, clustersumoff = 0, clusteroff = 0;
    uint32_t rotor, frotor, irotor;
    uchar *inosused, *blksfree;

    if (cgload(c) < 0)
      exit(1);
    dbase = cgbase(c);
    dmax = dbase + fpg;
    if (dmax > fssize)
      dmax = fssize;
    memset(newcg, 0, cgsize);
    iusedoff = UFS2_CG_HEADER_SIZE;
    freeoff = iusedoff + inomapsize;
    nextfreeoff = freeoff + blkmapsize;
    if (contigsumsize > 0) {
      clustersumoff = roundup(nextfreeoff, 4) - 4;
      clusteroff = clustersumoff + (contigsumsize + 1) * 4;
      nextfreeoff = clusteroff + howmany(fpg / frag, NBBY);
    }
    mapsize = nextfreeoff - iusedoff;
    memmove(newcg + UFS2_CG_time, CG(time), 8);
    memmove(newcg + UFS2_CG_old_time, CG(old_time), 4);
    wr32(newcg + UFS2_CG_magic, UFS2_CG_MAGIC);
    wr32(newcg + UFS2_CG_cgx, c);
    wr32(newcg + UFS2_CG_ndblk, (uint32_t)(dmax - dbase));
    if (contigsumsize > 0)
      wr32(newcg + UFS2_CG_nclusterblks, (uint32_t)((dmax - dbase) / frag));
    wr32(newcg + UFS2_CG_cs + 8, ipg);
    rotor = rd32(CG(rotor));
    frotor = rd32(CG(frotor));
    irotor = rd32(CG(irotor));
    wr32(newcg + UFS2_CG_rotor, rotor < dmax - dbase ? rotor : 0);
    wr32(newcg + UFS2_CG_frotor, frotor < dmax - dbase ? frotor : 0);
    wr32(newcg + UFS2_CG_irotor, irotor < ipg ? irotor : 0);
    wr32(newcg + UFS2_CG_initediblk, initedmax[c]);
    wr32(newcg + UFS2_CG_niblk, ipg);
    wr32(newcg + UFS2_CG_iusedoff, iusedoff);
    wr32(newcg + UFS2_CG_freeoff, freeoff);
    wr32(newcg + UFS2_CG_nextfreeoff, nextfreeoff);
    wr32(newcg + UFS2_CG_clustersumoff, clustersumoff);
    wr32(newcg + UFS2_CG_clusteroff, clusteroff);
    memmove(newcg + UFS2_CG_unrefs, CG(unrefs), UFS2_CG_time - UFS2_CG_unrefs);
    memmove(newcg + UFS2_CG_time + 8, CG(time) + 8, UFS2_CG_HEADER_SIZE - UFS2_CG_time - 8);
    inosused = newcg + iusedoff;
    blksfree = newcg + freeoff;
    for (i = 0; i < initedmax[c]; i++) {
      ino = (uint64_t)c * ipg + i;
      switch (state[ino]) {
      case USTATE:
        break;
      case DSTATE:
      case DCLEAR:
      case DFOUND:
        add32(newcg + UFS2_CG_cs + 0, 1);
        /* fall through */
      case FSTATE:
      case FCLEAR:
        add32(newcg + UFS2_CG_cs + 8, -1);
        setbit_(inosused, i);
        break;
      }
    }
    if (c == 0)
      for (i = 0; i < UFS2_ROOT_INO; i++) {
        setbit_(inosused, i);
        add32(newcg + UFS2_CG_cs + 8, -1);
      }
    for (i = 0, d = dbase; d < dmax; d += frag, i += frag) {
      uint frags = 0, fragmap = 0;
      for (j = 0; j < frag; j++) {
        if (d + j >= dmax || testbit(bmap, d + j))
          continue;
        setbit_(blksfree, i + j);
        fragmap |= 1u << j;
        frags++;
      }
      if (frags == frag) {
        add32(newcg + UFS2_CG_cs + 4, 1);
        if (contigsumsize > 0)
          setbit_(newcg + clusteroff, i / frag);
      } else if (frags > 0) {
        add32(newcg + UFS2_CG_cs + 12, (int)frags);
        fragacct(fragmap, newcg + UFS2_CG_frsum, 1);
      }
    }
    if (contigsumsize > 0) {
      uchar *sump = newcg + clustersumoff, *mapp = newcg + clusteroff;
      uint run = 0, nclusterblks = rd32(newcg + UFS2_CG_nclusterblks);
      for (i = 0; i < nclusterblks; i++) {
        if (testbit(mapp, i))
          run++;
        else if (run != 0) {
          if (run > contigsumsize)
            run = contigsumsize;
          add32(sump + run * 4, 1);
          run = 0;
        }
      }
      if (run != 0) {
        if (run > contigsumsize)
          run = contigsumsize;
        add32(sump + run * 4, 1);
      }
    }
    cstotal[0] += (int32_t)rd32(newcg + UFS2_CG_cs + 0);
    cstotal[1] += (int32_t)rd32(newcg + UFS2_CG_cs + 4);
    cstotal[2] += (int32_t)rd32(newcg + UFS2_CG_cs + 8);
    cstotal[3] += (int32_t)rd32(newcg + UFS2_CG_cs + 12);
    if (memcmp(newcg + UFS2_CG_cs, cs + c * 16, 16) != 0) {
      printf("FREE BLK COUNT(S) WRONG IN SUPERBLK");
      problems++;
      if (reply("SALVAGE")) {
        memmove(cs + c * 16, newcg + UFS2_CG_cs, 16);
        sbchanged = 1;
      }
    }
    if (memcmp(newcg, cgbuf, UFS2_CG_HEADER_SIZE) != 0) {
      printf("SUMMARY INFORMATION BAD CG %u", c);
      problems++;
      if (reply("SALVAGE")) {
        memmove(cgbuf, newcg, UFS2_CG_HEADER_SIZE);
        cgdirty = 1;
      }
    }
    if (memcmp(newcg + iusedoff, cgbuf + iusedoff, mapsize) != 0) {
      printf("BLK(S) MISSING IN BIT MAPS CG %u", c);
      problems++;
      if (reply("SALVAGE")) {
        memmove(cgbuf + iusedoff, newcg + iusedoff, mapsize);
        cgdirty = 1;
      }
    }
  }
  cgflush();
  if ((int64_t)rd64(SB(cstotal) + 0) != cstotal[0] || (int64_t)rd64(SB(cstotal) + 8) != cstotal[1] ||
      (int64_t)rd64(SB(cstotal) + 16) != cstotal[2] || (int64_t)rd64(SB(cstotal) + 24) != cstotal[3]) {
    printf("FREE BLK COUNT(S) WRONG IN SUPERBLK");
    problems++;
    if (reply("SALVAGE")) {
      wr64(SB(cstotal) + 0, (uint64_t)cstotal[0]);
      wr64(SB(cstotal) + 8, (uint64_t)cstotal[1]);
      wr64(SB(cstotal) + 16, (uint64_t)cstotal[2]);
      wr64(SB(cstotal) + 24, (uint64_t)cstotal[3]);
      sb[UFS2_SB_ronly] = 0;
      sb[UFS2_SB_fmod] = 0;
      sbchanged = 1;
    }
  }
  if (sbchanged && fix)
    wrfrag(csaddr, 0, cs, (uint)cssize);
  free(newcg);
  free(cs);
}

static int power_of_two(uint v) { return v != 0 && (v & (v - 1)) == 0; }

// SETUP

static int setup(const char *path) {
  uint64_t bytes = 0;

  if (rdbytes(SBLOCK_UFS2, sb, sizeof(sb)) < 0)
    return -1;
  if (rd32(SB(magic)) != UFS2_MAGIC) {
    dprintf(2, "fsck_ffs: %s carries no UFS2 filesystem\n", path);
    return -1;
  }
  bsize = rd32(SB(bsize));
  fsize = rd32(SB(fsize));
  frag = rd32(SB(frag));
  sbsize = rd32(SB(sbsize));
  ncg = rd32(SB(ncg));
  ipg = rd32(SB(ipg));
  fpg = rd32(SB(fpg));
  cgsize = rd32(SB(cgsize));
  inopb = rd32(SB(inopb));
  nindir = rd32(SB(nindir));
  fssize = rd64(SB(size));
  dsize = rd64(SB(dsize));
  cssize = rd32(SB(cssize));
  csaddr = rd64(SB(csaddr));
  sblkno = rd32(SB(sblkno));
  cblkno = rd32(SB(cblkno));
  iblkno = rd32(SB(iblkno));
  dblkno = rd32(SB(dblkno));
  contigsumsize = rd32(SB(contigsumsize));
  maxsymlinklen = rd32(SB(maxsymlinklen));
  maxfilesize = rd64(SB(maxfilesize));
  if (!power_of_two(bsize) || bsize < 4096 || bsize > 65536 || !power_of_two(fsize) ||
      fsize < DEV_BSIZE || fsize > bsize || frag != bsize / fsize || frag > UFS2_MAXFRAG ||
      sbsize < 1376 || sbsize > UFS2_SUPERBLOCK_SIZE || inopb != bsize / UFS2_DINODE_SIZE ||
      nindir != bsize / 8 || ncg == 0 || ipg == 0 || ipg % inopb != 0 || fpg == 0 ||
      fpg % frag != 0 || cgsize < UFS2_CG_HEADER_SIZE || cgsize > bsize || fssize == 0 ||
      dsize > fssize || fssize > (uint64_t)fpg * ncg || (uint64_t)fpg * (ncg - 1) >= fssize ||
      cssize < (uint64_t)ncg * 16 || cssize % fsize != 0 || csaddr + cssize / fsize > fssize ||
      dblkno > fpg || contigsumsize > 16) {
    dprintf(2, "fsck_ffs: the superblock of %s describes nothing this can check\n", path);
    return -1;
  }
  fragshift = 0;
  while ((1u << fragshift) < frag)
    fragshift++;
  bshift = 0;
  while ((1u << bshift) < bsize)
    bshift++;
  fshift = 0;
  while ((1u << fshift) < fsize)
    fshift++;
  maxino = (uint64_t)ncg * ipg;
  if (debug)
    dprintf(2, "fsck_ffs: bsize %u fsize %u frag %u ncg %u ipg %u fpg %u "
               "size %lld dsize %lld inodes %lld\n",
            bsize, fsize, frag, ncg, ipg, fpg, (int64_t)fssize, (int64_t)dsize,
            (int64_t)maxino);
  if (ioctl(dev, FNU_DISKSIZE, (uint64_t)(uintptr_t)&bytes) == 0 && bytes != 0 &&
      fssize * fsize > bytes) {
    dprintf(2, "fsck_ffs: %s says it is bigger than the device\n", path);
    return -1;
  }
  cgbuf = malloc(cgsize);
  blk = malloc(bsize);
  iblk = malloc(bsize);
  bmap = malloc((size_t)howmany(fssize + frag, NBBY));
  dupmap = malloc((size_t)howmany(fssize + frag, NBBY));
  state = malloc((size_t)maxino);
  itype = malloc((size_t)maxino);
  lncnt = malloc((size_t)maxino * sizeof(int32_t));
  parent = malloc((size_t)maxino * sizeof(uint32_t));
  dotdot = malloc((size_t)maxino * sizeof(uint32_t));
  initedmax = malloc((size_t)ncg * sizeof(uint32_t));
  if (cgbuf == 0 || blk == 0 || iblk == 0 || bmap == 0 || dupmap == 0 || state == 0 ||
      itype == 0 || lncnt == 0 || parent == 0 || dotdot == 0 || initedmax == 0) {
    dprintf(2, "fsck_ffs: not enough memory for %llu fragments and %llu inodes\n", fssize,
            maxino);
    return -1;
  }
  memset(bmap, 0, (size_t)howmany(fssize + frag, NBBY));
  memset(dupmap, 0, (size_t)howmany(fssize + frag, NBBY));
  memset(state, 0, (size_t)maxino);
  memset(itype, 0, (size_t)maxino);
  memset(lncnt, 0, (size_t)maxino * sizeof(int32_t));
  memset(parent, 0, (size_t)maxino * sizeof(uint32_t));
  memset(dotdot, 0, (size_t)maxino * sizeof(uint32_t));
  return 0;
}

// THE LOG
static uint64_t *logfrag;
static uint64_t logsize, logarea;

static int log_rd(uint64_t off, uchar *buf, uint64_t n) {
  while (n > 0) {
    uint64_t lbn = off >> bshift, in = off & (bsize - 1), take = bsize - in;

    if (take > n)
      take = n;
    if (lbn >= logsize >> bshift ||
        rdbytes((int64_t)(logfrag[lbn] * fsize + in), buf, (uint)take) < 0)
      return -1;
    off += take;
    buf += take;
    n -= take;
  }
  return 0;
}

static int ring_rd(uint64_t pos, uchar *buf, uint64_t n) {
  while (n > 0) {
    uint64_t take = logarea - pos < n ? logarea - pos : n;

    if (log_rd(ULOG_AREA + pos, buf, take) < 0)
      return -1;
    pos = (pos + take) % logarea;
    buf += take;
    n -= take;
  }
  return 0;
}

struct lrev {
  uint64_t sector, seq;
  uint32_t bytes;
};

static int lr_revoked(const struct lrev *rv, int nrv, uint64_t sector,
                      uint32_t bytes, uint64_t seq) {
  int i;

  for (i = 0; i < nrv; i++)
    if (rv[i].seq >= seq && rv[i].sector < sector + bytes / ULOG_BLK &&
        sector < rv[i].sector + rv[i].bytes / ULOG_BLK)
      return 1;
  return 0;
}

static void *lr_grow(void *v, int *cap, int used, size_t size) {
  if (used < *cap)
    return v;
  *cap = *cap == 0 ? 32 : *cap * 2;
  if ((v = realloc(v, (size_t)*cap * size)) == 0) {
    dprintf(2, "fsck_ffs: no memory for the log\n");
    exit(8);
  }
  return v;
}

static void replay_log(void) {
  uchar b[ULOG_BLK], h[ULOG_BLK], *dp, *buf = 0;
  uint64_t lbn, gen = 0, tail = 0, seq = 0, pos, scanned = 0;
  struct { uint64_t pos, seq; } *tx = 0;
  uint32_t lino, *orph = 0, *pend = 0, *swap;
  struct lrev *rv = 0;
  int s, found = 0, ntx = 0, txcap = 0, nrv = 0, rvcap = 0, norph = 0, orphcap = 0;
  int npend = 0, pendcap = 0, t, i;

  if (find_in_root(ULOG_NAME, &lino) < 0 || (dp = ginode(lino)) == 0 ||
      (DI_MODE(dp) & IFMT) != IFREG ||
      !(rd32(dp + UFS2_DINODE_FLAGS) & FNU_SF_IMMUTABLE))
    return;
  logsize = DI_SIZE(dp);
  if (logsize <= ULOG_AREA + 4 * ULOG_BLK || logsize & (bsize - 1) ||
      (logfrag = malloc((size_t)(logsize >> bshift) * sizeof(*logfrag))) == 0)
    return;
  for (lbn = 0; lbn < logsize >> bshift; lbn++)
    if (bmap_of(dp, lbn, &logfrag[lbn]) < 0 || logfrag[lbn] == 0 ||
        logfrag[lbn] >= fssize)
      return;
  logarea = (logsize - ULOG_AREA) & ~(uint64_t)(ULOG_BLK - 1);
  for (s = 0; s < 2; s++) {
    uint32_t want;

    if (log_rd((uint64_t)s * ULOG_BLK, b, ULOG_BLK) < 0 ||
        memcmp(b, ulog_magic, sizeof(ulog_magic)) != 0 ||
        rd32(b + ULOG_H_VERSION) != ULOG_VERSION)
      continue;
    want = rd32(b + ULOG_H_CRC);
    wr32(b + ULOG_H_CRC, 0);
    if (ulog_crc(0, b, ULOG_BLK) != want || (found && rd64(b + ULOG_H_GEN) <= gen))
      continue;
    gen = rd64(b + ULOG_H_GEN);
    memcpy(h, b, ULOG_BLK);
    found = 1;
  }
  if (!found || rd32(h + ULOG_H_FSID) != rd32(SB(id)) ||
      rd32(h + ULOG_H_FSID + 4) != rd32(SB(id) + 4) || rd32(h + ULOG_H_INO) != lino ||
      rd64(h + ULOG_H_AREA) != logarea || !(rd32(h + ULOG_H_FLAGS) & ULOG_LIVE) ||
      rd64(h + ULOG_H_MTIME) != rd64(SB(mtime)) || rd64(h + ULOG_H_TAIL) >= logarea)
    return;
  if ((buf = malloc(bsize)) == 0)
    return;
  pos = tail = rd64(h + ULOG_H_TAIL);
  seq = rd64(h + ULOG_H_TAILSEQ);
  for (;;) {
    uint64_t start = pos;
    uint32_t crc = 0;
    int keep = nrv, ok = 0;

    npend = 0;
    for (;;) {
      uint type, count, len, k;
      uint64_t sum = ULOG_BLK;

      if (scanned + ULOG_BLK > logarea || ring_rd(pos, b, ULOG_BLK) < 0 ||
          rd32(b) != LR_MAGIC || rd64(b + LR_SEQ) != seq)
        break;
      type = rd16(b + LR_TYPE);
      count = rd16(b + LR_COUNT);
      len = rd32(b + LR_LEN);
      if (len < ULOG_BLK || len % ULOG_BLK || len >= logarea)
        break;
      if (type == LR_COMMIT) {
        ok = rd32(b + LR_HDR) == crc &&
             (uint64_t)rd32(b + LR_HDR + 4) * ULOG_BLK == (pos + logarea - start) % logarea;
        pos = (pos + ULOG_BLK) % logarea;
        scanned += ULOG_BLK;
        break;
      }
      crc = ulog_crc(crc, b, ULOG_BLK);
      if (type == LR_BLOCKS) {
        uint64_t at = (pos + ULOG_BLK) % logarea;

        if (count > LR_ENTRIES || scanned + len > logarea)
          break;
        for (k = 0; k < count; k++) {
          uint32_t bytes = rd32(b + LR_HDR + k * 16 + 8);

          if (bytes == 0 || bytes % ULOG_BLK || bytes > bsize ||
              rd64(b + LR_HDR + k * 16) + bytes / ULOG_BLK > fssize * (fsize / ULOG_BLK) ||
              ring_rd(at, buf, bytes) < 0)
            break;
          crc = ulog_crc(crc, buf, bytes);
          at = (at + bytes) % logarea;
          sum += bytes;
        }
        if (k < count || sum != len)
          break;
      } else if (type == LR_REVOKE && count <= LR_ENTRIES && len == ULOG_BLK) {
        for (k = 0; k < count; k++) {
          rv = lr_grow(rv, &rvcap, nrv, sizeof(*rv));
          rv[nrv].sector = rd64(b + LR_HDR + k * 16);
          rv[nrv].bytes = rd32(b + LR_HDR + k * 16 + 8);
          rv[nrv].seq = seq;
          nrv++;
        }
      } else if (type == LR_ORPHANS && count <= LR_INOS && len == ULOG_BLK) {
        if (rd32(b + LR_FLAGS) & LR_FIRST)
          npend = 0;
        for (k = 0; k < count; k++) {
          pend = lr_grow(pend, &pendcap, npend, sizeof(*pend));
          pend[npend++] = rd32(b + LR_HDR + k * 4);
        }
      } else {
        break;
      }
      scanned += len;
      pos = (pos + len) % logarea;
    }
    if (!ok) {
      nrv = keep;
      break;
    }
    tx = lr_grow(tx, &txcap, ntx, sizeof(*tx));
    tx[ntx].pos = start;
    tx[ntx++].seq = seq++;
    swap = orph;
    orph = pend;
    norph = npend;
    i = orphcap;
    orphcap = pendcap;
    pend = swap;
    pendcap = i;
  }
  if (ntx == 0)
    goto done;
  if (!fix) {
    printf("** The log holds %d transactions; -y puts them home first\n", ntx);
    goto done;
  }
  for (t = 0; t < ntx; t++) {
    pos = tx[t].pos;
    for (;;) {
      uint64_t at;
      uint k;

      if (ring_rd(pos, b, ULOG_BLK) < 0 || rd16(b + LR_TYPE) == LR_COMMIT)
        break;
      at = (pos + ULOG_BLK) % logarea;
      for (k = 0; rd16(b + LR_TYPE) == LR_BLOCKS && k < rd16(b + LR_COUNT); k++) {
        uint64_t sector = rd64(b + LR_HDR + k * 16);
        uint32_t bytes = rd32(b + LR_HDR + k * 16 + 8);

        if (!lr_revoked(rv, nrv, sector, bytes, tx[t].seq) &&
            (ring_rd(at, buf, bytes) < 0 ||
             wrbytes((int64_t)(sector * ULOG_BLK), buf, bytes) < 0))
          exit(8);
        at = (at + bytes) % logarea;
      }
      pos = (pos + rd32(b + LR_LEN)) % logarea;
    }
  }
  fsync(dev);
  iblkat = ~(uint64_t)0;
  for (i = 0; i < norph; i++)
    if ((dp = ginode(orph[i])) != 0 && DI_MODE(dp) != 0 && DI_NLINK(dp) == 0) {
      clearinode(dp);
      inodirty();
    }
  iflush();
  iblkat = ~(uint64_t)0;
  rdbytes(SBLOCK_UFS2, sb, sizeof(sb));
  printf("** %d transactions from the log are home\n", ntx);
done:
  free(buf);
  free(tx);
  free(rv);
  free(orph);
  free(pend);
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

int main(int argc, char **argv) {
  const char *path;
  uint32_t flags;
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
      else if (*o == 'd')
        debug = 1;
      else if (*o == 'b' || *o == 'c' || *o == 'm') {
        dprintf(2, "fsck_ffs: -%c is not kept here\n", *o);
        exit(1);
      } else {
        dprintf(2, "usage: fsck_ffs [-dfnpy] device\n");
        exit(1);
      }
    }
  }
  if (i != argc - 1) {
    dprintf(2, "usage: fsck_ffs [-dfnpy] device\n");
    exit(1);
  }
  path = argv[i];
  if (fix && mounted(path)) {
    dprintf(2, "fsck_ffs: %s is mounted; unmount it before repairing\n", path);
    exit(1);
  }
  if ((dev = open(path, fix ? O_RDWR : O_RDONLY)) < 0) {
    dprintf(2, "fsck_ffs: cannot open %s\n", path);
    exit(1);
  }
  if (setup(path) < 0)
    exit(1);
  if (sb[UFS2_SB_clean] == 0)
    replay_log();
  flags = rd32(SB(flags));
  if (preen && sb[UFS2_SB_clean] && !(flags & UFS2_FS_NEEDSFSCK) && !force) {
    printf("%s: file system is clean; not checking\n", path);
    exit(0);
  }
  if (!preen) {
    printf("** %s%s\n", path, sb[UFS2_SB_clean] ? "" : " (NOT CLEAN)");
    if (sb[UFS2_SB_clean])
      printf("** File system is already clean\n");
    if (SB(fsmnt)[0] != 0)
      printf("** Last Mounted on %s\n", (char *)SB(fsmnt));
    printf("** Phase 1 - Check Blocks and Sizes\n");
  }
  pass1();
  if (ndups > 0) {
    if (!preen)
      printf("** Phase 1b - Rescan For More DUPS\n");
    pass1b();
  }
  if (!preen)
    printf("** Phase 2 - Check Pathnames\n");
  pass2();
  if (!preen)
    printf("** Phase 3 - Check Connectivity\n");
  pass3();
  if (!preen)
    printf("** Phase 4 - Check Reference Counts\n");
  pass4();
  if (!preen)
    printf("** Phase 5 - Check Cyl groups\n");
  pass5();
  iflush();
  cgflush();
  if (!preen || problems != 0)
    printf("%llu files, %llu used, %llu free (%lld frags, %lld blocks)\n", n_files,
           n_blks, dsize > n_blks ? dsize - n_blks : 0,
           (int64_t)rd64(SB(cstotal) + 24), (int64_t)rd64(SB(cstotal) + 8));
  if (fix && unfixed == 0 &&
      (modified || sb[UFS2_SB_clean] == 0 || (flags & (UFS2_FS_UNCLEAN | UFS2_FS_NEEDSFSCK)))) {
    sb[UFS2_SB_clean] = 1;
    wr32(SB(flags), flags & ~(UFS2_FS_UNCLEAN | UFS2_FS_NEEDSFSCK));
    wrbytes(SBLOCK_UFS2, sb, sbsize);
    printf("\n***** FILE SYSTEM WAS MODIFIED *****\n");
  }
  if (fix)
    fsync(dev);
  close(dev);
  if (problems == 0) {
    if (!preen)
      printf("%s: clean\n", path);
  } else if (preen && unfixed) {
    printf("%s: UNEXPECTED INCONSISTENCY; RUN fsck_ffs MANUALLY.\n", path);
    exit(8);
  } else
    printf("%s: %d problem%s%s\n", path, problems, problems == 1 ? "" : "s",
           fix ? (unfixed ? ", not all put right" : " put right")
               : "; run with -y to correct them");
  exit(problems != 0 && (!fix || unfixed) ? 1 : 0);
}
