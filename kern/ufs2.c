// UFS2
#include "defs.h"
#include "malloc.h"
#include "mutex.h"
#include "sleeplock.h"
#include "proc.h"
#include "ufs2.h"
#include "ufs2log.h"
#include "ulogfmt.h"
#include "inc/stat.h"
#include "buf.h"
#include "uio.h"

// may wait, may fail
#define UFS_MEM (M_WAITOK | M_CANFAIL)

#define NUFS2 8
#define NBBY 8
#define DEV_BSIZE 512
#define DIRBLKSIZ UFS2_DIR_BLOCK_SIZE
#define NDADDR UFS2_DIRECT_BLOCKS
#define NIADDR UFS2_INDIRECT_BLOCKS
#define MAXBSIZE 65536
#define FS_OPTTIME 0
#define FS_OPTSPACE 1
#define MAXMNTLEN 468
#define SBLOCK_UFS2 65536

_Static_assert(NUFS2 <= NFSTX, "a thread counts its transactions per mount");

/* struct csum inside cg_cs and the summary area: four int32 */
#define CS_NDIR 0
#define CS_NBFREE 4
#define CS_NIFREE 8
#define CS_NFFREE 12
#define CS_SIZE 16
/* struct csum_total inside the superblock: five int64 */
#define CST_NDIR 0
#define CST_NBFREE 8
#define CST_NIFREE 16
#define CST_NFFREE 24
#define CST_NUMCLUSTERS 32

struct ufs2_mount {
  int used;
  uint id;
  struct sleeplock lock;
  int rw;
  int fmod;
  int64_t mounted_time; // fs_time as found
  uint dev;
  struct blockdev device;
  uchar *sb;   // fs_sbsize bytes
  uchar *cg;
  uchar *csum;
  uchar *contigdirs;
  int cgx;
  int cg_dirty;

  uint bsize, fsize, frag, fragshift, bshift, fshift;
  uint ncg, ipg, fpg, inopb, nindir, cgsize, sbsize;
  uint sblkno, cblkno, iblkno, dblkno;
  uint minfree, contigsumsize, maxsymlinklen, maxbpg;
  uint64_t size, dsize, csaddr, cssize, metaspace, maxfilesize;

  struct ulog *log;
  uint log_ino;
  struct mutex txlock;
  int active;
  int want;
  volatile int txdirty;
  uchar **pending;
  uint npending;           // groups with some
  uint32_t *orph;
  uint norph, orphcap;
  struct ulog_overlay *overlay;
};

#define FILEOWNER(m, ino) ((uint64_t)(m)->id << 32 | (ino))

static struct ufs2_mount mounts[NUFS2];
static struct sleeplock ufs2_lock;

static uint32_t rd32(const uchar *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uchar *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
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
static void add32(uchar *p, int delta) { wr32(p, rd32(p) + (uint32_t)delta); }
static void add64(uchar *p, int64_t delta) {
  wr64(p, rd64(p) + (uint64_t)delta);
}

#define SB(m, f) ((m)->sb + UFS2_SB_##f)
#define CG(m, f) ((m)->cg + UFS2_CG_##f)
#define CS(m, cgx) ((m)->csum + (uint64_t)(cgx) * CS_SIZE)

/* fs.h: cylinder group geometry, in fragments */
#define cgbase(m, c) ((uint64_t)(m)->fpg * (c))
#define cgtod(m, c) (cgbase(m, c) + (m)->cblkno)
#define cgimin(m, c) (cgbase(m, c) + (m)->iblkno)
#define cgdmin(m, c) (cgbase(m, c) + (m)->dblkno)
#define cgmeta(m, c) cgdmin(m, c)
#define cgdata(m, c) (cgdmin(m, c) + (m)->metaspace)
#define dtog(m, d) ((uint)((d) / (m)->fpg))
#define dtogd(m, d) ((uint)((d) % (m)->fpg))
#define ino_to_cg(m, i) ((i) / (m)->ipg)
#define numfrags(m, bytes) ((uint)((bytes) >> (m)->fshift))
#define fragroundup(m, bytes) (((bytes) + (m)->fsize - 1) & ~(uint64_t)((m)->fsize - 1))
#define blkoff(m, off) ((uint)((off) & ((m)->bsize - 1)))
#define lblkno(m, off) ((off) >> (m)->bshift)
#define fragnum(m, fsb) ((uint)((fsb) & ((m)->frag - 1)))
#define blknum(m, fsb) ((fsb) & ~(uint64_t)((m)->frag - 1))
#define fragstoblks(m, f) ((f) >> (m)->fragshift)
#define btodb(bytes) ((bytes) / DEV_BSIZE)

static int isset_bit(const uchar *map, uint bit) {
  return (map[bit / NBBY] >> (bit % NBBY)) & 1;
}
static void set_bit(uchar *map, uint bit) { map[bit / NBBY] |= (uchar)(1 << (bit % NBBY)); }
static void clr_bit(uchar *map, uint bit) { map[bit / NBBY] &= (uchar)~(1 << (bit % NBBY)); }

static int power_of_two(uint value) {
  return value != 0 && (value & (value - 1)) == 0;
}

static int tx_full(struct ufs2_mount *m);
static int tx_restart(struct ufs2_mount *m);
static void orphan_mark(struct ufs2_mount *m, uint ino, int on);

static struct ufs2_mount *mount_of(const struct ufs2_volume *volume) {
  struct ufs2_mount *m;

  if (volume == 0 || (m = volume->m) == 0 || !m->used ||
      m->dev != volume->device.device)
    return 0;
  return m;
}

static void meta_pin(struct ufs2_mount *m, struct buf *bp) {
  if (m->log == 0)
    return;
  bpin(bp, ulog_pins(m->log));
  m->txdirty = 1;
}

// THE VOLUME'S OWN BYTES
static int block_move(struct ufs2_mount *m, uint64_t at, void *buf, uint length,
                      int write, int age) {
  uchar *p = buf;
  uint unit = DEV_BSIZE;

  if (m->bsize == 0 || (m->bsize & (DEV_BSIZE - 1)) != 0)
    return -1;
  while (length > 0) {
    uint64_t blk = at / unit;
    uint off = (uint)(at % unit);
    uint n = unit - off;
    uint64_t lba = blk * (unit / DEV_BSIZE);
    struct buf *bp;

    if (n > length)
      n = length;
    if (write && n == unit)
      bp = bgetblk_size(m->device.device, m->device.first_lba + lba, unit);
    else
      bp = bread_size(m->device.device, m->device.first_lba + lba, unit);
    if (bp->flags & B_ERROR) {
      brelse(bp);
      return -1;
    }
    if (write) {
      memmove(bp->data + off, p, n);
      if (age && n == unit)
        bp->flags |= B_AGE;
      meta_pin(m, bp);
      bdwrite(bp);
    } else {
      memmove(p, bp->data + off, n);
      if (age && n == unit)
        bp->flags |= B_AGE;
      brelse(bp);
    }
    p += n;
    at += n;
    length -= n;
  }
  return 0;
}

// A FILE'S OWN BLOCK
static struct buf *data_buf(struct ufs2_mount *m, uint ino, uint64_t lbn,
                            uint64_t fragno, uint size, int whole) {
  uint64_t lba = m->device.first_lba + fragno * (m->fsize / DEV_BSIZE);
  uint64_t owner = FILEOWNER(m, ino);
  struct buf *bp;

  if (size == 0 || (size & (DEV_BSIZE - 1)) != 0 || fragno >= m->size)
    return 0;
  if (whole)
    bp = bgetblk_file(m->device.device, owner, lbn, lba, size);
  else
    bp = bread_file(m->device.device, owner, lbn, lba, size);
  if (bp->flags & B_ERROR) {
    brelse(bp);
    return 0;
  }
  return bp;
}

static int frag_read(struct ufs2_mount *m, uint64_t fragno, uint offset,
                     void *dst, uint length) {
  return block_move(m, fragno * m->fsize + offset, dst, length, 0, 0);
}
static int frag_write(struct ufs2_mount *m, uint64_t fragno, uint offset,
                      const void *src, uint length) {
  return block_move(m, fragno * m->fsize + offset, (void *)src, length, 1, 0);
}
static int frag_zero(struct ufs2_mount *m, uint64_t fragno, uint offset,
                     uint length) {
  static const uchar zeros[DEV_BSIZE];

  while (length > 0) {
    uint n = length < DEV_BSIZE ? length : DEV_BSIZE;
    if (frag_write(m, fragno, offset, zeros, n) < 0)
      return -1;
    offset += n;
    length -= n;
  }
  return 0;
}

static int cg_flush(struct ufs2_mount *m) {
  struct fnu_timespec now;

  if (m->cgx < 0 || !m->cg_dirty)
    return 0;
  time_now(&now);
  wr32(CG(m, old_time), (uint32_t)now.sec);
  wr64(CG(m, time), (uint64_t)now.sec);
  if (frag_write(m, cgtod(m, (uint)m->cgx), 0, m->cg, m->cgsize) < 0)
    return -1;
  m->cg_dirty = 0;
  return 0;
}

static int cg_load(struct ufs2_mount *m, uint cgx) {
  uint iusedoff, freeoff, nextfreeoff, clusteroff, clustersumoff;

  if (m->cgx == (int)cgx)
    return 0;
  if (cg_flush(m) < 0)
    return -1;
  m->cgx = -1;
  if (cgx >= m->ncg ||
      frag_read(m, cgtod(m, cgx), 0, m->cg, m->cgsize) < 0)
    return -1;
  iusedoff = rd32(CG(m, iusedoff));
  freeoff = rd32(CG(m, freeoff));
  nextfreeoff = rd32(CG(m, nextfreeoff));
  clusteroff = rd32(CG(m, clusteroff));
  clustersumoff = rd32(CG(m, clustersumoff));
  if (rd32(CG(m, magic)) != UFS2_CG_MAGIC || rd32(CG(m, cgx)) != cgx ||
      rd32(CG(m, niblk)) != m->ipg || rd32(CG(m, initediblk)) > m->ipg ||
      rd32(CG(m, ndblk)) > m->fpg || iusedoff < UFS2_CG_HEADER_SIZE ||
      iusedoff + (m->ipg + NBBY - 1) / NBBY > freeoff ||
      freeoff + (m->fpg + NBBY - 1) / NBBY > nextfreeoff ||
      nextfreeoff > m->cgsize ||
      (m->contigsumsize > 0 &&
       (clustersumoff < freeoff || clustersumoff + (m->contigsumsize + 1) * 4 > clusteroff ||
        clusteroff + (m->fpg / m->frag + NBBY - 1) / NBBY > nextfreeoff))) {
    printf("UFS2: cylinder group %d is damaged\n", cgx);
    return -1;
  }
  m->cgx = (int)cgx;
  m->cg_dirty = 0;
  return 0;
}

#define cg_inosused(m) ((m)->cg + rd32(CG(m, iusedoff)))
#define cg_blksfree(m) ((m)->cg + rd32(CG(m, freeoff)))
#define cg_clustersfree(m) ((m)->cg + rd32(CG(m, clusteroff)))
#define cg_clustersum(m) ((m)->cg + rd32(CG(m, clustersumoff)))
#define cg_frsum(m, i) (CG(m, frsum) + (i) * 4)

static void sb_touch(struct ufs2_mount *m) {
  m->fmod = 1;
  m->sb[UFS2_SB_fmod] = 1;
  if (m->log != 0)
    m->txdirty = 1;
}

/* whole-block tests, any fs_frag */
static int isblock(struct ufs2_mount *m, const uchar *map, uint64_t blk) {
  uint first = (uint)(blk << m->fragshift), i;

  for (i = 0; i < m->frag; i++)
    if (!isset_bit(map, first + i))
      return 0;
  return 1;
}
static int isfreeblock(struct ufs2_mount *m, const uchar *map, uint64_t blk) {
  uint first = (uint)(blk << m->fragshift), i;

  for (i = 0; i < m->frag; i++)
    if (isset_bit(map, first + i))
      return 0;
  return 1;
}
static void setblock(struct ufs2_mount *m, uchar *map, uint64_t blk) {
  uint first = (uint)(blk << m->fragshift), i;

  for (i = 0; i < m->frag; i++)
    set_bit(map, first + i);
}
static void clrblock(struct ufs2_mount *m, uchar *map, uint64_t blk) {
  uint first = (uint)(blk << m->fragshift), i;

  for (i = 0; i < m->frag; i++)
    clr_bit(map, first + i);
}

static uint blkmap(struct ufs2_mount *m, const uchar *map, uint loc) {
  uint bits = 0, i;

  for (i = 0; i < m->frag; i++)
    if (isset_bit(map, loc + i))
      bits |= 1u << i;
  return bits;
}

static void fragacct(struct ufs2_mount *m, uint fragmap, int cnt) {
  uint pos = 0;

  if (fragmap == (1u << m->frag) - 1)
    return;
  while (pos < m->frag) {
    uint run = 0;
    while (pos < m->frag && (fragmap & (1u << pos))) {
      run++;
      pos++;
    }
    if (run > 0)
      add32(cg_frsum(m, run), cnt);
    pos++;
  }
}

static void clusteracct(struct ufs2_mount *m, uint64_t blkno, int cnt) {
  uchar *freemap, *sump;
  uint nblocks, start, end, i, forw, back;

  if (m->contigsumsize == 0)
    return;
  freemap = cg_clustersfree(m);
  sump = cg_clustersum(m);
  nblocks = rd32(CG(m, nclusterblks));
  if (cnt > 0)
    set_bit(freemap, (uint)blkno);
  else
    clr_bit(freemap, (uint)blkno);
  start = (uint)blkno + 1;
  end = start + m->contigsumsize;
  if (end > nblocks)
    end = nblocks;
  for (i = start; i < end; i++)
    if (!isset_bit(freemap, i))
      break;
  forw = i - start;
  back = 0;
  for (i = (uint)blkno; i > 0 && back < m->contigsumsize; i--) {
    if (!isset_bit(freemap, i - 1))
      break;
    back++;
  }
  i = back + forw + 1;
  if (i > m->contigsumsize)
    i = m->contigsumsize;
  add32(sump + i * 4, cnt);
  if (back > 0)
    add32(sump + back * 4, -cnt);
  if (forw > 0)
    add32(sump + forw * 4, -cnt);
}

static int mapsearch(struct ufs2_mount *m, uint64_t bpref, uint allocsiz,
                     uint *out) {
  const uchar *map = cg_blksfree(m);
  uint nblk = m->fpg / m->frag, first, b, k;

  first = bpref ? dtogd(m, bpref) / m->frag : rd32(CG(m, frotor)) / m->frag;
  for (k = 0; k < nblk; k++) {
    uint base, pos;
    b = (first + k) % nblk;
    base = b * m->frag;
    for (pos = 0; pos < m->frag;) {
      uint run = 0;
      while (pos + run < m->frag && isset_bit(map, base + pos + run))
        run++;
      if (run == allocsiz) {
        wr32(CG(m, frotor), base);
        *out = base + pos;
        return 0;
      }
      pos += run + 1;
    }
  }
  printf("UFS2: group %d: frsum says a run of %d exists and the map has none\n",
          m->cgx, allocsiz);
  return -1;
}

static uint64_t alloccgblk(struct ufs2_mount *m, uint64_t bpref, uint size) {
  uchar *map = cg_blksfree(m);
  uint cgx = (uint)m->cgx, bno, frags, i;
  uint64_t blkno;

  if (bpref == 0) {
    bpref = cgbase(m, cgx) + rd32(CG(m, rotor)) + m->frag;
  } else if (dtog(m, bpref) != cgx) {
    if (bpref < cgdata(m, dtog(m, bpref)))
      bpref = cgmeta(m, cgx);
    else
      bpref = cgdata(m, cgx);
  }
  bno = dtogd(m, blknum(m, bpref));
  if (bno < m->fpg && isblock(m, map, fragstoblks(m, bno)))
    goto gotit;
  if (mapsearch(m, bpref, m->frag, &bno) < 0)
    return 0;
  if (bno >= dtogd(m, cgdata(m, cgx)))
    wr32(CG(m, rotor), bno);
gotit:
  blkno = fragstoblks(m, bno);
  clrblock(m, map, blkno);
  clusteracct(m, blkno, -1);
  add32(CG(m, cs) + CS_NBFREE, -1);
  add64(SB(m, cstotal) + CST_NBFREE, -1);
  add32(CS(m, cgx) + CS_NBFREE, -1);
  sb_touch(m);
  blkno = cgbase(m, cgx) + bno;
  frags = numfrags(m, size);
  if (frags != m->frag) {
    for (i = frags; i < m->frag; i++)
      set_bit(map, bno + i);
    i = m->frag - frags;
    add32(CG(m, cs) + CS_NFFREE, (int)i);
    add64(SB(m, cstotal) + CST_NFFREE, (int)i);
    add32(CS(m, cgx) + CS_NFFREE, (int)i);
    add32(cg_frsum(m, i), 1);
  }
  m->cg_dirty = 1;
  return blkno;
}

static uint64_t alloccg(struct ufs2_mount *m, uint cgx, uint64_t bpref,
                        uint size, uint rsize) {
  uchar *map;
  uint frags, allocsiz, bno, i;
  uint64_t blkno;

  if (rd32(CS(m, cgx) + CS_NBFREE) == 0 && size == m->bsize)
    return 0;
  if (cg_load(m, cgx) < 0)
    return 0;
  if (rd32(CG(m, cs) + CS_NBFREE) == 0 && size == m->bsize)
    return 0;
  if (size == m->bsize)
    return alloccgblk(m, bpref, rsize);
  map = cg_blksfree(m);
  frags = numfrags(m, size);
  for (allocsiz = frags; allocsiz < m->frag; allocsiz++)
    if (rd32(cg_frsum(m, allocsiz)) != 0)
      break;
  if (allocsiz == m->frag) {
    if (rd32(CG(m, cs) + CS_NBFREE) == 0)
      return 0;
    return alloccgblk(m, bpref, rsize);
  }
  if (mapsearch(m, bpref, allocsiz, &bno) < 0)
    return 0;
  for (i = 0; i < frags; i++)
    clr_bit(map, bno + i);
  add32(CG(m, cs) + CS_NFFREE, -(int)frags);
  add32(cg_frsum(m, allocsiz), -1);
  if (frags != allocsiz)
    add32(cg_frsum(m, allocsiz - frags), 1);
  add64(SB(m, cstotal) + CST_NFFREE, -(int)frags);
  add32(CS(m, cgx) + CS_NFFREE, -(int)frags);
  sb_touch(m);
  m->cg_dirty = 1;
  blkno = cgbase(m, cgx) + bno;
  return blkno;
}

static uint64_t hashalloc(struct ufs2_mount *m, uint cgx, uint64_t pref,
                          uint size, uint rsize) {
  uint64_t result;
  uint i, icg = cgx;

  if ((result = alloccg(m, cgx, pref, size, rsize)) != 0)
    return result;
  for (i = 1; i < m->ncg; i *= 2) {
    cgx += i;
    if (cgx >= m->ncg)
      cgx -= m->ncg;
    if ((result = alloccg(m, cgx, 0, size, rsize)) != 0)
      return result;
  }
  cgx = (icg + 2) % m->ncg;
  for (i = 2; i < m->ncg; i++) {
    if ((result = alloccg(m, cgx, 0, size, rsize)) != 0)
      return result;
    if (++cgx == m->ncg)
      cgx = 0;
  }
  return 0;
}

static int64_t freespace(struct ufs2_mount *m) {
  return (int64_t)(rd64(SB(m, cstotal) + CST_NBFREE) << m->fragshift) +
         (int64_t)rd64(SB(m, cstotal) + CST_NFFREE) -
         (int64_t)m->dsize * m->minfree / 100;
}

static int privileged(void) {
  struct proc *p = curproc;

  return p == 0 || p->p_p->uid == 0;
}

static uint64_t blkalloc(struct ufs2_mount *m, uint ino, uint64_t bpref,
                         uint size) {
  uint cgx;

  if (size == m->bsize && rd64(SB(m, cstotal) + CST_NBFREE) == 0)
    return 0;
  if (!privileged() && freespace(m) - (int64_t)numfrags(m, size) < 0)
    return 0;
  if (bpref >= m->size)
    bpref = 0;
  cgx = bpref == 0 ? ino_to_cg(m, ino) : dtog(m, bpref);
  return hashalloc(m, cgx, bpref, size, size);
}

static int freeable(struct ufs2_mount *m, uint64_t bno, uint size) {
  if (bno >= m->size || size == 0 || size > m->bsize ||
      (size & (m->fsize - 1)) != 0 ||
      fragnum(m, bno) + numfrags(m, size) > m->frag) {
    printf("UFS2: bad block %ld freed\n", bno);
    return 0;
  }
  return 1;
}

static void needs_fsck(struct ufs2_mount *m) {
  wr32(SB(m, flags), rd32(SB(m, flags)) | UFS2_FS_NEEDSFSCK);
  sb_touch(m);
}

static int cg_blkfree(struct ufs2_mount *m, uint64_t bno, uint size) {
  uchar *map;
  uint cgx = dtog(m, bno), cgbno, frags, bbase, i;
  uint64_t fragno;

  if (!freeable(m, bno, size))
    return -1;
  if (cg_load(m, cgx) < 0)
    return -1;
  cgbno = dtogd(m, bno);
  map = cg_blksfree(m);
  if (size == m->bsize) {
    fragno = fragstoblks(m, cgbno);
    if (!isfreeblock(m, map, fragno)) {
      printf("UFS2: block %ld freed while free\n", bno);
      needs_fsck(m);
      return -1;
    }
    setblock(m, map, fragno);
    clusteracct(m, fragno, 1);
    add32(CG(m, cs) + CS_NBFREE, 1);
    add64(SB(m, cstotal) + CST_NBFREE, 1);
    add32(CS(m, cgx) + CS_NBFREE, 1);
  } else {
    bbase = cgbno - fragnum(m, cgbno);
    fragacct(m, blkmap(m, map, bbase), -1);
    frags = numfrags(m, size);
    for (i = 0; i < frags; i++) {
      if (isset_bit(map, cgbno + i)) {
        printf("UFS2: fragment %ld freed while free\n", bno + i);
        needs_fsck(m);
        return -1;
      }
      set_bit(map, cgbno + i);
    }
    add32(CG(m, cs) + CS_NFFREE, (int)frags);
    add64(SB(m, cstotal) + CST_NFFREE, (int)frags);
    add32(CS(m, cgx) + CS_NFFREE, (int)frags);
    fragacct(m, blkmap(m, map, bbase), 1);
    fragno = fragstoblks(m, bbase);
    if (isblock(m, map, fragno)) {
      add32(CG(m, cs) + CS_NFFREE, -(int)m->frag);
      add64(SB(m, cstotal) + CST_NFFREE, -(int)m->frag);
      add32(CS(m, cgx) + CS_NFFREE, -(int)m->frag);
      clusteracct(m, fragno, 1);
      add32(CG(m, cs) + CS_NBFREE, 1);
      add64(SB(m, cstotal) + CST_NBFREE, 1);
      add32(CS(m, cgx) + CS_NBFREE, 1);
    }
  }
  sb_touch(m);
  m->cg_dirty = 1;
  return 0;
}

// FREES HELD BACK
static int blkfree(struct ufs2_mount *m, uint64_t bno, uint size, int revoke) {
  uint cgx, first, n, i;
  uchar *map;

  if (m->log == 0)
    return cg_blkfree(m, bno, size);
  if (!freeable(m, bno, size))
    return -1;
  cgx = dtog(m, bno);
  first = dtogd(m, bno);
  n = numfrags(m, size);
  if ((map = m->pending[cgx]) == 0) {
    map = m->pending[cgx] =
        malloc((m->fpg + NBBY - 1) / NBBY, M_UFS, M_WAITOK | M_ZERO);
    m->npending++;
  }
  for (i = 0; i < n; i++) {
    if (isset_bit(map, first + i)) {
      printf("UFS2: fragment %ld freed twice\n", bno + i);
      needs_fsck(m);
      return -1;
    }
    set_bit(map, first + i);
  }
  if (revoke)
    ulog_revoke(m->log, bno * (m->fsize / DEV_BSIZE), size);
  m->txdirty = 1;
  return 0;
}

static void frees_apply(struct ufs2_mount *m) {
  uint cgx, blk, i;

  for (cgx = 0; m->npending > 0 && cgx < m->ncg; cgx++) {
    uchar *map = m->pending[cgx];

    if (map == 0)
      continue;
    for (blk = 0; blk < m->fpg / m->frag; blk++) {
      uint base = blk * m->frag, run = 0;

      if (isblock(m, map, blk)) {
        cg_blkfree(m, cgbase(m, cgx) + base, m->bsize);
        continue;
      }
      for (i = 0; i <= m->frag; i++) {
        if (i < m->frag && isset_bit(map, base + i)) {
          run++;
          continue;
        }
        if (run > 0)
          cg_blkfree(m, cgbase(m, cgx) + base + i - run, run * m->fsize);
        run = 0;
      }
    }
    free(map, M_UFS);
    m->pending[cgx] = 0;
    m->npending--;
  }
}

static uint64_t fragextend(struct ufs2_mount *m, uint64_t bprev, uint osize,
                           uint nsize) {
  uchar *map;
  uint cgx = dtog(m, bprev), frags, bbase, bno, i, nffree;

  if (rd32(CS(m, cgx) + CS_NFFREE) < numfrags(m, nsize - osize))
    return 0;
  frags = numfrags(m, nsize);
  bbase = fragnum(m, bprev);
  if (bbase > fragnum(m, bprev + frags - 1))
    return 0;
  if (cg_load(m, cgx) < 0)
    return 0;
  bno = dtogd(m, bprev);
  map = cg_blksfree(m);
  for (i = numfrags(m, osize); i < frags; i++)
    if (!isset_bit(map, bno + i))
      return 0;
  for (i = frags; i < m->frag - bbase; i++)
    if (!isset_bit(map, bno + i))
      break;
  add32(cg_frsum(m, i - numfrags(m, osize)), -1);
  if (i != frags)
    add32(cg_frsum(m, i - frags), 1);
  for (i = numfrags(m, osize), nffree = 0; i < frags; i++) {
    clr_bit(map, bno + i);
    nffree++;
  }
  add32(CG(m, cs) + CS_NFFREE, -(int)nffree);
  add64(SB(m, cstotal) + CST_NFFREE, -(int)nffree);
  add32(CS(m, cgx) + CS_NFFREE, -(int)nffree);
  sb_touch(m);
  m->cg_dirty = 1;
  return bprev;
}

static int data_regrow(struct ufs2_mount *m, uint ino, uint64_t lbn,
                       uint64_t oldfrag, uint64_t newfrag, uint osize,
                       uint nsize, int meta) {
  struct buf *bp = data_buf(m, ino, lbn, oldfrag, osize, 0);

  if (bp == 0)
    return -1;
  if (bio_regrow(bp, m->device.first_lba + newfrag * (m->fsize / DEV_BSIZE),
                 nsize) < 0) {
    brelse(bp);
    return -1;
  }
  memset(bp->data + osize, 0, nsize - osize);
  if (meta)
    meta_pin(m, bp);
  bdwrite(bp);
  return 0;
}

static uint64_t realloccg(struct ufs2_mount *m, uint ino, uint64_t lbn,
                          uint64_t bprev, uint64_t bpref, uint osize,
                          uint nsize, int meta) {
  uint64_t bno;
  uint request, optim;

  if (!privileged() &&
      freespace(m) - (int64_t)numfrags(m, nsize - osize) < 0)
    return 0;
  if ((bno = fragextend(m, bprev, osize, nsize)) != 0) {
    if (data_regrow(m, ino, lbn, bprev, bno, osize, nsize, meta) < 0) {
      blkfree(m, bno + numfrags(m, osize), nsize - osize, 0);
      return 0;
    }
    return bno;
  }
  if (bpref >= m->size)
    bpref = 0;
  optim = rd32(SB(m, optim));
  if (optim == FS_OPTSPACE) {
    request = nsize;
    if (m->minfree > 5 &&
        rd64(SB(m, cstotal) + CST_NFFREE) <= m->dsize * m->minfree / 200) {
      wr32(SB(m, optim), FS_OPTTIME);
      sb_touch(m);
    }
  } else {
    request = m->bsize;
    if (m->minfree >= 2 &&
        rd64(SB(m, cstotal) + CST_NFFREE) >= m->dsize * (m->minfree - 2) / 100) {
      wr32(SB(m, optim), FS_OPTSPACE);
      sb_touch(m);
    }
  }
  bno = hashalloc(m, dtog(m, bprev), bpref, request, nsize);
  if (bno == 0)
    return 0;
  if (data_regrow(m, ino, lbn, bprev, bno, osize, nsize, meta) < 0) {
    blkfree(m, bno, nsize, 0);
    return 0;
  }
  blkfree(m, bprev, osize, meta);
  return bno;
}

static uint64_t blkpref(struct ufs2_mount *m, uint ino, uint16_t mode,
                        const struct ufs2_inode *ip, uint64_t lbn, int indx,
                        uint64_t prevbn) {
  uint inocg = ino_to_cg(m, ino), cg, startcg;
  uint64_t pref, avgbfree;

  if (indx < 0) {
    pref = cgmeta(m, inocg);
    if (indx == -1 && lbn < NDADDR + m->nindir && ip->direct[NDADDR - 1] != 0) {
      pref = ip->direct[NDADDR - 1] + m->frag;
      if (dtog(m, pref) >= m->ncg)
        pref = 0;
    }
    return pref;
  }
  if (lbn == NDADDR) {
    pref = ip->indirect[0];
    if (pref != 0 && pref >= cgdata(m, inocg) && pref < cgbase(m, inocg + 1)) {
      if (dtog(m, pref + m->frag) >= m->ncg)
        return 0;
      return pref + m->frag;
    }
  }
  if (indx == 0 || prevbn >= m->size)
    prevbn = 0;
  if (m->maxbpg == 0 || (uint)indx % m->maxbpg == 0 || prevbn == 0) {
    if ((mode & UFS2_IFMT) == UFS2_IFDIR)
      return cgmeta(m, inocg);
    if (lbn < NDADDR + m->nindir)
      return cgdata(m, inocg);
    if (indx == 0 || prevbn == 0)
      startcg = inocg + (uint)(lbn / m->maxbpg);
    else
      startcg = dtog(m, prevbn) + 1;
    startcg %= m->ncg;
    avgbfree = rd64(SB(m, cstotal) + CST_NBFREE) / m->ncg;
    for (cg = startcg; cg < m->ncg; cg++)
      if (rd32(CS(m, cg) + CS_NBFREE) >= avgbfree) {
        wr32(SB(m, cgrotor), cg);
        return cgdata(m, cg);
      }
    for (cg = 0; cg < startcg; cg++)
      if (rd32(CS(m, cg) + CS_NBFREE) >= avgbfree) {
        wr32(SB(m, cgrotor), cg);
        return cgdata(m, cg);
      }
    return 0;
  }
  if (dtog(m, prevbn + m->frag) >= m->ncg)
    return 0;
  return prevbn + m->frag;
}

// INODES

static int inode_locate(struct ufs2_mount *m, uint ino, uint64_t *fragno,
                        uint *offset) {
  uint cgx, index;

  if (ino < UFS2_ROOT_INO || ino >= m->ipg * m->ncg)
    return -1;
  cgx = ino_to_cg(m, ino);
  index = ino % m->ipg;
  *fragno = cgimin(m, cgx) + ((uint64_t)(index / m->inopb) << m->fragshift);
  *offset = (index % m->inopb) * UFS2_DINODE_SIZE;
  return 0;
}

static int dinode_read(struct ufs2_mount *m, uint ino, uchar *raw) {
  uint64_t fragno;
  uint offset;

  if (inode_locate(m, ino, &fragno, &offset) < 0)
    return -1;
  return frag_read(m, fragno, offset, raw, UFS2_DINODE_SIZE);
}

static int dinode_write(struct ufs2_mount *m, uint ino, const uchar *raw) {
  uint64_t fragno;
  uint offset;

  if (inode_locate(m, ino, &fragno, &offset) < 0)
    return -1;
  return frag_write(m, fragno, offset, raw, UFS2_DINODE_SIZE);
}

static void dinode_unpack(const uchar *raw, struct ufs2_inode *ip) {
  int i;

  memset(ip, 0, sizeof(*ip));
  ip->mode = rd16(raw + UFS2_DINODE_MODE);
  ip->nlink = rd16(raw + UFS2_DINODE_NLINK);
  ip->uid = rd32(raw + UFS2_DINODE_UID);
  ip->gid = rd32(raw + UFS2_DINODE_GID);
  ip->size = rd64(raw + UFS2_DINODE_SIZE_OFFSET);
  ip->blocks = rd64(raw + UFS2_DINODE_BLOCKS);
  ip->atime.sec = (int64_t)rd64(raw + UFS2_DINODE_ATIME);
  ip->atime.nsec = (int64_t)rd32(raw + UFS2_DINODE_ATIMENSEC);
  ip->mtime.sec = (int64_t)rd64(raw + UFS2_DINODE_MTIME);
  ip->mtime.nsec = (int64_t)rd32(raw + UFS2_DINODE_MTIMENSEC);
  ip->ctime.sec = (int64_t)rd64(raw + UFS2_DINODE_CTIME);
  ip->ctime.nsec = (int64_t)rd32(raw + UFS2_DINODE_CTIMENSEC);
  ip->birthtime.sec = (int64_t)rd64(raw + UFS2_DINODE_BIRTHTIME);
  ip->birthtime.nsec = (int64_t)rd32(raw + UFS2_DINODE_BIRTHNSEC);
  ip->gen = rd32(raw + UFS2_DINODE_GEN);
  ip->flags = rd32(raw + UFS2_DINODE_FLAGS);
  for (i = 0; i < NDADDR; i++)
    ip->direct[i] = rd64(raw + UFS2_DINODE_DIRECT_OFFSET + i * 8);
  for (i = 0; i < NIADDR; i++)
    ip->indirect[i] = rd64(raw + UFS2_DINODE_INDIRECT_OFFSET + i * 8);
}

static int is_meta(const struct ufs2_inode *ip) {
  return (ip->mode & UFS2_IFMT) == UFS2_IFDIR ||
         (ip->mode & UFS2_IFMT) == UFS2_IFLNK;
}

static int inline_link(struct ufs2_mount *m, const struct ufs2_inode *ip) {
  return (ip->mode & UFS2_IFMT) == UFS2_IFLNK && ip->size < m->maxsymlinklen;
}

static void dinode_pack(struct ufs2_mount *m, const struct ufs2_inode *ip,
                        uchar *raw) {
  int i;

  wr16(raw + UFS2_DINODE_MODE, ip->mode);
  wr16(raw + UFS2_DINODE_NLINK, ip->nlink);
  wr32(raw + UFS2_DINODE_UID, ip->uid);
  wr32(raw + UFS2_DINODE_GID, ip->gid);
  wr64(raw + UFS2_DINODE_SIZE_OFFSET, ip->size);
  wr64(raw + UFS2_DINODE_BLOCKS, ip->blocks);
  wr64(raw + UFS2_DINODE_ATIME, (uint64_t)ip->atime.sec);
  wr32(raw + UFS2_DINODE_ATIMENSEC, (uint32_t)ip->atime.nsec);
  wr64(raw + UFS2_DINODE_MTIME, (uint64_t)ip->mtime.sec);
  wr32(raw + UFS2_DINODE_MTIMENSEC, (uint32_t)ip->mtime.nsec);
  wr64(raw + UFS2_DINODE_CTIME, (uint64_t)ip->ctime.sec);
  wr32(raw + UFS2_DINODE_CTIMENSEC, (uint32_t)ip->ctime.nsec);
  wr64(raw + UFS2_DINODE_BIRTHTIME, (uint64_t)ip->birthtime.sec);
  wr32(raw + UFS2_DINODE_BIRTHNSEC, (uint32_t)ip->birthtime.nsec);
  wr32(raw + UFS2_DINODE_GEN, ip->gen);
  wr32(raw + UFS2_DINODE_FLAGS, ip->flags);
  if (inline_link(m, ip))
    return;
  for (i = 0; i < NDADDR; i++)
    wr64(raw + UFS2_DINODE_DIRECT_OFFSET + i * 8, ip->direct[i]);
  for (i = 0; i < NIADDR; i++)
    wr64(raw + UFS2_DINODE_INDIRECT_OFFSET + i * 8, ip->indirect[i]);
}

static int inode_read(struct ufs2_mount *m, uint ino, struct ufs2_inode *ip) {
  uchar raw[UFS2_DINODE_SIZE];

  if (dinode_read(m, ino, raw) < 0)
    return -1;
  dinode_unpack(raw, ip);
  return 0;
}

static int inode_write(struct ufs2_mount *m, uint ino,
                       const struct ufs2_inode *ip) {
  uchar raw[UFS2_DINODE_SIZE];

  if (!m->rw || dinode_read(m, ino, raw) < 0)
    return -1;
  dinode_pack(m, ip, raw);
  if (ip->nlink == 0 && ip->mode != 0)
    orphan_mark(m, ino, 1);
  return dinode_write(m, ino, raw);
}

static void stamp(struct ufs2_inode *ip, int access, int update, int change) {
  struct fnu_timespec now;

  time_now(&now);
  if (access)
    ip->atime = now;
  if (update)
    ip->mtime = now;
  if (change)
    ip->ctime = now;
}

static uint dirpref(struct ufs2_mount *m, uint parent) {
  uint64_t avgifree, avgbfree, avgndir, maxndir, minifree, minbfree;
  uint64_t cgsize, dirsize, curdirsize, maxcontigdirs;
  uint cg, prefcg;

  avgifree = rd64(SB(m, cstotal) + CST_NIFREE) / m->ncg;
  avgbfree = rd64(SB(m, cstotal) + CST_NBFREE) / m->ncg;
  avgndir = rd64(SB(m, cstotal) + CST_NDIR) / m->ncg;
  if (parent == UFS2_ROOT_INO) {
    uint mincg, minndir = m->ipg;
    uint32_t r;

    random_bytes(&r, sizeof(r));
    prefcg = r % m->ncg;
    mincg = prefcg;
    for (cg = 0; cg < m->ncg; cg++) {
      uint c = (prefcg + cg) % m->ncg;
      if (rd32(CS(m, c) + CS_NDIR) < minndir &&
          rd32(CS(m, c) + CS_NIFREE) >= avgifree &&
          rd32(CS(m, c) + CS_NBFREE) >= avgbfree) {
        mincg = c;
        minndir = rd32(CS(m, c) + CS_NDIR);
      }
    }
    return mincg;
  }
  prefcg = ino_to_cg(m, parent);
  maxndir = avgndir + m->ipg / 16;
  if (maxndir > m->ipg)
    maxndir = m->ipg;
  minifree = avgifree - avgifree / 4;
  if (minifree < 1)
    minifree = 1;
  minbfree = avgbfree - avgbfree / 4;
  if (minbfree < 1)
    minbfree = 1;
  cgsize = (uint64_t)m->fsize * m->fpg;
  dirsize = (uint64_t)rd32(SB(m, avgfilesize)) * rd32(SB(m, avgfpdir));
  curdirsize = avgndir ? (cgsize - avgbfree * m->bsize) / avgndir : 0;
  if (dirsize < curdirsize)
    dirsize = curdirsize;
  if (dirsize == 0)
    maxcontigdirs = 0;
  else {
    maxcontigdirs = avgbfree * m->bsize / dirsize;
    if (maxcontigdirs > 255)
      maxcontigdirs = 255;
  }
  if (rd32(SB(m, avgfpdir)) > 0 && maxcontigdirs > m->ipg / rd32(SB(m, avgfpdir)))
    maxcontigdirs = m->ipg / rd32(SB(m, avgfpdir));
  if (maxcontigdirs == 0)
    maxcontigdirs = 1;
  for (cg = 0; cg < m->ncg; cg++) {
    uint c = (prefcg + cg) % m->ncg;
    if (rd32(CS(m, c) + CS_NDIR) < maxndir &&
        rd32(CS(m, c) + CS_NIFREE) >= minifree &&
        rd32(CS(m, c) + CS_NBFREE) >= minbfree &&
        m->contigdirs[c] < maxcontigdirs)
      return c;
  }
  for (cg = 0; cg < m->ncg; cg++) {
    uint c = (prefcg + cg) % m->ncg;
    if (rd32(CS(m, c) + CS_NIFREE) >= avgifree)
      return c;
  }
  return prefcg;
}

static uint nodealloccg(struct ufs2_mount *m, uint cgx, uint ipref,
                        uint16_t mode) {
  uchar *inosused;
  uint start, i, found = 0, initediblk;

  if (rd32(CS(m, cgx) + CS_NIFREE) == 0)
    return 0;
  if (cg_load(m, cgx) < 0 || rd32(CG(m, cs) + CS_NIFREE) == 0)
    return 0;
  inosused = cg_inosused(m);
  if (ipref != 0) {
    ipref %= m->ipg;
    if (!isset_bit(inosused, ipref))
      goto gotit;
  }
  start = rd32(CG(m, irotor));
  if (start >= m->ipg)
    start = 0;
  for (i = 0; i < m->ipg; i++) {
    ipref = (start + i) % m->ipg;
    if (!isset_bit(inosused, ipref)) {
      found = 1;
      break;
    }
  }
  if (!found) {
    printf("UFS2: group %d: nifree says an inode is free and the map has none\n", cgx);
    return 0;
  }
gotit:
  initediblk = rd32(CG(m, initediblk));
  while (ipref + m->inopb > initediblk && initediblk < m->ipg) {
    uchar *block;
    uint k;

    if ((block = malloc(m->bsize, M_UFS, UFS_MEM)) == 0)
      return 0;
    memset(block, 0, m->bsize);
    for (k = 0; k < m->inopb; k++) {
      uint32_t gen = 0;
      while (gen == 0)
        random_bytes(&gen, sizeof(gen));
      wr32(block + k * UFS2_DINODE_SIZE + UFS2_DINODE_GEN, gen);
    }
    if (frag_write(m, cgimin(m, cgx) + ((uint64_t)(initediblk / m->inopb) << m->fragshift),
                   0, block, m->bsize) < 0) {
      free(block, M_UFS);
      return 0;
    }
    free(block, M_UFS);
    initediblk += m->inopb;
    wr32(CG(m, initediblk), initediblk);
    m->cg_dirty = 1;
  }
  wr32(CG(m, irotor), ipref);
  set_bit(inosused, ipref);
  add32(CG(m, cs) + CS_NIFREE, -1);
  add64(SB(m, cstotal) + CST_NIFREE, -1);
  add32(CS(m, cgx) + CS_NIFREE, -1);
  if ((mode & UFS2_IFMT) == UFS2_IFDIR) {
    add32(CG(m, cs) + CS_NDIR, 1);
    add64(SB(m, cstotal) + CST_NDIR, 1);
    add32(CS(m, cgx) + CS_NDIR, 1);
  }
  sb_touch(m);
  m->cg_dirty = 1;
  return cgx * m->ipg + ipref;
}

static int inode_alloc(struct ufs2_mount *m, uint16_t mode, uint parent,
                       uint *out) {
  uchar raw[UFS2_DINODE_SIZE];
  struct ufs2_inode ip;
  struct proc *p = curproc;
  uint ipref, cgx, ino = 0, i;
  uint32_t gen;

  if (!m->rw || rd64(SB(m, cstotal) + CST_NIFREE) == 0)
    return -1;
  if ((mode & UFS2_IFMT) == UFS2_IFDIR)
    ipref = dirpref(m, parent) * m->ipg;
  else
    ipref = parent;
  if (ipref >= m->ncg * m->ipg)
    ipref = 0;
  cgx = ino_to_cg(m, ipref);
  if ((mode & UFS2_IFMT) == UFS2_IFDIR) {
    if (m->contigdirs[cgx] < 255)
      m->contigdirs[cgx]++;
  } else if (m->contigdirs[cgx] > 0) {
    m->contigdirs[cgx]--;
  }
  // quadratic rehash, then every group
  if ((ino = nodealloccg(m, cgx, ipref, mode)) == 0) {
    uint c = cgx;
    for (i = 1; i < m->ncg && ino == 0; i *= 2) {
      c += i;
      if (c >= m->ncg)
        c -= m->ncg;
      ino = nodealloccg(m, c, 0, mode);
    }
    c = (cgx + 2) % m->ncg;
    for (i = 2; i < m->ncg && ino == 0; i++) {
      ino = nodealloccg(m, c, 0, mode);
      if (++c == m->ncg)
        c = 0;
    }
  }
  if (ino < UFS2_ROOT_INO)
    return -1;
  if (dinode_read(m, ino, raw) < 0)
    return -1;
  gen = rd32(raw + UFS2_DINODE_GEN);
  while (gen == 0 || ++gen == 0)
    random_bytes(&gen, sizeof(gen));
  memset(raw, 0, sizeof(raw));
  memset(&ip, 0, sizeof(ip));
  ip.mode = mode;
  ip.nlink = 1;
  ip.uid = p ? p->p_p->uid : 0;
  ip.gid = p ? p->p_p->gid : 0;
  ip.gen = gen;
  stamp(&ip, 1, 1, 1);
  ip.birthtime = ip.ctime;
  dinode_pack(m, &ip, raw);
  if (dinode_write(m, ino, raw) < 0)
    return -1;
  *out = ino;
  return 0;
}

static int inode_free(struct ufs2_mount *m, uint ino) {
  uchar raw[UFS2_DINODE_SIZE], *inosused;
  uint cgx = ino_to_cg(m, ino), cgino = ino % m->ipg;
  uint16_t mode;
  uint32_t gen;

  if (!m->rw || ino < UFS2_ROOT_INO || ino >= m->ipg * m->ncg ||
      dinode_read(m, ino, raw) < 0)
    return -1;
  mode = rd16(raw + UFS2_DINODE_MODE);
  gen = rd32(raw + UFS2_DINODE_GEN);
  memset(raw, 0, sizeof(raw));
  wr32(raw + UFS2_DINODE_GEN, gen);
  if (dinode_write(m, ino, raw) < 0 || cg_load(m, cgx) < 0)
    return -1;
  inosused = cg_inosused(m);
  if (!isset_bit(inosused, cgino)) {
    printf("UFS2: inode %d freed while free\n", ino);
    return -1;
  }
  clr_bit(inosused, cgino);
  orphan_mark(m, ino, 0);
  if (cgino < rd32(CG(m, irotor)))
    wr32(CG(m, irotor), cgino);
  add32(CG(m, cs) + CS_NIFREE, 1);
  add64(SB(m, cstotal) + CST_NIFREE, 1);
  add32(CS(m, cgx) + CS_NIFREE, 1);
  if ((mode & UFS2_IFMT) == UFS2_IFDIR) {
    add32(CG(m, cs) + CS_NDIR, -1);
    add64(SB(m, cstotal) + CST_NDIR, -1);
    add32(CS(m, cgx) + CS_NDIR, -1);
  }
  sb_touch(m);
  m->cg_dirty = 1;
  bio_forget_file(m->dev, FILEOWNER(m, ino), 0);
  return 0;
}

// THE BLOCK MAP

static uint blksize(struct ufs2_mount *m, const struct ufs2_inode *ip,
                    uint64_t lbn) {
  if (lbn >= NDADDR || ip->size >= ((lbn + 1) << m->bshift))
    return m->bsize;
  return (uint)fragroundup(m, blkoff(m, ip->size));
}

static int indirs(struct ufs2_mount *m, uint64_t lbn, uint *level,
                  uint idx[NIADDR]) {
  uint64_t base = NDADDR, span = m->nindir, rest;
  uint l, i;

  for (l = 1; l <= NIADDR; l++) {
    if (lbn < base + span) {
      rest = lbn - base;
      for (i = l; i > 0; i--) {
        idx[i - 1] = (uint)(rest % m->nindir);
        rest /= m->nindir;
      }
      *level = l;
      return 0;
    }
    base += span;
    if (span > ~(uint64_t)0 / m->nindir)
      return -1;
    span *= m->nindir;
  }
  return -1;
}

static int indir_get(struct ufs2_mount *m, uint64_t blk, uint idx,
                     uint64_t *out) {
  uchar raw[8];

  if (frag_read(m, blk, idx * 8, raw, 8) < 0)
    return -1;
  *out = rd64(raw);
  return 0;
}

static int indir_put(struct ufs2_mount *m, uint64_t blk, uint idx,
                     uint64_t value) {
  uchar raw[8];

  wr64(raw, value);
  return frag_write(m, blk, idx * 8, raw, 8);
}

// 0 for a hole
static int bmap(struct ufs2_mount *m, const struct ufs2_inode *ip,
                uint64_t lbn, uint64_t *fragno) {
  uint level, idx[NIADDR], l;
  uint64_t blk;

  if (lbn < NDADDR) {
    *fragno = ip->direct[lbn];
    return 0;
  }
  if (indirs(m, lbn, &level, idx) < 0)
    return -1;
  blk = ip->indirect[level - 1];
  for (l = 0; l < level && blk != 0; l++)
    if (blk >= m->size || indir_get(m, blk, idx[l], &blk) < 0)
      return -1;
  *fragno = blk;
  return 0;
}

static int balloc(struct ufs2_mount *m, uint ino, struct ufs2_inode *ip,
                  uint64_t lbn, uint size, uint64_t *fragno, uint *bsizep,
                  int *freshp, int meta) {
  uint64_t lastlbn = lblkno(m, ip->size), nb, newb, pref;
  uint osize, nsize, level, idx[NIADDR], l;
  uchar *indir;
  int error;

  if (size > m->bsize)
    return -FNU_ERR_TOOLARGE;
  if (!m->rw)
    return -FNU_ERR_READONLY;
  *bsizep = m->bsize;
  *freshp = 0;
  if (lastlbn < NDADDR && lastlbn < lbn) {
    nb = lastlbn;
    osize = blksize(m, ip, nb);
    if (osize < m->bsize && osize > 0) {
      pref = blkpref(m, ino, ip->mode, ip, nb, (int)nb,
                     nb > 0 ? ip->direct[nb - 1] : 0);
      newb = realloccg(m, ino, nb, ip->direct[nb], pref, osize, m->bsize, meta);
      if (newb == 0)
        return -FNU_ERR_NOSPACE;
      ip->blocks += btodb(m->bsize - osize);
      ip->size = (nb + 1) << m->bshift;
      ip->direct[nb] = newb;
    }
  }
  if (lbn < NDADDR) {
    nb = ip->direct[lbn];
    if (nb != 0 && ip->size >= ((lbn + 1) << m->bshift)) {
      *fragno = nb;
      return 0;
    }
    if (nb != 0) {
      osize = (uint)fragroundup(m, blkoff(m, ip->size));
      nsize = (uint)fragroundup(m, size);
      if (nsize <= osize) {
        *fragno = nb;
        *bsizep = osize;
        return 0;
      }
      pref = blkpref(m, ino, ip->mode, ip, lbn, (int)lbn,
                     lbn > 0 ? ip->direct[lbn - 1] : 0);
      newb = realloccg(m, ino, lbn, nb, pref, osize, nsize, meta);
      if (newb == 0)
        return -FNU_ERR_NOSPACE;
      ip->blocks += btodb(nsize - osize);
      ip->direct[lbn] = newb;
      *fragno = newb;
      *bsizep = nsize;
      return 0;
    }
    nsize = ip->size < ((lbn + 1) << m->bshift) ? (uint)fragroundup(m, size)
                                                 : m->bsize;
    pref = blkpref(m, ino, ip->mode, ip, lbn, (int)lbn,
                   lbn > 0 ? ip->direct[lbn - 1] : 0);
    newb = blkalloc(m, ino, pref, nsize);
    if (newb == 0)
      return -FNU_ERR_NOSPACE;
    ip->blocks += btodb(nsize);
    ip->direct[lbn] = newb;
    *fragno = newb;
    *bsizep = nsize;
    *freshp = 1;
    return 0;
  }

  if (indirs(m, lbn, &level, idx) < 0)
    return -FNU_ERR_TOOLARGE;
  nb = ip->indirect[level - 1];
  if (nb == 0) {
    pref = blkpref(m, ino, ip->mode, ip, lbn, -(int)level, 0);
    if ((nb = blkalloc(m, ino, pref, m->bsize)) == 0)
      return -FNU_ERR_NOSPACE;
    if (frag_zero(m, nb, 0, m->bsize) < 0)
      return -FNU_ERR_IO;
    ip->blocks += btodb(m->bsize);
    ip->indirect[level - 1] = nb;
  }
  if ((indir = malloc(m->bsize, M_UFS, UFS_MEM)) == 0)
    return -FNU_ERR_NOMEMORY;
  error = -FNU_ERR_IO;
  for (l = 0; l < level; l++) {
    uint64_t next;

    if (frag_read(m, nb, 0, indir, m->bsize) < 0)
      goto fail;
    next = rd64(indir + idx[l] * 8);
    if (next != 0) {
      nb = next;
      continue;
    }
    if (l + 1 < level)
      pref = blkpref(m, ino, ip->mode, ip, lbn, -(int)(level - l - 1), 0);
    else
      pref = blkpref(m, ino, ip->mode, ip, lbn, (int)idx[l],
                     idx[l] > 0 ? rd64(indir + (idx[l] - 1) * 8) : 0);
    if ((next = blkalloc(m, ino, pref, m->bsize)) == 0) {
      error = -FNU_ERR_NOSPACE;
      goto fail;
    }
    if ((l + 1 < level && frag_zero(m, next, 0, m->bsize) < 0) ||
        indir_put(m, nb, idx[l], next) < 0)
      goto fail;
    if (l + 1 == level)
      *freshp = 1;
    ip->blocks += btodb(m->bsize);
    nb = next;
  }
  free(indir, M_UFS);
  *fragno = nb;
  return 0;
fail:
  free(indir, M_UFS);
  return error;
}

static int read_bytes(struct ufs2_mount *m, uint ino,
                      const struct ufs2_inode *ip, uint64_t offset,
                      uchar *dst, uint length) {
  if (offset > ip->size || length > ip->size - offset)
    return -1;
  while (length > 0) {
    uint64_t lbn = lblkno(m, offset), fragno;
    uint off = blkoff(m, offset), n = m->bsize - off;
    struct buf *bp;

    if (n > length)
      n = length;
    if (bmap(m, ip, lbn, &fragno) < 0)
      return -1;
    if (fragno == 0) {
      memset(dst, 0, n);
    } else {
      uint bs = blksize(m, ip, lbn);

      if (off + n > bs ||
          (bp = data_buf(m, ino, lbn, fragno, bs, 0)) == 0)
        return -1;
      memmove(dst, bp->data + off, n);
      if (off + n == bs || offset + n == ip->size)
        bp->flags |= B_AGE;
      brelse(bp);
    }
    dst += n;
    offset += n;
    length -= n;
  }
  return 0;
}

static int write_bytes(struct ufs2_mount *m, uint ino, struct ufs2_inode *ip,
                       uint64_t offset, const uchar *src, uint length) {
  int meta = is_meta(ip), error;

  if (!m->rw)
    return -FNU_ERR_READONLY;
  if (offset > m->maxfilesize || length > m->maxfilesize - offset)
    return -FNU_ERR_TOOLARGE;
  while (length > 0) {
    uint64_t lbn = lblkno(m, offset), fragno;
    uint off = blkoff(m, offset), n = m->bsize - off;

    if (n > length)
      n = length;
    uint bs;
    int fresh;
    struct buf *bp;

    if ((error = balloc(m, ino, ip, lbn, off + n, &fragno, &bs, &fresh, meta)) < 0)
      return error;
    if (off + n > bs ||
        (bp = data_buf(m, ino, lbn, fragno, bs, fresh || (off == 0 && n == bs))) == 0)
      return -FNU_ERR_IO;
    if (fresh && (off != 0 || n != bs))
      memset(bp->data, 0, bs);
    memmove(bp->data + off, src, n);
    if (off + n == bs || offset + n >= ip->size)
      bp->flags |= B_AGE;
    if (meta)
      meta_pin(m, bp);
    bdwrite(bp);
    if (offset + n > ip->size)
      ip->size = offset + n;
    src += n;
    offset += n;
    length -= n;
  }
  return 0;
}

static int read_bytes_uio(struct ufs2_mount *m, uint ino,
                          const struct ufs2_inode *ip, uint64_t offset,
                          struct uio *uio, uint length) {
  if (offset > ip->size || length > ip->size - offset)
    return -1;
  while (length > 0) {
    uint64_t lbn = lblkno(m, offset), fragno;
    uint off = blkoff(m, offset), n = m->bsize - off;

    if (n > length)
      n = length;
    if (bmap(m, ip, lbn, &fragno) < 0)
      return -1;
    if (fragno == 0) {
      // a hole reads as zeros
      static const uchar zeros[DEV_BSIZE];
      uint left = n;

      while (left > 0) {
        uint part = left < DEV_BSIZE ? left : DEV_BSIZE;

        if (uiomove((void *)zeros, part, uio) != 0)
          return -1;
        left -= part;
      }
    } else {
      uint bs = blksize(m, ip, lbn);
      struct buf *bp;

      if (off + n > bs || (bp = data_buf(m, ino, lbn, fragno, bs, 0)) == 0)
        return -1;
      if (uiomove(bp->data + off, n, uio) != 0) {
        brelse(bp);
        return -1;
      }
      if (off + n == bs || offset + n == ip->size)
        bp->flags |= B_AGE;
      brelse(bp);
    }
    offset += n;
    length -= n;
  }
  return 0;
}

static int write_bytes_uio(struct ufs2_mount *m, uint ino,
                           struct ufs2_inode *ip, uint64_t offset,
                           struct uio *uio, uint length) {
  int error;

  if (!m->rw)
    return -FNU_ERR_READONLY;
  if (offset > m->maxfilesize || length > m->maxfilesize - offset)
    return -FNU_ERR_TOOLARGE;
  while (length > 0) {
    uint64_t lbn = lblkno(m, offset), fragno;
    uint off = blkoff(m, offset), n = m->bsize - off;

    if (n > length)
      n = length;
    uint bs;
    int fresh;
    struct buf *bp;

    if (tx_full(m) && (inode_write(m, ino, ip) < 0 || tx_restart(m) < 0))
      return -FNU_ERR_IO;
    if ((error = balloc(m, ino, ip, lbn, off + n, &fragno, &bs, &fresh, 0)) < 0)
      return error;
    if (off + n > bs ||
        (bp = data_buf(m, ino, lbn, fragno, bs, fresh || (off == 0 && n == bs))) == 0)
      return -FNU_ERR_IO;
    if (fresh && (off != 0 || n != bs))
      memset(bp->data, 0, bs);
    if (uiomove(bp->data + off, n, uio) != 0) {
      brelse(bp);
      return -FNU_ERR_IO;
    }
    if (off + n == bs || offset + n >= ip->size)
      bp->flags |= B_AGE;
    bdwrite(bp);
    if (offset + n > ip->size)
      ip->size = offset + n;
    offset += n;
    length -= n;
  }
  return 0;
}


// lastbn -1 keeps nothing
static int indirtrunc(struct ufs2_mount *m, struct ufs2_inode *ip,
                      uint64_t blk, uint level, int64_t lastbn) {
  uchar *raw;
  uint i, l;
  uint64_t factor = 1;
  int64_t last;
  int r = 0, dirty = 0;

  for (l = 1; l < level; l++)
    factor *= m->nindir;
  last = lastbn < 0 ? -1 : (int64_t)((uint64_t)lastbn / factor);
  if (last >= (int64_t)m->nindir)
    return 0;
  if (blk >= m->size ||
      (raw = malloc(m->bsize, M_UFS, UFS_MEM)) == 0)
    return -1;
  if (frag_read(m, blk, 0, raw, m->bsize) < 0) {
    free(raw, M_UFS);
    return -1;
  }
  for (i = m->nindir; (int64_t)i > last + 1; i--) {
    uint64_t child = rd64(raw + (i - 1) * 8);
    if (child == 0)
      continue;
    if (level > 1) {
      if (indirtrunc(m, ip, child, level - 1, -1) < 0)
        r = -1;
    } else {
      if (blkfree(m, child, m->bsize, is_meta(ip)) < 0)
        r = -1;
      ip->blocks -= btodb(m->bsize);
    }
    wr64(raw + (i - 1) * 8, 0);
    dirty = 1;
  }
  if (last >= 0 && level > 1) {
    uint64_t child = rd64(raw + (uint64_t)last * 8);
    if (child != 0 &&
        indirtrunc(m, ip, child, level - 1, lastbn - last * (int64_t)factor) < 0)
      r = -1;
  }
  if (last >= 0) {
    if (dirty && frag_write(m, blk, 0, raw, m->bsize) < 0)
      r = -1;
    free(raw, M_UFS);
    return r;
  }
  free(raw, M_UFS);
  bio_forget_run(m->dev, m->device.first_lba + blk * (m->fsize / DEV_BSIZE),
                 m->bsize / DEV_BSIZE);
  if (blkfree(m, blk, m->bsize, 1) < 0)
    r = -1;
  ip->blocks -= btodb(m->bsize);
  return r;
}

static int truncate_once(struct ufs2_mount *m, uint ino, struct ufs2_inode *ip,
                         uint64_t length) {
  int64_t lastblock, lastiblock[NIADDR];
  uint64_t osize = ip->size, span, bn;
  uint i, l, oldspace, newspace, off;
  int r = 0;

  if (!m->rw)
    return -FNU_ERR_READONLY;
  if (length > m->maxfilesize)
    return -FNU_ERR_TOOLARGE;
  if (inline_link(m, ip)) {
    if (length != 0)
      return -FNU_ERR_INVALID;
    ip->size = 0;
    ip->blocks = 0;
    memset(ip->direct, 0, sizeof(ip->direct));
    goto done;
  }
  if (length > osize) {
    uchar zero = 0;
    if ((r = write_bytes(m, ino, ip, length - 1, &zero, 1)) < 0)
      return r;
    r = 0;
    goto done;
  }
  if (length == osize)
    return 0;
  off = blkoff(m, length);
  if (off != 0) {
    uint64_t fragno, lbn = lblkno(m, length);
    uint bs = blksize(m, ip, lbn), keep;

    ip->size = length;
    keep = blksize(m, ip, lbn);
    ip->size = osize;
    if (bmap(m, ip, lbn, &fragno) == 0 && fragno != 0 && bs > off) {
      struct buf *bp = data_buf(m, ino, lbn, fragno, bs, 0);

      if (bp == 0)
        return -FNU_ERR_IO;
      memset(bp->data + off, 0, keep - off);
      if (bio_regrow(bp, bp->blockno, keep) < 0) {
        brelse(bp);
        return -FNU_ERR_IO;
      }
      if (is_meta(ip))
        meta_pin(m, bp);
      bdwrite(bp);
    }
  }
  lastblock = (int64_t)lblkno(m, length + m->bsize - 1) - 1;
  lastiblock[0] = lastblock - NDADDR;
  span = m->nindir;
  for (l = 1; l < NIADDR; l++) {
    lastiblock[l] = lastiblock[l - 1] - (int64_t)span;
    span *= m->nindir;
  }
  for (l = NIADDR; l > 0; l--) {
    bn = ip->indirect[l - 1];
    if (bn == 0)
      continue;
    if (indirtrunc(m, ip, bn, l, lastiblock[l - 1]) < 0)
      r = -FNU_ERR_IO;
    if (lastiblock[l - 1] < 0)
      ip->indirect[l - 1] = 0;
  }
  for (i = NDADDR; (int64_t)i > lastblock + 1; i--) {
    uint bs;
    bn = ip->direct[i - 1];
    if (bn == 0)
      continue;
    bs = blksize(m, ip, i - 1); // by the old size
    if (blkfree(m, bn, bs, is_meta(ip)) < 0)
      r = -FNU_ERR_IO;
    ip->blocks -= btodb(bs);
    ip->direct[i - 1] = 0;
  }
  if (lastblock >= 0 && lastblock < NDADDR && (bn = ip->direct[lastblock]) != 0) {
    oldspace = blksize(m, ip, (uint64_t)lastblock);
    ip->size = length;
    newspace = blksize(m, ip, (uint64_t)lastblock);
    if (oldspace > newspace) {
      if (blkfree(m, bn + numfrags(m, newspace), oldspace - newspace,
                  is_meta(ip)) < 0)
        r = -FNU_ERR_IO;
      ip->blocks -= btodb(oldspace - newspace);
    }
  }
  ip->size = length;
  if (length == 0)
    ip->blocks = 0;
  bio_forget_file(m->dev, FILEOWNER(m, ino),
                  lblkno(m, length) + (off != 0 ? 1 : 0));
done:
  stamp(ip, 0, 1, 1);
  if (inode_write(m, ino, ip) < 0)
    return -FNU_ERR_IO;
  return r;
}

static int truncate_to(struct ufs2_mount *m, uint ino, struct ufs2_inode *ip,
                       uint64_t length) {
  uint64_t step;
  int r;

  if (m->log == 0 || length >= ip->size || inline_link(m, ip))
    return truncate_once(m, ino, ip, length);
  step = (ulog_room(m->log) / 4 / m->cgsize) << m->bshift;
  if (step == 0)
    step = m->bsize;
  while (ip->size > length) {
    uint64_t next = length;

    if (ip->size - length > step)
      next = (ip->size - step) & ~(uint64_t)(m->bsize - 1);
    if (next < length)
      next = length;
    if ((r = truncate_once(m, ino, ip, next)) < 0)
      return r;
    if (ip->size > length && tx_full(m) && tx_restart(m) < 0)
      return -FNU_ERR_IO;
  }
  return 0;
}

#define DIRECTSIZ(namlen) ((UFS2_DIR_HEADER_SIZE + (namlen) + 1 + 3) & ~3U)

// DIRECTORIES

static int name_length(const char *name, uint *length) {
  uint n;

  if (name == 0)
    return -1;
  for (n = 0; name[n] != 0; n++) {
    if (name[n] == '/' || n >= UFS2_MAX_NAME)
      return -1;
  }
  if (n == 0)
    return -1;
  *length = n;
  return 0;
}

static int record_ok(const uchar *chunk, uint off, uint limit) {
  uint reclen, namlen;

  if (off + UFS2_DIR_HEADER_SIZE > limit)
    return 0;
  reclen = rd16(chunk + off + 4);
  namlen = chunk[off + 7];
  return reclen >= UFS2_DIR_HEADER_SIZE && reclen % 4 == 0 &&
         reclen <= limit - off && namlen <= UFS2_MAX_NAME &&
         DIRECTSIZ(namlen) <= reclen;
}

static int dir_chunk(struct ufs2_mount *m, uint dir_ino,
                     const struct ufs2_inode *dir, uint64_t offset,
                     uchar *chunk, uint *length) {
  uint n = dir->size - offset < DIRBLKSIZ ? (uint)(dir->size - offset) : DIRBLKSIZ;

  if (read_bytes(m, dir_ino, dir, offset, chunk, n) < 0)
    return -1;
  *length = n;
  return 0;
}

static int dir_lookup(struct ufs2_mount *m, uint dir_ino, const char *name,
                      uint *ino) {
  struct ufs2_inode dir;
  uchar chunk[DIRBLKSIZ];
  uint want, off, n;
  uint64_t pos;

  if (name_length(name, &want) < 0 || inode_read(m, dir_ino, &dir) < 0 ||
      (dir.mode & UFS2_IFMT) != UFS2_IFDIR)
    return -1;
  for (pos = 0; pos < dir.size; pos += DIRBLKSIZ) {
    if (dir_chunk(m, dir_ino, &dir, pos, chunk, &n) < 0)
      return -1;
    for (off = 0; record_ok(chunk, off, n); off += rd16(chunk + off + 4)) {
      if (rd32(chunk + off) != 0 && chunk[off + 7] == want &&
          memcmp(chunk + off + UFS2_DIR_HEADER_SIZE, name, want) == 0) {
        *ino = rd32(chunk + off);
        return 0;
      }
    }
  }
  return -1;
}

static int dir_enter(struct ufs2_mount *m, uint dir_ino, const char *name,
                     uint ino, uchar type) {
  struct ufs2_inode dir;
  uchar chunk[DIRBLKSIZ];
  uint namlen, needed, off, n, existing;
  uint64_t pos;

  if (!m->rw || ino < UFS2_ROOT_INO || name_length(name, &namlen) < 0 ||
      inode_read(m, dir_ino, &dir) < 0 || (dir.mode & UFS2_IFMT) != UFS2_IFDIR ||
      dir.size % DIRBLKSIZ != 0)
    return -1;
  if (dir_lookup(m, dir_ino, name, &existing) == 0)
    return -1;
  needed = DIRECTSIZ(namlen);
  for (pos = 0; pos < dir.size; pos += DIRBLKSIZ) {
    uint slack = 0, at = 0, found = 0;

    if (dir_chunk(m, dir_ino, &dir, pos, chunk, &n) < 0)
      return -1;
    for (off = 0; record_ok(chunk, off, n); off += rd16(chunk + off + 4)) {
      uint reclen = rd16(chunk + off + 4);
      uint dsize = rd32(chunk + off) == 0 ? 0 : DIRECTSIZ(chunk[off + 7]);

      slack += reclen - dsize;
      if (found || reclen - dsize < needed)
        continue;
      wr16(chunk + off + 4, (uint16_t)dsize);
      at = off + dsize;
      memset(chunk + at, 0, reclen - dsize);
      wr16(chunk + at + 4, (uint16_t)(reclen - dsize));
      found = 1;
    }
    if (!found && slack >= needed && off == n) {
      // compaction
      uchar packed[DIRBLKSIZ];
      uint to = 0, from;

      memset(packed, 0, sizeof(packed));
      for (from = 0; from < n; from += rd16(chunk + from + 4)) {
        uint dsize = DIRECTSIZ(chunk[from + 7]);
        if (rd32(chunk + from) == 0)
          continue;
        memmove(packed + to, chunk + from, dsize);
        wr16(packed + to + 4, (uint16_t)dsize);
        to += dsize;
      }
      memmove(chunk, packed, n);
      at = to;
      wr16(chunk + at + 4, (uint16_t)(n - to));
      found = 1;
    }
    if (found) {
      uint reclen = rd16(chunk + at + 4);
      memset(chunk + at, 0, reclen);
      wr32(chunk + at, ino);
      wr16(chunk + at + 4, (uint16_t)reclen);
      chunk[at + 6] = type;
      chunk[at + 7] = (uchar)namlen;
      memmove(chunk + at + UFS2_DIR_HEADER_SIZE, name, namlen);
      if (write_bytes(m, dir_ino, &dir, pos, chunk, n) < 0)
        return -1;
      stamp(&dir, 0, 1, 1);
      return inode_write(m, dir_ino, &dir);
    }
  }
  memset(chunk, 0, sizeof(chunk));
  wr32(chunk, ino);
  wr16(chunk + 4, DIRBLKSIZ);
  chunk[6] = type;
  chunk[7] = (uchar)namlen;
  memmove(chunk + UFS2_DIR_HEADER_SIZE, name, namlen);
  if (write_bytes(m, dir_ino, &dir, dir.size, chunk, DIRBLKSIZ) < 0)
    return -1;
  stamp(&dir, 0, 1, 1);
  return inode_write(m, dir_ino, &dir);
}

static int dir_remove(struct ufs2_mount *m, uint dir_ino, const char *name,
                      uint *ino) {
  struct ufs2_inode dir;
  uchar chunk[DIRBLKSIZ];
  uint want, off, n, prev;
  uint64_t pos;

  if (!m->rw || name_length(name, &want) < 0 || inode_read(m, dir_ino, &dir) < 0 ||
      (dir.mode & UFS2_IFMT) != UFS2_IFDIR)
    return -1;
  for (pos = 0; pos < dir.size; pos += DIRBLKSIZ) {
    if (dir_chunk(m, dir_ino, &dir, pos, chunk, &n) < 0)
      return -1;
    prev = 0;
    for (off = 0; record_ok(chunk, off, n); prev = off, off += rd16(chunk + off + 4)) {
      uint reclen = rd16(chunk + off + 4);
      if (rd32(chunk + off) == 0 || chunk[off + 7] != want ||
          memcmp(chunk + off + UFS2_DIR_HEADER_SIZE, name, want) != 0)
        continue;
      if (m->log_ino != 0 && rd32(chunk + off) == m->log_ino)
        return -1;
      if (ino != 0)
        *ino = rd32(chunk + off);
      memset(chunk + off, 0, reclen);
      if (off == 0)
        wr16(chunk + 4, (uint16_t)reclen);
      else
        wr16(chunk + prev + 4, (uint16_t)(rd16(chunk + prev + 4) + reclen));
      if (write_bytes(m, dir_ino, &dir, pos, chunk, n) < 0)
        return -1;
      stamp(&dir, 0, 1, 1);
      return inode_write(m, dir_ino, &dir);
    }
  }
  return -1;
}

static int sb_check(struct ufs2_mount *m) {
  const uchar *sb = m->sb;
  uint bsize = rd32(sb + UFS2_SB_bsize), fsize = rd32(sb + UFS2_SB_fsize);
  uint frag = rd32(sb + UFS2_SB_frag), sbsize = rd32(sb + UFS2_SB_sbsize);
  uint ncg = rd32(sb + UFS2_SB_ncg), ipg = rd32(sb + UFS2_SB_ipg);
  uint fpg = rd32(sb + UFS2_SB_fpg), cgsize = rd32(sb + UFS2_SB_cgsize);
  uint inopb = rd32(sb + UFS2_SB_inopb), nindir = rd32(sb + UFS2_SB_nindir);
  uint64_t size = rd64(sb + UFS2_SB_size), dsize = rd64(sb + UFS2_SB_dsize);
  uint64_t cssize = rd32(sb + UFS2_SB_cssize), csaddr = rd64(sb + UFS2_SB_csaddr);

  if (rd32(sb + UFS2_SB_magic) != UFS2_MAGIC ||
      rd64(sb + UFS2_SB_sblockloc) != SBLOCK_UFS2 ||
      !power_of_two(bsize) || bsize < 4096 || bsize > MAXBSIZE ||
      !power_of_two(fsize) || fsize < DEV_BSIZE || fsize > bsize ||
      frag != bsize / fsize || frag > UFS2_MAXFRAG ||
      rd32(sb + UFS2_SB_bshift) != (uint)(ffs(bsize) - 1) ||
      rd32(sb + UFS2_SB_fshift) != (uint)(ffs(fsize) - 1) ||
      rd32(sb + UFS2_SB_fragshift) != (uint)(ffs(frag) - 1) ||
      rd32(sb + UFS2_SB_fsbtodb) != (uint)(ffs(fsize / DEV_BSIZE) - 1) ||
      sbsize < 1376 || sbsize > UFS2_SUPERBLOCK_SIZE ||
      inopb != bsize / UFS2_DINODE_SIZE || nindir != bsize / 8 ||
      ncg == 0 || ipg == 0 || ipg % inopb != 0 || fpg == 0 || fpg % frag != 0 ||
      cgsize < UFS2_CG_HEADER_SIZE || cgsize > bsize ||
      (int32_t)rd32(sb + UFS2_SB_sblkno) < 0 ||
      (int32_t)rd32(sb + UFS2_SB_cblkno) < 0 ||
      (int32_t)rd32(sb + UFS2_SB_iblkno) < 0 ||
      (int32_t)rd32(sb + UFS2_SB_dblkno) < 0 ||
      rd32(sb + UFS2_SB_dblkno) > fpg || size == 0 || dsize > size ||
      size > (uint64_t)fpg * ncg || (uint64_t)fpg * (ncg - 1) >= size ||
      cssize < (uint64_t)ncg * CS_SIZE || cssize % fsize != 0 ||
      csaddr + cssize / fsize > size ||
      rd32(sb + UFS2_SB_minfree) > 99 ||
      rd32(sb + UFS2_SB_contigsumsize) > 16 ||
      rd32(sb + UFS2_SB_maxsymlinklen) > NDADDR * 8 + NIADDR * 8 ||
      size * fsize / DEV_BSIZE > m->device.sector_count)
    return -1;
  m->bsize = bsize;
  m->fsize = fsize;
  m->frag = frag;
  m->fragshift = (uint)(ffs(frag) - 1);
  m->bshift = (uint)(ffs(bsize) - 1);
  m->fshift = (uint)(ffs(fsize) - 1);
  m->ncg = ncg;
  m->ipg = ipg;
  m->fpg = fpg;
  m->inopb = inopb;
  m->nindir = nindir;
  m->cgsize = cgsize;
  m->sbsize = sbsize;
  m->sblkno = rd32(sb + UFS2_SB_sblkno);
  m->cblkno = rd32(sb + UFS2_SB_cblkno);
  m->iblkno = rd32(sb + UFS2_SB_iblkno);
  m->dblkno = rd32(sb + UFS2_SB_dblkno);
  m->minfree = rd32(sb + UFS2_SB_minfree);
  m->contigsumsize = rd32(sb + UFS2_SB_contigsumsize);
  m->maxsymlinklen = rd32(sb + UFS2_SB_maxsymlinklen);
  m->maxbpg = rd32(sb + UFS2_SB_maxbpg);
  m->size = size;
  m->dsize = dsize;
  m->csaddr = csaddr;
  m->cssize = cssize;
  m->metaspace = rd64(sb + UFS2_SB_metaspace);
  m->maxfilesize = rd64(sb + UFS2_SB_maxfilesize);
  if (m->metaspace > fpg)
    m->metaspace = 0;
  return 0;
}

static void mount_release(struct ufs2_mount *m) {
  struct sleeplock lock = m->lock;
  struct mutex txlock = m->txlock;
  uint id = m->id, i;

  if (m->used) {
    bio_flush_device(m->dev);
    bio_forget_mount(m->dev, m->id);
  }
  if (m->overlay != 0) {
    ulog_overlay_close(m->overlay);
    bio_forget_dev(m->dev);
  }
  if (m->sb)
    free(m->sb, M_UFS);
  if (m->cg)
    free(m->cg, M_UFS);
  if (m->csum)
    free(m->csum, M_UFS);
  if (m->contigdirs)
    free(m->contigdirs, M_UFS);
  for (i = 0; m->pending != 0 && i < m->ncg; i++)
    if (m->pending[i] != 0)
      free(m->pending[i], M_UFS);
  if (m->pending != 0)
    free(m->pending, M_UFS);
  if (m->orph != 0)
    free(m->orph, M_UFS);
  memset(m, 0, sizeof(*m));
  m->lock = lock;
  m->txlock = txlock;
  m->id = id;
  m->cgx = -1;
}

static int mount_attach(struct ufs2_mount *m, const struct blockdev *device) {
  uint dev = device->device;

  mount_release(m);
  m->device = *device;
  m->dev = dev;
  if ((m->sb = malloc(UFS2_SUPERBLOCK_SIZE, M_UFS, UFS_MEM)) == 0 ||
      blockdev_read(device, SBLOCK_UFS2, m->sb, UFS2_SUPERBLOCK_SIZE) < 0 ||
      sb_check(m) < 0 || (m->cg = malloc(m->cgsize, M_UFS, UFS_MEM)) == 0 ||
      (m->csum = malloc(m->cssize, M_UFS, UFS_MEM)) == 0 ||
      (m->contigdirs = malloc(m->ncg, M_UFS, UFS_MEM)) == 0 ||
      frag_read(m, m->csaddr, 0, m->csum, (uint)m->cssize) < 0) {
    mount_release(m);
    return -1;
  }
  memset(m->contigdirs, 0, m->ncg);
  m->mounted_time = (int64_t)rd64(SB(m, time));
  m->used = 1;
  m->cgx = -1;
  return 0;
}

static int sb_write(struct ufs2_mount *m) {
  struct fnu_timespec now;

  if (frag_write(m, m->csaddr, 0, m->csum, (uint)m->cssize) < 0)
    return -1;
  time_now(&now);
  wr64(SB(m, time), (uint64_t)now.sec);
  m->sb[UFS2_SB_fmod] = 0;
  if (block_move(m, SBLOCK_UFS2, m->sb, m->sbsize, 1, 0) < 0)
    return -1;
  m->fmod = 0;
  return 0;
}

static int sync_locked(struct ufs2_mount *m) {
  if (!m->rw)
    return 0;
  if (cg_flush(m) < 0)
    return -1;
  if (m->fmod && sb_write(m) < 0)
    return -1;
  return 0;
}

static struct ufs2_mount *lock_mount(const struct ufs2_volume *volume) {
  struct ufs2_mount *m = mount_of(volume);

  if (m == 0)
    return 0;
  acquiresleep(&m->lock);
  if (!m->used || m->dev != volume->device.device) {
    releasesleep(&m->lock);
    return 0;
  }
  return m;
}
// TRANSACTIONS
static uchar *tx_depth(struct ufs2_mount *m) {
  struct proc *p = curproc;

  return p != 0 ? &p->fstx[m - mounts] : 0;
}

static uint64_t tx_cost(struct ufs2_mount *m) {
  return ulog_pending(m->log, m->norph) + (uint64_t)m->npending * m->cgsize;
}

static uint64_t tx_limit(struct ufs2_mount *m) {
  uint64_t log = ulog_room(m->log) / 2, mem = bio_capacity() / 4;

  return log < mem ? log : mem;
}

static void log_fail(struct ufs2_mount *m) {
  printf("UFS2: the log of %s cannot be written; it will need fsck\n",
          (char *)SB(m, fsmnt));
  ulog_abandon(m->log);
  m->log = 0;
  needs_fsck(m);
}

// lock held
static void commit_locked(struct ufs2_mount *m) {
  if (!m->txdirty && !m->fmod && !m->cg_dirty)
    return;
  frees_apply(m);
  if (cg_flush(m) < 0 || (m->fmod && sb_write(m) < 0) ||
      ulog_commit(m->log, m->orph, m->norph) < 0) {
    log_fail(m);
    return;
  }
  m->txdirty = 0;
}

static void tx_commit(struct ufs2_mount *m) {
  mtx_enter(&m->txlock);
  m->want++;
  while (m->active > 0)
    msleep(&m->active, &m->txlock, PRIBIO, "ufs2cmt", 0);
  mtx_leave(&m->txlock);
  acquiresleep(&m->lock);
  if (m->used && m->rw && m->log != 0) {
    commit_locked(m);
  } else {
    if (m->used && m->rw)
      frees_apply(m);
    m->txdirty = 0;
  }
  releasesleep(&m->lock);
  mtx_enter(&m->txlock);
  if (--m->want == 0)
    wakeup(&m->want);
  mtx_leave(&m->txlock);
}

static void tx_enter(struct ufs2_mount *m) {
  uchar *depth = tx_depth(m);
  int big;

  if (depth != 0 && (*depth)++ > 0)
    return;
  acquiresleep(&m->lock);
  big = m->used && m->log != 0 && tx_cost(m) >= tx_limit(m);
  releasesleep(&m->lock);
  if (big)
    tx_commit(m);
  mtx_enter(&m->txlock);
  while (m->want > 0)
    msleep(&m->want, &m->txlock, PRIBIO, "ufs2tx", 0);
  m->active++;
  mtx_leave(&m->txlock);
}

static void tx_leave(struct ufs2_mount *m) {
  uchar *depth = tx_depth(m);

  if (depth != 0 && --(*depth) > 0)
    return;
  mtx_enter(&m->txlock);
  if (--m->active == 0 && m->want > 0)
    wakeup(&m->active);
  mtx_leave(&m->txlock);
}

static int tx_full(struct ufs2_mount *m) {
  uchar *depth = tx_depth(m);

  return m->log != 0 && depth != 0 && *depth == 1 && tx_cost(m) >= tx_limit(m);
}

static int tx_restart(struct ufs2_mount *m) {
  uint dev = m->dev;

  releasesleep(&m->lock);
  tx_leave(m);
  tx_commit(m);
  tx_enter(m);
  acquiresleep(&m->lock);
  return m->used && m->dev == dev ? 0 : -1;
}

// nameless and still open
static void orphan_mark(struct ufs2_mount *m, uint ino, int on) {
  uint i;

  if (m->log == 0)
    return;
  for (i = 0; i < m->norph && m->orph[i] != ino; i++)
    ;
  if (on == (i < m->norph))
    return;
  if (!on) {
    m->orph[i] = m->orph[--m->norph];
  } else {
    if (m->norph == m->orphcap) {
      uint cap = m->orphcap == 0 ? 16 : m->orphcap * 2;
      uint32_t *n = malloc(cap * sizeof(*n), M_UFS, M_WAITOK);

      if (m->norph > 0)
        memmove(n, m->orph, m->norph * sizeof(*n));
      if (m->orph != 0)
        free(m->orph, M_UFS);
      m->orph = n;
      m->orphcap = cap;
    }
    m->orph[m->norph++] = ino;
  }
  m->txdirty = 1;
}

static int logger_chan;

void ufs2_logger(void *arg) {
  int i;

  (void)arg;
  for (;;) {
    tsleep(&logger_chan, PRIBIO, "ufs2log", HZ);
    for (i = 0; i < NUFS2; i++)
      if (mounts[i].txdirty || mounts[i].npending > 0)
        tx_commit(&mounts[i]);
  }
}

// THE LOG FILE
static int log_map(struct ufs2_mount *m, uint ino, struct ufs2_inode *ip,
                   struct ulog_place *at) {
  uint64_t lbn, nblk = ip->size >> m->bshift, frag;
  struct ulog_extent *ext;
  uint n = 0;

  if (nblk == 0)
    return -1;
  ext = malloc(nblk * sizeof(*ext), M_UFS, M_WAITOK);
  for (lbn = 0; lbn < nblk; lbn++) {
    uint64_t sector;

    if (bmap(m, ip, lbn, &frag) < 0 || frag == 0 || frag >= m->size) {
      free(ext, M_UFS);
      return -1;
    }
    sector = frag * (m->fsize / DEV_BSIZE);
    if (n > 0 && ext[n - 1].sector + ext[n - 1].len / DEV_BSIZE == sector) {
      ext[n - 1].len += m->bsize;
    } else {
      ext[n].off = lbn << m->bshift;
      ext[n].sector = sector;
      ext[n].len = m->bsize;
      n++;
    }
  }
  memset(at, 0, sizeof(*at));
  at->dev = m->device;
  at->ext = ext;
  at->next = n;
  at->size = ip->size;
  at->ino = ino;
  at->fsid[0] = rd32(SB(m, id));
  at->fsid[1] = rd32(SB(m, id) + 4);
  at->mtime = rd64(SB(m, mtime));
  return 0;
}

static int log_find(struct ufs2_mount *m, struct ulog_place *at) {
  struct ufs2_inode ip;
  uint ino;

  if (dir_lookup(m, UFS2_ROOT_INO, ULOG_NAME, &ino) < 0 ||
      inode_read(m, ino, &ip) < 0)
    return -1;
  if (ip.mode != (UFS2_IFREG | 0400) || ip.uid != 0 || ip.nlink != 1 ||
      (ip.flags & FNU_SF_IMMUTABLE) == 0 ||
      ip.size < ULOG_MIN || ip.size > ULOG_MAX ||
      (ip.size & (m->bsize - 1)) != 0 || log_map(m, ino, &ip, at) < 0) {
    printf("UFS2: /%s is not a log\n", ULOG_NAME);
    return -1;
  }
  m->log_ino = ino;
  return 0;
}

static int log_create(struct ufs2_mount *m, struct ulog_place *at) {
  uint64_t size = m->size * m->fsize / ULOG_SCALE, lbn, fragno;
  struct ufs2_inode ip;
  uint ino, bs;
  int fresh;

  if (size < ULOG_MIN)
    size = ULOG_MIN;
  if (size > ULOG_MAX)
    size = ULOG_MAX;
  size &= ~(uint64_t)(m->bsize - 1);
  if (freespace(m) < (int64_t)(4 * size / m->fsize)) {
    printf("UFS2: %s has no room for a log\n", (char *)SB(m, fsmnt));
    return -1;
  }
  if (inode_alloc(m, UFS2_IFREG | 0400, UFS2_ROOT_INO, &ino) < 0)
    return -1;
  if (inode_read(m, ino, &ip) < 0)
    goto undo;
  ip.uid = ip.gid = 0;
  ip.flags = FNU_SF_IMMUTABLE;
  for (lbn = 0; lbn < size >> m->bshift; lbn++) {
    if (balloc(m, ino, &ip, lbn, m->bsize, &fragno, &bs, &fresh, 0) < 0)
      break;
    ip.size = (lbn + 1) << m->bshift;
  }
  if (ip.size != size || inode_write(m, ino, &ip) < 0 ||
      dir_enter(m, UFS2_ROOT_INO, ULOG_NAME, ino, UFS2_DIRTYPE_REG) < 0)
    goto undo;
  return log_find(m, at);
undo:
  truncate_once(m, ino, &ip, 0);
  inode_free(m, ino);
  printf("UFS2: no log could be made on %s\n", (char *)SB(m, fsmnt));
  return -1;
}

static struct ufs2_mount *lock_mount_tx(const struct ufs2_volume *volume) {
  struct ufs2_mount *m = mount_of(volume);

  if (m == 0)
    return 0;
  tx_enter(m);
  acquiresleep(&m->lock);
  if (!m->used || m->dev != volume->device.device) {
    releasesleep(&m->lock);
    tx_leave(m);
    return 0;
  }
  return m;
}

static void unlock_mount_tx(struct ufs2_mount *m) {
  releasesleep(&m->lock);
  tx_leave(m);
}

// THE PUBLIC FACE

void ufs2_init(void) {
  int i;

  initsleeplock(&ufs2_lock, "ufs2");
  for (i = 0; i < NUFS2; i++) {
    initsleeplock(&mounts[i].lock, "ufs2mount");
    mtx_init(&mounts[i].txlock, "ufs2tx");
    mounts[i].id = (uint)i + 1;
    mounts[i].cgx = -1;
  }
}

// fs_time as found
int64_t ufs2_time(uint dev) {
  int i;

  for (i = 0; i < NUFS2; i++)
    if (mounts[i].used && mounts[i].dev == dev && mounts[i].sb != 0)
      return mounts[i].mounted_time;
  return 0;
}

int ufs2_probe(const struct blockdev *device, struct ufs2_volume *volume) {
  struct ufs2_mount *m = 0, *spare = 0;
  int i;

  if (device == 0 || volume == 0)
    return -1;
  acquiresleep(&ufs2_lock);
  for (i = 0; i < NUFS2; i++) {
    if (mounts[i].used && mounts[i].dev == device->device)
      m = &mounts[i];
    else if (!mounts[i].used && spare == 0)
      spare = &mounts[i];
  }
  if (m != 0 && !m->rw) {
    acquiresleep(&m->lock);
    if (mount_attach(m, device) < 0) {
      releasesleep(&m->lock);
      m = 0;
    } else {
      releasesleep(&m->lock);
    }
  } else if (m == 0) {
    if (spare == 0) {
      releasesleep(&ufs2_lock);
      return -1;
    }
    acquiresleep(&spare->lock);
    if (mount_attach(spare, device) < 0) {
      releasesleep(&spare->lock);
      releasesleep(&ufs2_lock);
      return -1;
    }
    releasesleep(&spare->lock);
    m = spare;
  }
  if (m == 0) {
    releasesleep(&ufs2_lock);
    return -1;
  }
  memset(volume, 0, sizeof(*volume));
  volume->device = *device;
  volume->m = m;
  volume->block_size = m->bsize;
  volume->fragment_size = m->fsize;
  volume->fragments_per_block = m->frag;
  volume->cylinder_groups = m->ncg;
  volume->inodes_per_group = m->ipg;
  volume->fragments_per_group = m->fpg;
  volume->inodes_per_block = m->inopb;
  volume->inode_table_fragment = m->iblkno;
  volume->cylinder_group_block = m->cblkno;
  volume->cylinder_group_size = m->cgsize;
  volume->data_fragments = m->dsize;
  releasesleep(&ufs2_lock);
  return 0;
}

static void orphan_reap(const struct ufs2_volume *volume, uint ino) {
  struct ufs2_inode ip;

  if (ufs2_read_inode(volume, ino, &ip) < 0 || ip.mode == 0 || ip.nlink != 0)
    return;
  if (ufs2_truncate_to(volume, ino, &ip, 0) == 0)
    ufs2_free_inode(volume, ino);
}

int ufs2_mount_ro(const struct ufs2_volume *volume, const char *on) {
  struct ufs2_mount *m;
  struct ulog_overlay *o = 0;
  struct ulog_place at;
  struct blockdev dev;
  int r = -1;

  if ((m = lock_mount(volume)) == 0)
    return -1;
  if (mount_of(volume) == 0)
    goto out;
  r = 0;
  if (m->rw || m->overlay != 0 || m->sb[UFS2_SB_clean] != 0 ||
      blockdev_memory(m->device.device) || log_find(m, &at) < 0)
    goto out;
  o = ulog_overlay_open(&at);
  free(at.ext, M_UFS);
  if (o == 0)
    goto out;
  dev = m->device;
  bio_forget_dev(m->dev);
  if (mount_attach(m, &dev) < 0) {
    printf("UFS2: %s cannot be read through its log\n", on);
    ulog_overlay_close(o);
    bio_forget_dev(dev.device);
    r = -1;
    goto out;
  }
  m->overlay = o;
out:
  releasesleep(&m->lock);
  return r;
}

int ufs2_mount_rw(const struct ufs2_volume *volume, const char *on) {
  struct ufs2_mount *m;
  struct ulog_place at;
  struct fnu_timespec now;
  struct blockdev dev;
  uint32_t flags, *orphans = 0;
  uint norphans = 0, i;
  int r = -1, found, memory;

  if ((m = lock_mount(volume)) == 0)
    return -1;
  if (mount_of(volume) == 0)
    goto out;
  if (m->rw) {
    r = 0;
    goto out;
  }
  flags = rd32(SB(m, flags));
  if (flags & UFS2_FS_NEEDSFSCK) {
    printf("UFS2: %s needs fsck; mounted read-only\n", on);
    goto out;
  }
  if (flags & (UFS2_FS_SUJ | UFS2_FS_GJOURNAL)) {
    printf("UFS2: %s carries a journal this kernel does not keep; mounted read-only\n", on);
    goto out;
  }
  memory = blockdev_memory(m->device.device);
  found = !memory && log_find(m, &at) == 0;
  if (m->sb[UFS2_SB_clean] == 0) {
    int got = found ? ulog_replay(&at, &orphans, &norphans) : ULOG_STALE;

    if (found)
      free(at.ext, M_UFS);
    if (got != ULOG_CLEAN && got != ULOG_REPLAYED) {
      printf("UFS2: %s was not properly dismounted; mounted read-only\n", on);
      goto out;
    }
    dev = m->device;
    if (mount_attach(m, &dev) < 0) {
      printf("UFS2: %s cannot be read after its log\n", on);
      goto out;
    }
    found = log_find(m, &at) == 0;
  }
  flags = rd32(SB(m, flags));
  flags &= ~(UFS2_FS_METACKHASH | UFS2_FS_DOSOFTDEP);
  flags |= UFS2_FS_UNCLEAN;
  wr32(SB(m, flags), flags);
  wr32(SB(m, metackhash), 0);
  time_now(&now);
  wr64(SB(m, mtime), (uint64_t)now.sec);
  memset(SB(m, fsmnt), 0, MAXMNTLEN);
  strlcpy((char *)SB(m, fsmnt), (char *)on, MAXMNTLEN);
  m->sb[UFS2_SB_clean] = 0;
  m->sb[UFS2_SB_ronly] = 0;
  m->rw = 1;
  sb_touch(m);
  if ((r = sb_write(m)) < 0) {
    m->rw = 0;
    if (found)
      free(at.ext, M_UFS);
    goto out;
  }
  if (!memory && (found || log_create(m, &at) == 0)) {
    at.mtime = rd64(SB(m, mtime));
    if (cg_flush(m) == 0 && (!m->fmod || sb_write(m) == 0)) {
      bio_flush_device(m->dev);
      blockdev_flush(m->dev);
      m->pending = malloc(m->ncg * sizeof(*m->pending), M_UFS, M_WAITOK | M_ZERO);
      m->log = ulog_open(&at);
    }
    if (m->log == 0)
      printf("UFS2: %s keeps no log\n", on);
    free(at.ext, M_UFS);
  }
out:
  releasesleep(&m->lock);
  for (i = 0; i < norphans; i++)
    orphan_reap(volume, orphans[i]);
  if (orphans != 0)
    free(orphans, M_UFS);
  return r;
}

// remount: fsck may have changed things, read again
int ufs2_upgrade(const struct ufs2_volume *volume, const char *on) {
  struct ufs2_mount *m;
  struct blockdev dev;
  int r;

  if ((m = lock_mount(volume)) == 0)
    return -1;
  if (mount_of(volume) == 0) {
    releasesleep(&m->lock);
    return -1;
  }
  if (m->rw) {
    releasesleep(&m->lock);
    return 0;
  }
  dev = m->device;
  bio_forget_dev(dev.device);
  r = mount_attach(m, &dev);
  releasesleep(&m->lock);
  if (r < 0) {
    printf("UFS2: %s could not be read again\n", on);
    return -1;
  }
  return ufs2_mount_rw(volume, on) < 0 || !m->rw ? -1 : 0;
}

// ... и обратно. хуйня, но работает
int ufs2_downgrade(const struct ufs2_volume *volume) {
  struct ufs2_mount *m = mount_of(volume);
  uint32_t flags;
  int r = -1, i;

  if (m != 0 && m->log != 0)
    tx_commit(m);
  if ((m = lock_mount(volume)) == 0)
    return -1;
  if (mount_of(volume) == 0)
    goto out;
  if (!m->rw) {
    r = 0;
    goto out;
  }
  if (m->norph > 0)
    goto out;
  if (m->log != 0) {
    bio_flush_device(m->dev);
    blockdev_flush(m->dev);
    ulog_close(m->log);
    m->log = 0;
  }
  if (sync_locked(m) < 0)
    goto out;
  bio_flush_device(m->dev);
  flags = rd32(SB(m, flags)) & ~UFS2_FS_UNCLEAN;
  wr32(SB(m, flags), flags);
  m->sb[UFS2_SB_clean] = (flags & UFS2_FS_NEEDSFSCK) ? 0 : 1;
  m->sb[UFS2_SB_ronly] = 1;
  sb_touch(m);
  if (sb_write(m) < 0)
    goto out;
  bio_flush_device(m->dev);
  blockdev_flush(m->dev);
  for (i = 0; m->pending != 0 && i < (int)m->ncg; i++)
    if (m->pending[i] != 0)
      free(m->pending[i], M_UFS);
  if (m->pending != 0)
    free(m->pending, M_UFS);
  m->pending = 0;
  m->npending = 0;
  m->rw = 0;
  r = 0;
out:
  releasesleep(&m->lock);
  return r;
}

void ufs2_sync(const struct ufs2_volume *volume) {
  struct ufs2_mount *m = mount_of(volume);

  if (m != 0 && m->log != 0) {
    tx_commit(m);
    return;
  }
  if ((m = lock_mount(volume)) == 0)
    return;
  sync_locked(m);
  releasesleep(&m->lock);
}

void ufs2_unmount(const struct ufs2_volume *volume) {
  struct ufs2_mount *m = mount_of(volume);
  int keep = 0;

  if (m != 0 && m->log != 0)
    tx_commit(m);
  if ((m = lock_mount(volume)) == 0)
    return;
  if (m->rw && m->log != 0) {
    bio_flush_device(m->dev);
    blockdev_flush(m->dev);
    keep = m->norph > 0;
    if (keep)
      ulog_abandon(m->log);
    else
      ulog_close(m->log);
    m->log = 0;
  }
  if (m->rw && sync_locked(m) == 0) {
    uint32_t flags = rd32(SB(m, flags));
    bio_flush_device(m->dev);
    if (!keep)
      flags &= ~UFS2_FS_UNCLEAN;
    wr32(SB(m, flags), flags);
    m->sb[UFS2_SB_clean] = (flags & UFS2_FS_NEEDSFSCK) || keep ? 0 : 1;
    sb_touch(m);
    if (sb_write(m) == 0)
      bio_flush_device(m->dev);
    blockdev_flush(m->dev);
  }
  releasesleep(&m->lock);
  acquiresleep(&ufs2_lock);
  acquiresleep(&m->lock);
  if (m->used && m->dev == volume->device.device)
    mount_release(m);
  releasesleep(&m->lock);
  releasesleep(&ufs2_lock);
}

void ufs2_begin(const struct ufs2_volume *volume) {
  struct ufs2_mount *m = mount_of(volume);

  if (m != 0)
    tx_enter(m);
}

void ufs2_end(const struct ufs2_volume *volume) {
  struct ufs2_mount *m = mount_of(volume);

  if (m != 0)
    tx_leave(m);
}

// frees the log holds back, let go
int ufs2_request_cleanup(const struct ufs2_volume *volume) {
  struct ufs2_mount *m = mount_of(volume);
  uchar *depth;
  int pending;

  if (m == 0 || m->log == 0 || ((depth = tx_depth(m)) != 0 && *depth > 0))
    return 0;
  acquiresleep(&m->lock);
  pending = m->used && m->npending > 0;
  releasesleep(&m->lock);
  if (pending)
    tx_commit(m);
  return pending;
}

int ufs2_statfs(const struct ufs2_volume *volume, struct fnu_fsstat *st) {
  struct ufs2_mount *m;

  if ((m = lock_mount(volume)) == 0)
    return -1;
  st->bsize = m->fsize;
  st->iosize = m->bsize;
  st->namemax = UFS2_MAX_NAME;
  st->blocks = m->dsize;
  st->bfree = (rd64(SB(m, cstotal) + CST_NBFREE) << m->fragshift) +
              rd64(SB(m, cstotal) + CST_NFFREE);
  st->bavail = freespace(m);
  st->files = (uint64_t)m->ncg * m->ipg - UFS2_ROOT_INO;
  st->ffree = rd64(SB(m, cstotal) + CST_NIFREE);
  st->favail = (int64_t)st->ffree;
  releasesleep(&m->lock);
  return 0;
}

int ufs2_read_inode(const struct ufs2_volume *volume, uint inode_number,
                    struct ufs2_inode *inode) {
  struct ufs2_mount *m;
  int r = -1;

  if (inode == 0)
    return -1;
  if ((m = lock_mount(volume)) == 0)
    return -1;
  if (mount_of(volume) != 0)
    r = inode_read(m, inode_number, inode);
  releasesleep(&m->lock);
  return r;
}

int ufs2_write_inode(const struct ufs2_volume *volume, uint inode_number,
                     const struct ufs2_inode *inode) {
  struct ufs2_mount *m;
  int r = -1;

  if (inode == 0)
    return -1;
  if ((m = lock_mount_tx(volume)) == 0)
    return -1;
  if (mount_of(volume) != 0 && inode_number != m->log_ino)
    r = inode_write(m, inode_number, inode);
  unlock_mount_tx(m);
  return r;
}

int ufs2_lookup(const struct ufs2_volume *volume, uint directory_inode,
                const char *name, uint *inode_number) {
  struct ufs2_mount *m;
  int r = -1;

  if (inode_number == 0)
    return -1;
  if ((m = lock_mount(volume)) == 0)
    return -1;
  if (mount_of(volume) != 0)
    r = dir_lookup(m, directory_inode, name, inode_number);
  releasesleep(&m->lock);
  return r;
}

int ufs2_read_uio(const struct ufs2_volume *volume, uint inode_number,
                  const struct ufs2_inode *inode, struct uio *uio) {
  struct ufs2_mount *m;
  int r = -1;

  if (inode == 0)
    return -1;
  if ((m = lock_mount(volume)) == 0)
    return -1;
  if (mount_of(volume) != 0)
    r = inline_link(m, inode)
            ? -1
            : read_bytes_uio(m, inode_number, inode, uio->uio_offset, uio,
                             (uint)uio->uio_resid);
  releasesleep(&m->lock);
  return r;
}

int ufs2_write_uio(const struct ufs2_volume *volume, uint inode_number,
                   struct ufs2_inode *inode, struct uio *uio) {
  struct ufs2_mount *m;
  uint64_t oblocks;
  size_t want = uio->uio_resid;
  int r = -FNU_ERR_INVALID;

  if (inode == 0)
    return -FNU_ERR_INVALID;
  if ((m = lock_mount_tx(volume)) == 0)
    return -FNU_ERR_NODEVICE;
  if (inode_number == m->log_ino)
    r = -FNU_ERR_NOTPERMITTED;
  else if (mount_of(volume) != 0 && !inline_link(m, inode)) {
    oblocks = inode->blocks;
    r = write_bytes_uio(m, inode_number, inode, uio->uio_offset, uio,
                        (uint)want);
    if (uio->uio_resid < want)
      stamp(inode, 0, 1, 1);
    if ((uio->uio_resid < want || inode->blocks != oblocks) &&
        inode_write(m, inode_number, inode) < 0)
      r = -FNU_ERR_IO;
  }
  unlock_mount_tx(m);
  return r < 0 ? r : (int)want;
}


int ufs2_truncate(const struct ufs2_volume *volume, uint inode_number,
                  struct ufs2_inode *inode) {
  return ufs2_truncate_to(volume, inode_number, inode, 0);
}

int ufs2_truncate_to(const struct ufs2_volume *volume, uint inode_number,
                     struct ufs2_inode *inode, uint64_t length) {
  struct ufs2_mount *m;
  int r = -FNU_ERR_IO;

  if (inode == 0)
    return -FNU_ERR_INVALID;
  if ((m = lock_mount_tx(volume)) == 0)
    return -FNU_ERR_IO;
  if (mount_of(volume) != 0 && inode_number != m->log_ino)
    r = truncate_to(m, inode_number, inode, length);
  unlock_mount_tx(m);
  return r;
}

int ufs2_alloc_inode(const struct ufs2_volume *volume, uint16_t mode,
                     uint parent_inode, uint *inode_number) {
  struct ufs2_mount *m;
  int r = -1;

  if (inode_number == 0 ||
      ((mode & UFS2_IFMT) != UFS2_IFREG && (mode & UFS2_IFMT) != UFS2_IFDIR &&
       (mode & UFS2_IFMT) != UFS2_IFLNK && (mode & UFS2_IFMT) != UFS2_IFIFO &&
       (mode & UFS2_IFMT) != UFS2_IFSOCK))
    return -1;
  if ((m = lock_mount_tx(volume)) == 0)
    return -1;
  if (mount_of(volume) != 0)
    r = inode_alloc(m, mode, parent_inode, inode_number);
  unlock_mount_tx(m);
  return r;
}

int ufs2_free_inode(const struct ufs2_volume *volume, uint inode_number) {
  struct ufs2_mount *m;
  int r = -1;

  if ((m = lock_mount_tx(volume)) == 0)
    return -1;
  if (mount_of(volume) != 0 && inode_number != m->log_ino)
    r = inode_free(m, inode_number);
  unlock_mount_tx(m);
  return r;
}

int ufs2_make_directory(const struct ufs2_volume *volume, uint inode_number,
                        uint parent_inode) {
  struct ufs2_mount *m;
  struct ufs2_inode dir;
  uchar chunk[DIRBLKSIZ];
  int r = -1;

  if ((m = lock_mount_tx(volume)) == 0)
    return -1;
  if (mount_of(volume) == 0 || inode_read(m, inode_number, &dir) < 0 ||
      (dir.mode & UFS2_IFMT) != UFS2_IFDIR || dir.size != 0)
    goto out;
  memset(chunk, 0, sizeof(chunk));
  wr32(chunk, inode_number);
  wr16(chunk + 4, 12);
  chunk[6] = UFS2_DIRTYPE_DIR;
  chunk[7] = 1;
  chunk[8] = '.';
  wr32(chunk + 12, parent_inode);
  wr16(chunk + 16, DIRBLKSIZ - 12);
  chunk[18] = UFS2_DIRTYPE_DIR;
  chunk[19] = 2;
  chunk[20] = '.';
  chunk[21] = '.';
  if (write_bytes(m, inode_number, &dir, 0, chunk, DIRBLKSIZ) < 0)
    goto out;
  dir.nlink = 2;
  stamp(&dir, 0, 1, 1);
  r = inode_write(m, inode_number, &dir);
out:
  unlock_mount_tx(m);
  return r;
}

int ufs2_dirlink(const struct ufs2_volume *volume, uint directory_inode,
                 const char *name, uint inode_number, uchar type) {
  struct ufs2_mount *m;
  int r = -1;

  if (type != UFS2_DIRTYPE_REG && type != UFS2_DIRTYPE_DIR &&
      type != UFS2_DIRTYPE_LNK && type != UFS2_DIRTYPE_FIFO &&
      type != UFS2_DIRTYPE_SOCK)
    return -1;
  if ((m = lock_mount_tx(volume)) == 0)
    return -1;
  if (mount_of(volume) != 0)
    r = dir_enter(m, directory_inode, name, inode_number, type);
  unlock_mount_tx(m);
  return r;
}

int ufs2_dirunlink(const struct ufs2_volume *volume, uint directory_inode,
                   const char *name, uint *inode_number) {
  struct ufs2_mount *m;
  int r = -1;

  if ((m = lock_mount_tx(volume)) == 0)
    return -1;
  if (mount_of(volume) != 0)
    r = dir_remove(m, directory_inode, name, inode_number);
  unlock_mount_tx(m);
  return r;
}

int ufs2_readdir(const struct ufs2_volume *volume, uint directory_inode,
                 uint index, uint *inode_number, char *name, uint name_size) {
  struct ufs2_mount *m;
  struct ufs2_inode dir;
  uchar chunk[DIRBLKSIZ];
  uint off, n, seen = 0;
  uint64_t pos;
  int r = -1;

  if (inode_number == 0 || name == 0 || name_size == 0)
    return -1;
  if ((m = lock_mount(volume)) == 0)
    return -1;
  if (mount_of(volume) == 0 || inode_read(m, directory_inode, &dir) < 0 ||
      (dir.mode & UFS2_IFMT) != UFS2_IFDIR)
    goto out;
  for (pos = 0; pos < dir.size && r < 0; pos += DIRBLKSIZ) {
    if (dir_chunk(m, directory_inode, &dir, pos, chunk, &n) < 0)
      break;
    for (off = 0; record_ok(chunk, off, n); off += rd16(chunk + off + 4)) {
      uint namlen = chunk[off + 7], copy;
      if (rd32(chunk + off) == 0 || seen++ != index)
        continue;
      copy = namlen < name_size - 1 ? namlen : name_size - 1;
      memmove(name, chunk + off + UFS2_DIR_HEADER_SIZE, copy);
      name[copy] = 0;
      *inode_number = rd32(chunk + off);
      r = 0;
      break;
    }
  }
out:
  releasesleep(&m->lock);
  return r;
}

int ufs2_readlink(const struct ufs2_volume *volume, uint inode_number,
                  const struct ufs2_inode *inode, char *buf, int size) {
  struct ufs2_mount *m;
  uchar raw[UFS2_DINODE_SIZE];
  uint n;
  int r = -1;

  if (inode == 0 || buf == 0 || size <= 0 || (inode->mode & UFS2_IFMT) != UFS2_IFLNK)
    return -1;
  n = (uint)inode->size;
  if (n >= (uint)size)
    return -1;
  if ((m = lock_mount(volume)) == 0)
    return -1;
  if (mount_of(volume) == 0)
    goto out;
  if (inline_link(m, inode)) {
    if (dinode_read(m, inode_number, raw) < 0)
      goto out;
    memmove(buf, raw + UFS2_DINODE_DIRECT_OFFSET, n);
  } else if (read_bytes(m, inode_number, inode, 0, (uchar *)buf, n) < 0) {
    goto out;
  }
  buf[n] = 0;
  r = (int)n;
out:
  releasesleep(&m->lock);
  return r;
}

int ufs2_symlink(const struct ufs2_volume *volume, uint inode_number,
                 struct ufs2_inode *inode, const char *target) {
  struct ufs2_mount *m;
  uchar raw[UFS2_DINODE_SIZE];
  uint n;
  int r = -1;

  if (inode == 0 || target == 0 || (inode->mode & UFS2_IFMT) != UFS2_IFLNK ||
      inode->size != 0)
    return -1;
  n = (uint)strlen(target);
  if ((m = lock_mount_tx(volume)) == 0)
    return -1;
  if (mount_of(volume) == 0 || !m->rw || n == 0)
    goto out;
  if (n < m->maxsymlinklen) {
    if (dinode_read(m, inode_number, raw) < 0)
      goto out;
    inode->size = n;
    inode->blocks = 0;
    stamp(inode, 0, 1, 1);
    dinode_pack(m, inode, raw);
    memset(raw + UFS2_DINODE_DIRECT_OFFSET, 0, NDADDR * 8 + NIADDR * 8);
    memmove(raw + UFS2_DINODE_DIRECT_OFFSET, target, n);
    r = dinode_write(m, inode_number, raw);
    goto out;
  }
  if (write_bytes(m, inode_number, inode, 0, (const uchar *)target, n) < 0)
    goto out;
  stamp(inode, 0, 1, 1);
  r = inode_write(m, inode_number, inode);
out:
  unlock_mount_tx(m);
  return r;
}

int ufs2_dir_empty(const struct ufs2_volume *volume, uint directory_inode) {
  struct ufs2_mount *m;
  struct ufs2_inode dir;
  uchar chunk[DIRBLKSIZ];
  uint off, n;
  uint64_t pos;
  int r = 0;

  if ((m = lock_mount(volume)) == 0)
    return -1;
  if (mount_of(volume) == 0 || inode_read(m, directory_inode, &dir) < 0 ||
      (dir.mode & UFS2_IFMT) != UFS2_IFDIR)
    goto out;
  r = 1;
  for (pos = 0; pos < dir.size && r; pos += DIRBLKSIZ) {
    if (dir_chunk(m, directory_inode, &dir, pos, chunk, &n) < 0) {
      r = 0;
      break;
    }
    for (off = 0; record_ok(chunk, off, n); off += rd16(chunk + off + 4)) {
      uint namlen = chunk[off + 7];
      const uchar *nm = chunk + off + UFS2_DIR_HEADER_SIZE;
      if (rd32(chunk + off) == 0)
        continue;
      if ((namlen == 1 && nm[0] == '.') || (namlen == 2 && nm[0] == '.' && nm[1] == '.'))
        continue;
      r = 0;
      break;
    }
  }
out:
  releasesleep(&m->lock);
  return r;
}
