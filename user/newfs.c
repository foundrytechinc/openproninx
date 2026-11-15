#include "inc/abi.h"
#include "kern/ufs2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <fnu/sys.h>
#include <err.h>

#define SBLOCKSIZE 8192
#define SBLOCK_UFS2 65536
#define MINBSIZE 4096
#define MAXBSIZE 65536
#define DFL_FRAGSIZE 4096
#define DFL_BLKSIZE 32768
#define MAXPHYS (128 * 1024)
#define FS_MAXCONTIG 16
#define MINCYLGRPS 4
#define MINFREE 8
#define NFPI 2
#define AVFILESIZ 16384
#define AFPDIR 64
#define DEV_BSIZE 512
#define NBBY 8
#define DIRBLKSIZ 512
#define UMASK 0755
#define FS_FLAGS_UPDATED 0x80
#define IFDIR 0040000
#define FS_OPTTIME 0
#define CGSIZEFUDGE 8

#define SB_old_cgoffset 24
#define SB_old_cgmask 28
#define SB_bmask 72
#define SB_fmask 76
#define SB_maxcontig 88
#define SB_old_flags 211
#define SB_maxbsize 860
#define SB_providersize 872
#define SB_sblockactualloc 992
#define SB_qbmask 1336
#define SB_qfmask 1344
#define SB_old_cpg 180
#define SB_old_postblformat 1356
#define SB_old_nrpos 1360
#define SB_old_inodefmt 1324
#define FS_DYNAMICPOSTBLFMT 1
#define FS_44INODEFMT 2
#define CG_old_ncyl 16
#define CG_old_btotoff 84
#define CG_old_boff 88

static uchar sb[SBLOCKSIZE];
static uchar *cg;
static uchar *csum;
static uchar *iobuf;
static int dev;
static int64_t mediasize;
static int Nflag;

static uint bsize = 0, fsize = 0, frag, fragshift, bshift, fshift;
static uint ncg, ipg, fpg, inopb, nindir, cgsize, sbsize, minfree = MINFREE;
static uint sblkno, cblkno, iblkno, dblkno, contigsumsize, maxcontig;
static uint64_t fssize, dsize, csaddr, cssize, metaspace;
static uint density;
static int64_t utime;

static uint32_t rd32(const uchar *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
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
static void add64(uchar *p, int64_t d) { wr64(p, rd64(p) + (uint64_t)d); }
static void setbit_(uchar *m, uint b) { m[b / NBBY] |= (uchar)(1 << (b % NBBY)); }
static int isset_(const uchar *m, uint b) { return (m[b / NBBY] >> (b % NBBY)) & 1; }

#define SB(f) (sb + UFS2_SB_##f)
#define CG(f) (cg + UFS2_CG_##f)
#define CS(c) (csum + (c) * 16)
#define roundup(a, b) (howmany(a, b) * (b))
#define POWEROF2(n) (((n) & ((n) - 1)) == 0)

static uint ilog2(uint v) {
  uint i;

  for (i = 0; i < 32; i++)
    if ((1u << i) == v)
      return i;
  dprintf(2, "newfs: %u is not a power of 2\n", v);
  exit(1);
}

static uint32_t newfs_random(void) {
  uint32_t r;

  if (getentropy(&r, sizeof(r)) < 0)
    r = (uint32_t)utime * 1103515245u + 12345u;
  return r;
}

#define cgbase(c) ((uint64_t)fpg * (c))
#define cgsblock(c) (cgbase(c) + sblkno)
#define cgtod(c) (cgbase(c) + cblkno)
#define cgimin(c) (cgbase(c) + iblkno)
#define cgdmin(c) (cgbase(c) + dblkno)
#define fragroundup(n) (roundup((uint64_t)(n), fsize))
#define numfrags(n) ((n) / fsize)
#define fragstoblks(f) ((f) / frag)
#define fragnum(f) ((f) % frag)
#define INOPF (inopb / frag)

static uint cgsize_of(void) {
  uint n = UFS2_CG_HEADER_SIZE + howmany(ipg, NBBY) + howmany(fpg, NBBY) + 4;

  if (contigsumsize > 0)
    n += contigsumsize * 4 + howmany(fragstoblks(fpg), NBBY);
  return n;
}

static void wtfs(uint64_t fragno, uint size, const void *buf) {
  if (Nflag)
    return;
  if (lseek(dev, (int64_t)fragno * fsize, SEEK_SET) < 0 ||
      write(dev, (void *)buf, (int)size) != (int)size) {
    warn("write at fragment %llu", fragno);
    exit(1);
  }
}

static void wtbytes(int64_t offset, uint size, const void *buf) {
  if (Nflag)
    return;
  if (lseek(dev, offset, SEEK_SET) < 0 ||
      write(dev, (void *)buf, (int)size) != (int)size) {
    warn("write at byte %lld", offset);
    exit(1);
  }
}

// the superblock, no summaries
static void sbwrite(int64_t loc) {
  wr64(SB(time), (uint64_t)utime);
  wr64(sb + SB_sblockactualloc, (uint64_t)loc);
  wtbytes(loc, sbsize, sb);
}

static void csumwrite(void) {
  wtfs(csaddr, (uint)cssize, csum);
}

static void setblock(uchar *map, uint blk) {
  uint i;

  for (i = 0; i < frag; i++)
    setbit_(map, blk * frag + i);
}

static void clrblock(uchar *map, uint blk) {
  uint i;

  for (i = 0; i < frag; i++)
    map[(blk * frag + i) / NBBY] &= (uchar)~(1 << ((blk * frag + i) % NBBY));
}

static int isblock(const uchar *map, uint blk) {
  uint i;

  for (i = 0; i < frag; i++)
    if (!isset_(map, blk * frag + i))
      return 0;
  return 1;
}

static void initcg(uint cylno) {
  uint64_t cbase = cgbase(cylno), dmax = cbase + fpg;
  uint dlower, dupper, d, i, blkno, ndblk, initediblk;
  uchar *inosused, *blksfree, *clustersfree;

  if (dmax > fssize)
    dmax = fssize;
  dlower = (uint)(cgsblock(cylno) - cbase);
  dupper = (uint)(cgdmin(cylno) - cbase);
  if (cylno == 0)
    dupper += (uint)howmany(cssize, fsize);
  memset(cg, 0, cgsize);
  wr64(CG(time), (uint64_t)utime);
  wr32(CG(old_time), (uint32_t)utime);
  wr32(CG(magic), UFS2_CG_MAGIC);
  wr32(CG(cgx), cylno);
  wr32(CG(niblk), ipg);
  initediblk = ipg < 2 * inopb ? ipg : 2 * inopb;
  wr32(CG(initediblk), initediblk);
  ndblk = (uint)(dmax - cbase);
  wr32(CG(ndblk), ndblk);
  if (contigsumsize > 0)
    wr32(CG(nclusterblks), ndblk / frag);
  wr32(CG(iusedoff), UFS2_CG_HEADER_SIZE);
  wr32(CG(freeoff), UFS2_CG_HEADER_SIZE + howmany(ipg, NBBY));
  wr32(CG(nextfreeoff), rd32(CG(freeoff)) + howmany(fpg, NBBY));
  if (contigsumsize > 0) {
    uint sumoff = roundup(rd32(CG(nextfreeoff)), 4) - 4;
    wr32(CG(clustersumoff), sumoff);
    wr32(CG(clusteroff), sumoff + (contigsumsize + 1) * 4);
    wr32(CG(nextfreeoff), rd32(CG(clusteroff)) + howmany(fragstoblks(fpg), NBBY));
  }
  if (rd32(CG(nextfreeoff)) > cgsize) {
    dprintf(2, "newfs: cylinder group too big by %u bytes\n",
            rd32(CG(nextfreeoff)) - cgsize);
    exit(1);
  }
  inosused = cg + rd32(CG(iusedoff));
  blksfree = cg + rd32(CG(freeoff));
  clustersfree = cg + rd32(CG(clusteroff));
  add32(CG(cs) + 8, (int)ipg);
  if (cylno == 0)
    for (i = 0; i < UFS2_ROOT_INO; i++) {
      setbit_(inosused, i);
      add32(CG(cs) + 8, -1);
    }
  if (cylno > 0) {
    for (d = 0; d < dlower; d += frag) {
      blkno = d / frag;
      setblock(blksfree, blkno);
      if (contigsumsize > 0)
        setbit_(clustersfree, blkno);
      add32(CG(cs) + 4, 1);
    }
  }
  if ((i = dupper % frag) != 0) {
    add32(CG(frsum) + (frag - i) * 4, 1);
    for (d = dupper + frag - i; dupper < d; dupper++) {
      setbit_(blksfree, dupper);
      add32(CG(cs) + 12, 1);
    }
  }
  for (d = dupper; d + frag <= ndblk; d += frag) {
    blkno = d / frag;
    setblock(blksfree, blkno);
    if (contigsumsize > 0)
      setbit_(clustersfree, blkno);
    add32(CG(cs) + 4, 1);
  }
  if (d < ndblk) {
    add32(CG(frsum) + (ndblk - d) * 4, 1);
    for (; d < ndblk; d++) {
      setbit_(blksfree, d);
      add32(CG(cs) + 12, 1);
    }
  }
  if (contigsumsize > 0) {
    uchar *sump = cg + rd32(CG(clustersumoff));
    uint run = 0, nclusterblks = rd32(CG(nclusterblks));

    for (i = 0; i < nclusterblks; i++) {
      if (isset_(clustersfree, i))
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
  memmove(CS(cylno), CG(cs), 16);
  sbwrite((int64_t)cgsblock(cylno) * fsize);
  wtfs(cgtod(cylno), cgsize, cg);
  memset(iobuf, 0, 2 * bsize);
  for (i = 0; i < initediblk; i++)
    wr32(iobuf + i * UFS2_DINODE_SIZE + UFS2_DINODE_GEN, newfs_random());
  wtfs(cgimin(cylno), 2 * bsize, iobuf);
}

static uint64_t alloc(uint size, uint mode) {
  uchar *blksfree, *clustersfree;
  uint d, ndblk, blkno, frags, i;

  if (lseek(dev, (int64_t)cgtod(0) * fsize, SEEK_SET) < 0 ||
      read(dev, cg, (int)cgsize) != (int)cgsize || rd32(CG(magic)) != UFS2_CG_MAGIC) {
    dprintf(2, "newfs: cannot read back cylinder group 0\n");
    exit(1);
  }
  blksfree = cg + rd32(CG(freeoff));
  clustersfree = cg + rd32(CG(clusteroff));
  ndblk = rd32(CG(ndblk));
  for (d = 0; d < ndblk; d += frag)
    if (isblock(blksfree, d / frag))
      break;
  if (d >= ndblk) {
    dprintf(2, "newfs: first cylinder group ran out of space\n");
    exit(1);
  }
  blkno = d / frag;
  clrblock(blksfree, blkno);
  if (contigsumsize > 0)
    clustersfree[blkno / NBBY] &= (uchar)~(1 << (blkno % NBBY));
  add32(CG(cs) + 4, -1);
  add64(SB(cstotal) + 8, -1);
  add32(CS(0) + 4, -1);
  if (mode & IFDIR) {
    add32(CG(cs) + 0, 1);
    add64(SB(cstotal) + 0, 1);
    add32(CS(0) + 0, 1);
  }
  if (size != bsize) {
    frags = howmany(size, fsize);
    add32(CS(0) + 12, (int)(frag - frags));
    add64(SB(cstotal) + 24, (int)(frag - frags));
    add32(CG(cs) + 12, (int)(frag - frags));
    add32(CG(frsum) + (frag - frags) * 4, 1);
    for (i = frags; i < frag; i++)
      setbit_(blksfree, d + i);
  }
  wtfs(cgtod(0), cgsize, cg);
  return d;
}

static void iput(const uchar *dinode, uint ino) {
  uint64_t fragno;
  uint offset;
  uchar *inosused;

  if (lseek(dev, (int64_t)cgtod(0) * fsize, SEEK_SET) < 0 ||
      read(dev, cg, (int)cgsize) != (int)cgsize || rd32(CG(magic)) != UFS2_CG_MAGIC) {
    dprintf(2, "newfs: cannot read back cylinder group 0\n");
    exit(1);
  }
  inosused = cg + rd32(CG(iusedoff));
  add32(CG(cs) + 8, -1);
  setbit_(inosused, ino);
  wtfs(cgtod(0), cgsize, cg);
  add64(SB(cstotal) + 16, -1);
  add32(CS(0) + 8, -1);
  fragno = cgimin(0) + (uint64_t)(ino / inopb) * frag;
  offset = (ino % inopb) * UFS2_DINODE_SIZE;
  if (lseek(dev, (int64_t)fragno * fsize, SEEK_SET) < 0 ||
      read(dev, iobuf, (int)bsize) != (int)bsize) {
    dprintf(2, "newfs: cannot read the inode block\n");
    exit(1);
  }
  memmove(iobuf + offset, dinode, UFS2_DINODE_SIZE);
  wtfs(fragno, bsize, iobuf);
}

// the root's own chunk
static uint makedir(uint ino) {
  memset(iobuf, 0, fsize > DIRBLKSIZ ? fsize : DIRBLKSIZ);
  wr32(iobuf, ino);
  wr16(iobuf + 4, 12);
  iobuf[6] = UFS2_DIRTYPE_DIR;
  iobuf[7] = 1;
  iobuf[8] = '.';
  wr32(iobuf + 12, ino);
  wr16(iobuf + 16, DIRBLKSIZ - 12);
  iobuf[18] = UFS2_DIRTYPE_DIR;
  iobuf[19] = 2;
  iobuf[20] = '.';
  iobuf[21] = '.';
  return DIRBLKSIZ;
}

static void fsinit(void) {
  uchar node[UFS2_DINODE_SIZE];
  uint64_t blk;

  memset(node, 0, sizeof(node));
  wr64(node + UFS2_DINODE_ATIME, (uint64_t)utime);
  wr64(node + UFS2_DINODE_MTIME, (uint64_t)utime);
  wr64(node + UFS2_DINODE_CTIME, (uint64_t)utime);
  wr64(node + UFS2_DINODE_BIRTHTIME, (uint64_t)utime);
  wr16(node + UFS2_DINODE_MODE, IFDIR | UMASK);
  wr16(node + UFS2_DINODE_NLINK, 2);
  wr64(node + UFS2_DINODE_SIZE_OFFSET, makedir(UFS2_ROOT_INO));
  blk = alloc(fsize, IFDIR);
  wr64(node + UFS2_DINODE_DIRECT_OFFSET, blk);
  wr64(node + UFS2_DINODE_BLOCKS, fragroundup(DIRBLKSIZ) / DEV_BSIZE);
  wr32(node + UFS2_DINODE_GEN, newfs_random());
  wtfs(blk, fsize, iobuf);
  iput(node, UFS2_ROOT_INO);
}

static void mkfs(const char *fsys, const char *label) {
  uint fragsperinode, minfragsperinode, minfpg, optimalfpg, lastminfpg, origdensity;
  uint64_t maxinum, sizepb, csfrags, maxfilesize;
  uint i, cylno;
  uchar fsr[DEV_BSIZE];
  struct fnu_timespec now;

  fnu_gettime(&now, FNU_CLOCK_WALL);
  utime = now.sec;
  memset(sb, 0, sizeof(sb));
  sb[SB_old_flags] = FS_FLAGS_UPDATED;
  wr32(sb + SB_old_cpg, 1);
  wr32(sb + SB_old_postblformat, FS_DYNAMICPOSTBLFMT);
  wr32(sb + SB_old_nrpos, 1);
  wr32(sb + SB_old_inodefmt, FS_44INODEFMT);
  if (label != 0)
    strncpy((char *)SB(volname), label, MAXVOLLEN - 1);
  wr32(SB(avgfilesize), AVFILESIZ);
  wr32(SB(avgfpdir), AFPDIR);

  if (!POWEROF2(bsize) || !POWEROF2(fsize)) {
    dprintf(2, "newfs: block and fragment sizes must be powers of 2\n");
    exit(1);
  }
  if (fsize < DEV_BSIZE)
    fsize = DEV_BSIZE;
  if (bsize > MAXBSIZE)
    bsize = MAXBSIZE;
  if (bsize < MINBSIZE)
    bsize = MINBSIZE;
  if (bsize < fsize)
    bsize = fsize;
  if (fsize * UFS2_MAXFRAG < bsize)
    fsize = bsize / UFS2_MAXFRAG;
  wr32(SB(bsize), bsize);
  wr32(SB(fsize), fsize);
  wr32(sb + SB_maxbsize, bsize);
  maxcontig = MAXPHYS / bsize > 1 ? MAXPHYS / bsize : 1;
  wr32(sb + SB_maxcontig, maxcontig);
  contigsumsize = maxcontig > 1 ? (maxcontig < FS_MAXCONTIG ? maxcontig : FS_MAXCONTIG) : 0;
  wr32(SB(contigsumsize), contigsumsize);
  wr32(sb + SB_bmask, ~(bsize - 1));
  wr32(sb + SB_fmask, ~(fsize - 1));
  wr64(sb + SB_qbmask, bsize - 1);
  wr64(sb + SB_qfmask, fsize - 1);
  bshift = ilog2(bsize);
  fshift = ilog2(fsize);
  wr32(SB(bshift), bshift);
  wr32(SB(fshift), fshift);
  frag = bsize / fsize;
  fragshift = ilog2(frag);
  wr32(SB(frag), frag);
  wr32(SB(fragshift), fragshift);
  wr32(SB(fsbtodb), ilog2(fsize / DEV_BSIZE));
  fssize = (uint64_t)mediasize / fsize;
  wr64(SB(size), fssize);
  wr64(sb + SB_providersize, fssize);
  wr64(SB(sblockloc), SBLOCK_UFS2);
  nindir = bsize / 8;
  inopb = bsize / UFS2_DINODE_SIZE;
  wr32(SB(nindir), nindir);
  wr32(SB(inopb), inopb);
  wr32(SB(maxsymlinklen), (UFS2_DIRECT_BLOCKS + UFS2_INDIRECT_BLOCKS) * 8);
  sblkno = roundup(howmany(SBLOCK_UFS2 + SBLOCKSIZE, fsize), frag);
  cblkno = sblkno + roundup(howmany(SBLOCKSIZE, fsize), frag);
  iblkno = cblkno + frag;
  wr32(SB(sblkno), sblkno);
  wr32(SB(cblkno), cblkno);
  wr32(SB(iblkno), iblkno);
  maxfilesize = (uint64_t)bsize * UFS2_DIRECT_BLOCKS - 1;
  for (sizepb = bsize, i = 0; i < UFS2_INDIRECT_BLOCKS; i++) {
    sizepb *= nindir;
    maxfilesize += sizepb;
  }
  wr64(SB(maxfilesize), maxfilesize);

  maxinum = ((uint64_t)1 << 32) - inopb;
  minfragsperinode = (uint)(1 + fssize / maxinum);
  if (density == 0)
    density = (NFPI > minfragsperinode ? NFPI : minfragsperinode) * fsize;
  else if (density < minfragsperinode * fsize)
    density = minfragsperinode * fsize;
  origdensity = density;
  for (;;) {
    fragsperinode = numfrags(density) > 1 ? numfrags(density) : 1;
    if (fragsperinode < minfragsperinode) {
      dprintf(2, "newfs: block size too small for a filesystem of this size\n");
      exit(1);
    }
    minfpg = fragsperinode * inopb;
    if (minfpg > fssize)
      minfpg = (uint)fssize;
    ipg = inopb;
    fpg = roundup(iblkno + ipg / INOPF, frag);
    if (fpg < minfpg)
      fpg = minfpg;
    ipg = roundup(howmany(fpg, fragsperinode), inopb);
    fpg = roundup(iblkno + ipg / INOPF, frag);
    if (fpg < minfpg)
      fpg = minfpg;
    ipg = roundup(howmany(fpg, fragsperinode), inopb);
    if (cgsize_of() < bsize - CGSIZEFUDGE)
      break;
    density -= fsize;
  }
  if (density != origdensity)
    printf("density reduced from %u to %u\n", origdensity, density);
  for (;; fpg += frag) {
    ipg = roundup(howmany(fpg, fragsperinode), inopb);
    if (fssize / fpg < MINCYLGRPS)
      break;
    if (cgsize_of() < bsize - CGSIZEFUDGE)
      continue;
    if (cgsize_of() == bsize - CGSIZEFUDGE)
      break;
    fpg -= frag;
    ipg = roundup(howmany(fpg, fragsperinode), inopb);
    break;
  }
  optimalfpg = fpg;
  for (;;) {
    ncg = (uint)howmany(fssize, fpg);
    lastminfpg = roundup(iblkno + ipg / INOPF, frag);
    if (fssize < lastminfpg) {
      dprintf(2, "newfs: filesystem size %llu < minimum size of %u\n", fssize,
              lastminfpg);
      exit(1);
    }
    if (fssize % fpg >= lastminfpg || fssize % fpg == 0)
      break;
    fpg -= frag;
    ipg = roundup(howmany(fpg, fragsperinode), inopb);
  }
  if (optimalfpg != fpg)
    printf("reduced frags per cylinder group from %u to %u to enlarge last cyl group\n",
           optimalfpg, fpg);
  wr32(SB(ncg), ncg);
  wr32(SB(ipg), ipg);
  wr32(SB(fpg), fpg);
  cgsize = (uint)fragroundup(cgsize_of());
  wr32(SB(cgsize), cgsize);
  dblkno = iblkno + ipg / INOPF;
  wr32(SB(dblkno), dblkno);
  csaddr = cgdmin(0);
  cssize = fragroundup((uint64_t)ncg * 16);
  wr64(SB(csaddr), csaddr);
  wr32(SB(cssize), (uint32_t)cssize);
  sbsize = (uint)fragroundup(1376);
  if (sbsize > SBLOCKSIZE)
    sbsize = SBLOCKSIZE;
  wr32(SB(sbsize), sbsize);
  wr32(SB(minfree), minfree);
  metaspace = ((uint64_t)fpg * minfree / 200) & ~(uint64_t)(frag - 1);
  wr64(SB(metaspace), metaspace);
  wr32(SB(maxbpg), bsize / 8);
  wr32(SB(optim), FS_OPTTIME);
  wr32(SB(cgrotor), 0);
  sb[UFS2_SB_fmod] = 0;
  sb[UFS2_SB_ronly] = 0;
  sb[UFS2_SB_clean] = 1;
  wr32(SB(state), 0);
  wr32(SB(id), (uint32_t)utime);
  wr32(SB(id) + 4, newfs_random());
  csfrags = howmany(cssize, fsize);
  dsize = fssize - sblkno - (uint64_t)ncg * (dblkno - sblkno);
  wr64(SB(cstotal) + 8, fragstoblks(dsize) - howmany(csfrags, frag));
  wr64(SB(cstotal) + 24, fragnum(fssize) + (fragnum(csfrags) > 0 ? frag - fragnum(csfrags) : 0));
  wr64(SB(cstotal) + 16, (uint64_t)ncg * ipg - UFS2_ROOT_INO);
  wr64(SB(cstotal) + 0, 0);
  dsize -= csfrags;
  wr64(SB(dsize), dsize);
  wr32(SB(flags), 0);
  wr32(SB(metackhash), 0);

  printf("%s: %llu.%lluMB (%llu sectors) block size %u, fragment size %u\n", fsys,
         fssize * fsize / (1024 * 1024), (fssize * fsize % (1024 * 1024)) * 10 / (1024 * 1024),
         fssize * fsize / DEV_BSIZE, bsize, fsize);
  printf("\tusing %u cylinder groups of %llu.%02lluMB, %u blks, %u inodes.\n", ncg,
         (uint64_t)fpg * fsize / (1024 * 1024),
         ((uint64_t)fpg * fsize % (1024 * 1024)) * 100 / (1024 * 1024), fpg / frag, ipg);

  if ((cg = malloc(cgsize)) == 0 || (csum = malloc((size_t)cssize)) == 0 ||
      (iobuf = malloc(2 * bsize)) == 0) {
    dprintf(2, "newfs: out of memory\n");
    exit(1);
  }
  memset(csum, 0, (size_t)cssize);
  wr32(SB(magic), 0x19960408);
  sbwrite(SBLOCK_UFS2);
  wr32(SB(magic), UFS2_MAGIC);
  printf("super-block backups (for fsck -b #) at:");
  for (cylno = 0; cylno < ncg; cylno++) {
    initcg(cylno);
    printf(" %llu%s", cgsblock(cylno) * (fsize / DEV_BSIZE), cylno + 1 < ncg ? "," : "\n");
  }
  if (Nflag)
    return;
  fsinit();
  csumwrite();
  sbwrite(SBLOCK_UFS2);
  memset(fsr, 0, sizeof(fsr));
  wr32(fsr + DEV_BSIZE - 20, UFS2_MAGIC);
  wr32(fsr + DEV_BSIZE - 16, ilog2(fsize / DEV_BSIZE));
  wr32(fsr + DEV_BSIZE - 12, sblkno);
  wr32(fsr + DEV_BSIZE - 8, fpg);
  wr32(fsr + DEV_BSIZE - 4, ncg);
  wtbytes(SBLOCK_UFS2 - DEV_BSIZE, DEV_BSIZE, fsr);
}

static void usage(void) {
  dprintf(2, "usage: newfs [-N] [-b block-size] [-f frag-size] [-i bytes-per-inode]\n"
             "             [-L volume-name] [-m free-space] special\n");
  exit(1);
}

int main(int argc, char **argv) {
  const char *label = 0, *special = 0;
  uint64_t bytes = 0;
  int i;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-N") == 0)
      Nflag = 1;
    else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
      bsize = (uint)strtoll(argv[++i], 0, 0);
    else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
      fsize = (uint)strtoll(argv[++i], 0, 0);
    else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
      density = (uint)strtoll(argv[++i], 0, 0);
    else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
      minfree = (uint)strtoll(argv[++i], 0, 0);
    else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc)
      label = argv[++i];
    else if (argv[i][0] == '-' || special != 0)
      usage();
    else
      special = argv[i];
  }
  if (special == 0 || minfree > 99)
    usage();
  if (fsize == 0)
    fsize = DFL_FRAGSIZE;
  if (bsize == 0)
    bsize = DFL_BLKSIZE < 8 * fsize ? DFL_BLKSIZE : 8 * fsize;
  if ((dev = open(special, Nflag ? O_RDONLY : O_RDWR)) < 0) {
    warn("%s", special);
    exit(1);
  }
  if (ioctl(dev, FNU_DISKSIZE, (uint64_t)(uintptr_t)&bytes) < 0 || bytes == 0) {
    dprintf(2, "newfs: %s is not a block device\n", special);
    exit(1);
  }
  mediasize = (int64_t)bytes;
  mkfs(special, label);
  if (!Nflag && fsync(dev) < 0)
    warn("%s", special);
  close(dev);
  exit(0);
}
