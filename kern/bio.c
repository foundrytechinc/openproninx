// BUFFER CACHE

#include "buf.h"
#include "blockdev.h"
#include "defs.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "sleeplock.h"
#include "mutex.h"

int nbuf;
struct {
  struct mutex lock;
  struct buf *buf[NBUF];
  struct buf *hash[BUFHASH];

  struct buf head;
  struct buf age;
  struct buf dirty;
  int ndirty;
  uint seq;
} bcache;

static uchar *cluster;
static uint clusterbytes;
static struct buf clusterbuf;

// BUFFER MEMORY
#define BUF_MINORDER 9                          // 512 bytes
#define BUF_MAXORDER 16                         // 64 KB
#define BUF_ORDERS (BUF_MAXORDER - BUF_MINORDER + 1)

static void hash_remove(struct buf *b);
static int bio_push_oldest(void);
static void bufmem_wake(void);

static uchar *bufregion;
static uint64_t bufregion_bytes;
static uint64_t bufmem_gen;
static uchar *buddy[BUF_ORDERS];
static int bufwaiters;

static int order_of(uint size) {
  int o = BUF_MINORDER;

  while (((uint)1 << o) < size && o < BUF_MAXORDER)
    o++;
  return o;
}

static void buddy_push(int o, uchar *p) {
  *(uchar **)p = buddy[o - BUF_MINORDER];
  buddy[o - BUF_MINORDER] = p;
}

static uchar *buddy_pop(int o) {
  uchar *p = buddy[o - BUF_MINORDER];

  if (p != 0)
    buddy[o - BUF_MINORDER] = *(uchar **)p;
  return p;
}

static int buddy_take(int o, uchar *p) {
  uchar **pp;

  for (pp = &buddy[o - BUF_MINORDER]; *pp != 0; pp = (uchar **)*pp)
    if (*pp == p) {
      *pp = *(uchar **)p;
      return 1;
    }
  return 0;
}

// bcache.lock held
static uchar *buddy_alloc(int o) {
  uchar *p;
  int big;

  if ((p = buddy_pop(o)) != 0)
    return p;
  for (big = o + 1; big <= BUF_MAXORDER; big++)
    if ((p = buddy_pop(big)) != 0) {
      while (--big >= o)
        buddy_push(big, p + ((uintptr_t)1 << big));
      return p;
    }
  return 0;
}

// bcache.lock held
static void buddy_release(int o, uchar *p) {
  while (o < BUF_MAXORDER) {
    uintptr_t off = (uintptr_t)(p - bufregion);
    uchar *mate = bufregion + (off ^ ((uintptr_t)1 << o));

    if ((uint64_t)(mate - bufregion) + ((uint64_t)1 << o) > bufregion_bytes)
      break;
    if (!buddy_take(o, mate))
      break;
    p = p < mate ? p : mate;
    o++;
  }
  buddy_push(o, p);
  bufmem_gen++;
}

// bcache.lock held
static uint64_t buffers_discard(struct buf *keep, uint64_t want) {
  struct buf *lists[2], *head, *b, *next;
  uint64_t got = 0;
  int i;

  lists[0] = &bcache.age;
  lists[1] = &bcache.head;
  for (i = 0; i < 2 && got < want; i++) {
    head = lists[i];
    for (b = head->next; b != head && got < want; b = next) {
      next = b->next;
      if (b == keep || b->refcnt != 0 || b->data == 0 ||
          (b->flags & (B_DIRTY | B_DELWRI)) != 0)
        continue;
      hash_remove(b);
      buddy_release(order_of(b->bufsize), b->data);
      got += b->bufsize;
      b->data = 0;
      b->bufsize = 0;
      b->flags &= ~B_VALID;
    }
  }
  return got;
}

// HIBERNATE
void bio_hibernate(void) {
  int o;
  uchar *p;

  mtx_enter(&bcache.lock);
  buffers_discard(0, bufregion_bytes);
  for (o = BUF_MINORDER; o <= BUF_MAXORDER; o++)
    for (p = buddy[o - BUF_MINORDER]; p != 0; p = *(uchar **)p)
      memset(p + sizeof(uchar *), 0, ((uint64_t)1 << o) - sizeof(uchar *));
  mtx_leave(&bcache.lock);
}

static uchar *bufmem_get(struct buf *b, uint size, uint64_t *gen) {
  int o = order_of(size);
  uchar *p;

  mtx_enter(&bcache.lock);
  if ((p = buddy_alloc(o)) == 0) {
    buffers_discard(b, (uint64_t)1 << o);
    p = buddy_alloc(o);
  }
  *gen = bufmem_gen;
  mtx_leave(&bcache.lock);
  return p;
}

static void bufmem_put(uchar *p, uint size) {
  mtx_enter(&bcache.lock);
  buddy_release(order_of(size), p);
  bufmem_wake();
  mtx_leave(&bcache.lock);
}

static void bufwait(uint64_t gen) {
  if (bio_push_oldest() == 0)
    return;
  mtx_enter(&bcache.lock);
  if (bufmem_gen == gen) {
    bufwaiters++;
    msleep(&bufregion_bytes, &bcache.lock, PRIBIO, "bio", 0);
    bufwaiters--;
  }
  mtx_leave(&bcache.lock);
}

// bcache.lock held
static void bufmem_wake(void) {
  if (bufwaiters > 0)
    wakeup(&bufregion_bytes);
}

static int allocbuf(struct buf *b, uint size, int keep, uint64_t *gen) {
  uchar *mem;

  if (size == 0)
    size = BSIZE;
  if (b->bufsize == size)
    return 0;
  if ((mem = bufmem_get(b, size, gen)) == 0)
    return -1;
  if (b->data != 0) {
    if (keep)
      memmove(mem, b->data, b->bufsize < size ? b->bufsize : size);
    bufmem_put(b->data, b->bufsize);
  }
  b->data = mem;
  b->bufsize = size;
  if (!keep)
    b->flags &= ~B_VALID;
  return 0;
}

static void freelist_insert(struct buf *b) {
  b->next = bcache.head.next;
  b->prev = &bcache.head;
  bcache.head.next->prev = b;
  bcache.head.next = b;
}

static void agelist_append(struct buf *b) {
  b->next = &bcache.age;
  b->prev = bcache.age.prev;
  bcache.age.prev->next = b;
  bcache.age.prev = b;
}

static void freelist_remove(struct buf *b) {
  b->next->prev = b->prev;
  b->prev->next = b->next;
  b->next = b->prev = 0;
}

static void dirty_insert(struct buf *b) {
  b->dirtyseq = ++bcache.seq;
  b->dnext = &bcache.dirty;
  b->dprev = bcache.dirty.dprev;
  bcache.dirty.dprev->dnext = b;
  bcache.dirty.dprev = b;
  if ((b->flags & B_MARKER) == 0)
    bcache.ndirty++;
}

static void dirty_remove(struct buf *b) {
  b->dnext->dprev = b->dprev;
  b->dprev->dnext = b->dnext;
  b->dnext = b->dprev = 0;
  if ((b->flags & B_MARKER) == 0)
    bcache.ndirty--;
}

static void marker_advance(struct buf *m) {
  struct buf *b = m->dnext;

  m->dnext->dprev = m->dprev;
  m->dprev->dnext = m->dnext;
  m->dprev = b;
  m->dnext = b->dnext;
  b->dnext->dprev = m;
  b->dnext = m;
}

// PINNED BUFFERS
static void pin_unlink(struct buf *b) {
  struct bpinlist *l = b->pinlist;

  if (l == 0)
    return;
  if (b->pprev != 0)
    b->pprev->pnext = b->pnext;
  else
    l->first = b->pnext;
  if (b->pnext != 0)
    b->pnext->pprev = b->pprev;
  l->count--;
  l->bytes -= b->bufsize;
  b->pinlist = 0;
  b->pnext = b->pprev = 0;
  b->flags &= ~B_PINNED;
}

// b is locked
void bpin(struct buf *b, struct bpinlist *l) {
  if (!holdingsleep(&b->lock))
    panic("bpin");
  mtx_enter(&bcache.lock);
  if (b->pinlist != l) {
    if (b->pinlist != 0)
      panic("bpin: held by another log");
    b->pinlist = l;
    b->pprev = 0;
    b->pnext = l->first;
    if (l->first != 0)
      l->first->pprev = b;
    l->first = b;
    l->count++;
    l->bytes += b->bufsize;
  }
  b->flags |= B_PINNED;
  mtx_leave(&bcache.lock);
}

// b is locked
void bunpin(struct buf *b) {
  if (!holdingsleep(&b->lock))
    panic("bunpin");
  mtx_enter(&bcache.lock);
  pin_unlink(b);
  mtx_leave(&bcache.lock);
}

void binit(void) {
  struct buf *b = 0;
  int i, hdrs = PGSIZE / sizeof(struct buf);

  mtx_init(&bcache.lock, "bcache");
  initsleeplock(&clusterbuf.lock, "cluster");
  clusterbytes = MAXPHYS;
  if ((cluster = (uchar *)contig_alloc(MAXPHYS)) == 0) {
    cluster = (uchar *)page_alloc();
    clusterbytes = PGSIZE;
  }

  bufmem_region((char **)&bufregion, &bufregion_bytes);
  if (bufregion == 0) {
    bufregion_bytes = 0;
    panic("binit: no buffer memory");
  }
  {
    uint64_t at = 0;
    int o;

    for (o = BUF_MAXORDER; o >= BUF_MINORDER; o--)
      while (at + ((uint64_t)1 << o) <= bufregion_bytes &&
             (at & (((uint64_t)1 << o) - 1)) == 0) {
        buddy_push(o, bufregion + at);
        at += (uint64_t)1 << o;
      }
  }

  nbuf = NBUF;
  while (nbuf > MINBUF && (uint64_t)nbuf * BSIZE > bufregion_bytes)
    nbuf /= 2;

  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  bcache.age.prev = &bcache.age;
  bcache.age.next = &bcache.age;
  bcache.dirty.dprev = &bcache.dirty;
  bcache.dirty.dnext = &bcache.dirty;
  bcache.dirty.flags = B_MARKER;
  for (i = 0; i < nbuf; i++) {
    if (i % hdrs == 0) {
      if ((b = (struct buf *)page_alloc()) == 0)
        break;
      memset(b, 0, PGSIZE);
    } else
      b++;
    b->data = 0;
    b->bufsize = 0;
    b->flags = 0;
    bcache.buf[i] = b;
    initsleeplock(&b->lock, "buffer");
    freelist_insert(b);
  }
  nbuf = i;
  printf("BIO: %d buffers over %d MB of memory of its own\n", nbuf,
          (int)(bufregion_bytes / (1024 * 1024)));
}

static struct buf **bucket(uint dev, uint64_t owner, uint64_t lblkno) {
  uint64_t h = (lblkno ^ (owner * 0xD6E8FEB86659FD93ull)) *
               0x9E3779B97F4A7C15ull;

  return &bcache.hash[(dev * 2654435761u ^ (uint)(h >> 32) ^ (uint)h) % BUFHASH];
}

static struct buf *hash_find(uint dev, uint64_t owner, uint64_t lblkno) {
  struct buf *b;

  for (b = *bucket(dev, owner, lblkno); b != 0; b = b->hnext)
    if (b->dev == dev && b->owner == owner && b->lblkno == lblkno)
      return b;
  return 0;
}

static void hash_remove(struct buf *b) {
  struct buf **pp;

  for (pp = bucket(b->dev, b->owner, b->lblkno); *pp != 0; pp = &(*pp)->hnext)
    if (*pp == b) {
      *pp = b->hnext;
      break;
    }
  b->hnext = 0;
  b->dev = 0;
  b->owner = 0;
  b->lblkno = 0;
  b->blockno = 0;
}

static void hash_move(struct buf *b, uint dev, uint64_t owner,
                      uint64_t lblkno, uint64_t blockno) {
  struct buf **pp;

  hash_remove(b);
  b->dev = dev;
  b->owner = owner;
  b->lblkno = lblkno;
  b->blockno = blockno;
  pp = bucket(dev, owner, lblkno);
  b->hnext = *pp;
  *pp = b;
}

// bcache.lock held
static struct buf *recycle(uint dev, uint64_t owner, uint64_t lblkno,
                           uint64_t blockno) {
  struct buf *b = bcache.age.next;

  if (b == &bcache.age)
    b = bcache.head.prev;
  if (b == &bcache.head)
    return 0;
  freelist_remove(b);
  hash_move(b, dev, owner, lblkno, blockno);
  b->flags = 0;
  b->addr = 0;
  b->bcount = 0;
  b->dirtyat = 0;
  b->refcnt = 1;
  return b;
}

static int bio_push_oldest(void);
static struct buf *bdirty_try(uint dev, uint64_t blockno);
static uint cluster_size(uint dev, uint64_t blockno);
static uint ra_want(uint dev, uint64_t blockno);

static struct buf *bget_size(uint dev, uint64_t blockno, uint size) {
  struct buf *b;
  uint64_t gen = 0;

again:
  mtx_enter(&bcache.lock);

  if ((b = hash_find(dev, 0, blockno)) != 0) {
    if (b->next != 0)
      freelist_remove(b);
    b->refcnt++;
    mtx_leave(&bcache.lock);
    acquiresleep(&b->lock);
    if (b->bufsize != size && size != 0) {
      if (b->flags & B_PINNED)
        panic("bget: a pinned block asked for at another size");
      if (b->flags & (B_DIRTY | B_DELWRI))
        bwrite(b);
      if (allocbuf(b, size, 0, &gen) < 0) {
        brelse(b);
        bufwait(gen);
        goto again;
      }
    }
    return b;
  }

  if ((b = recycle(dev, 0, blockno, blockno)) != 0) {
    mtx_leave(&bcache.lock);
    acquiresleep(&b->lock);
    if (size != 0 && b->bufsize != size && allocbuf(b, size, 0, &gen) < 0) {
      brelse(b);
      bufwait(gen);
      goto again;
    }
    return b;
  }
  mtx_leave(&bcache.lock);

  if (bio_push_oldest() == 0)
    goto again;
  panic("bget: no buffers");
}

static struct buf *bget(uint dev, uint64_t blockno) {
  return bget_size(dev, blockno, BSIZE);
}

static int bio_flush_dev(uint dev, int age) {
  struct buf marker, *b;
  uint64_t now = ticks;
  int written = 0;
  uint bound;

  memset(&marker, 0, sizeof(marker));
  marker.flags = B_MARKER;
  mtx_enter(&bcache.lock);
  bound = bcache.seq;
  marker.dnext = bcache.dirty.dnext;
  marker.dprev = &bcache.dirty;
  bcache.dirty.dnext->dprev = &marker;
  bcache.dirty.dnext = &marker;
  while ((b = marker.dnext) != &bcache.dirty && (int)(b->dirtyseq - bound) <= 0) {
    marker_advance(&marker);
    if ((b->flags & B_MARKER) != 0 || (dev != 0 && b->dev != dev))
      continue;
    if (age > 0 && now - b->dirtyat < (uint64_t)age)
      continue;
    b->refcnt++;
    mtx_leave(&bcache.lock);

    acquiresleep(&b->lock);
    if ((b->flags & (B_DELWRI | B_PINNED)) == B_DELWRI) {
      cluster_write(b);
      written++;
    }
    brelse(b);

    mtx_enter(&bcache.lock);
  }
  dirty_remove(&marker);
  mtx_leave(&bcache.lock);
  return written;
}

int bio_flush(void) { return bio_flush_dev(0, 0); }

int bio_flush_device(uint dev) { return bio_flush_dev(dev, 0); }

// THE SYNCER

#define SYNCER_DELAY 30

static int syncer_chan;

void bio_syncer(void *arg __unused) {
  for (;;) {
    tsleep(&syncer_chan, PUSER, "syncer", HZ);
    if (bcache.ndirty >= HIDIRTY)
      while (bcache.ndirty > LODIRTY && bio_push_oldest() == 0)
        ;
    bio_flush_dev(0, SYNCER_DELAY * HZ);
  }
}

// a device has gone
void bio_forget_dev(uint dev) {
  struct buf *b;
  int i;

  bio_flush_dev(dev, 0);

  mtx_enter(&bcache.lock);
  for (i = 0; i < nbuf; i++) {
    b = bcache.buf[i];
    if (b->dev != dev || b->next == 0)
      continue;
    pin_unlink(b);
    hash_remove(b);
    b->flags = 0;
    freelist_remove(b);
    agelist_append(b);
  }
  mtx_leave(&bcache.lock);
}

// READ-AHEAD

static struct buf *bget_try(uint dev, uint64_t blockno, int *cached) {
  struct buf *b;

  *cached = 0;
  mtx_enter(&bcache.lock);
  if (hash_find(dev, 0, blockno) != 0) {
    mtx_leave(&bcache.lock);
    *cached = 1;
    return 0;
  }
  if ((b = recycle(dev, 0, blockno, blockno)) == 0) {
    mtx_leave(&bcache.lock);
    return 0;
  }
  mtx_leave(&bcache.lock);
  if (trysleep(&b->lock)) {
    uint64_t gen;

    if (b->bufsize == BSIZE || allocbuf(b, BSIZE, 0, &gen) == 0)
      return b;
    releasesleep(&b->lock);
  }
  mtx_enter(&bcache.lock);
  b->refcnt = 0;
  freelist_insert(b);
  mtx_leave(&bcache.lock);
  return 0;
}

static struct buf *bdirty_try(uint dev, uint64_t blockno) {
  struct buf *b;

  mtx_enter(&bcache.lock);
  if ((b = hash_find(dev, 0, blockno)) != 0) {
    if (b->refcnt != 0 || (b->flags & (B_DELWRI | B_PINNED)) != B_DELWRI ||
        b->bufsize != BSIZE) {
      mtx_leave(&bcache.lock);
      return 0;
    }
    b->refcnt++;
    mtx_leave(&bcache.lock);
    if (trysleep(&b->lock))
      return b;
    mtx_enter(&bcache.lock);
    b->refcnt--;
    mtx_leave(&bcache.lock);
    return 0;
  }
  mtx_leave(&bcache.lock);
  return 0;
}

#define NRA 16
#define RA_SMALL 8
static struct {
  uint dev;
  uint64_t next;
} readahead[NRA];
static int ra_victim;

static int ra_find(uint dev, uint64_t blockno) {
  int i;

  for (i = 0; i < NRA; i++)
    if (readahead[i].dev == dev && blockno - readahead[i].next < BCLUSTER)
      return i;
  return -1;
}

static uint ra_want(uint dev, uint64_t blockno) {
  return ra_find(dev, blockno) >= 0 ? BCLUSTER : RA_SMALL;
}

static void ra_note(uint dev, uint64_t blockno, uint got) {
  int i = ra_find(dev, blockno);

  if (i < 0) {
    i = ra_victim;
    ra_victim = (ra_victim + 1) % NRA;
    readahead[i].dev = dev;
  }
  readahead[i].next = blockno + got;
}

static uint cluster_size(uint dev, uint64_t blockno) {
  uint64_t blocks = blockdev_device_size(dev) / (BSIZE / FNU_BLOCKDEV_SECTOR_SIZE);
  uint n = blockdev_maxphys(dev) / BSIZE;

  if (n > clusterbytes / BSIZE)
    n = clusterbytes / BSIZE;
  if (n > (uint)nbuf / 2)
    n = (uint)nbuf / 2;
  if (blocks != 0 && blockno + n > blocks)
    n = (uint)(blocks - blockno);
  return n;
}

static void cluster_release(struct buf *list) {
  struct buf *b;

  while ((b = list) != 0) {
    list = b->cnext;
    b->cnext = 0;
    brelse(b);
  }
}

static int cluster_read(uint dev, uint64_t blockno, struct buf *first) {
  struct buf *list = 0, **tail = &list, *b;
  uint n = cluster_size(dev, blockno), i, got = 1;
  int cached;

  if (n > ra_want(dev, blockno))
    n = ra_want(dev, blockno);
  if (cluster == 0 || n < 2)
    return -1;
  for (i = 1; i < n; i++) {
    if ((b = bget_try(dev, blockno + i, &cached)) == 0 && !cached)
      break;
    if (b != 0) {
      b->cnext = 0;
      *tail = b;
      tail = &b->cnext;
      got = i + 1;
    }
  }
  ra_note(dev, blockno, got);
  if (list == 0)
    return -1;

  acquiresleep(&clusterbuf.lock);
  clusterbuf.flags = 0;
  clusterbuf.dev = dev;
  clusterbuf.blockno = blockno;
  clusterbuf.addr = cluster;
  clusterbuf.bcount = got * BSIZE;
  blockdev_rw(&clusterbuf);
  if (clusterbuf.flags & B_ERROR) {
    releasesleep(&clusterbuf.lock);
    cluster_release(list);
    return -1;
  }
  memmove(first->data, cluster, BSIZE);
  first->flags |= B_VALID;
  for (b = list; b != 0; b = b->cnext) {
    memmove(b->data, cluster + (b->blockno - blockno) * BSIZE, BSIZE);
    b->flags |= B_VALID;
  }
  releasesleep(&clusterbuf.lock);
  cluster_release(list);
  return 0;
}

// WRITE-BEHIND
static struct {
  uint dev;
  uint64_t owner;
  uint64_t start;
  uint64_t next;
} writebehind[NRA];
static int wb_victim;

static int wb_note(uint dev, uint64_t owner, uint64_t blockno, uint size,
                   uint64_t *start) {
  uint n = owner == 0 ? cluster_size(dev, blockno) : clusterbytes / size;
  int i, k = -1;

  if (n < 2)
    return 0;
  if (n > BCLUSTER)
    n = BCLUSTER;
  for (i = 0; i < NRA; i++)
    if (writebehind[i].dev == dev && writebehind[i].owner == owner &&
        writebehind[i].next == blockno) {
      k = i;
      break;
    }
  if (k < 0) {
    k = wb_victim;
    wb_victim = (wb_victim + 1) % NRA;
    writebehind[k].dev = dev;
    writebehind[k].owner = owner;
    writebehind[k].start = blockno;
  }
  writebehind[k].next = blockno + 1;
  if (writebehind[k].next - writebehind[k].start < n)
    return 0;
  *start = writebehind[k].start;
  writebehind[k].start = writebehind[k].next;
  return 1;
}

static struct buf *bdirty_try_file(uint dev, uint64_t owner, uint64_t lblkno,
                                   uint64_t blockno, uint size) {
  struct buf *b;

  mtx_enter(&bcache.lock);
  if ((b = hash_find(dev, owner, lblkno)) == 0 || b->refcnt != 0 ||
      (b->flags & (B_DELWRI | B_PINNED)) != B_DELWRI || b->bufsize != size ||
      (blockno != 0 && b->blockno != blockno)) {
    mtx_leave(&bcache.lock);
    return 0;
  }
  b->refcnt++;
  mtx_leave(&bcache.lock);
  if (trysleep(&b->lock))
    return b;
  mtx_enter(&bcache.lock);
  b->refcnt--;
  mtx_leave(&bcache.lock);
  return 0;
}

static void wb_push(uint dev, uint64_t owner, uint64_t start, uint size) {
  struct buf *b = owner == 0 ? bdirty_try(dev, start)
                             : bdirty_try_file(dev, owner, start, 0, size);

  if (b == 0)
    return;
  cluster_write(b);
  brelse(b);
}

static void bclean(struct buf *b) {
  mtx_enter(&bcache.lock);
  if (b->flags & B_DELWRI)
    dirty_remove(b);
  b->flags &= ~(B_DIRTY | B_DELWRI);
  bufmem_wake();
  mtx_leave(&bcache.lock);
}

static void cluster_unwind(struct buf *first, struct buf *list) {
  struct buf *b;

  bwrite(first);
  while ((b = list) != 0) {
    list = b->cnext;
    b->cnext = 0;
    bawrite(b);
  }
}

// and what did is clean
static void cluster_done(struct buf *first, struct buf *list) {
  struct buf *b;

  bclean(first);
  for (b = list; b != 0; b = b->cnext)
    bclean(b);
  cluster_release(list);
}

static void cluster_write_file(struct buf *first) {
  struct buf *list = 0, **tail = &list, *b;
  uint size = first->bufsize, n, i, got = 1;
  uint64_t step = size / BSIZE;

  n = clusterbytes / size;
  if (n > BCLUSTER)
    n = BCLUSTER;
  if (cluster == 0 || n < 2) {
    bwrite(first);
    return;
  }
  for (i = 1; i < n; i++) {
    if ((b = bdirty_try_file(first->dev, first->owner, first->lblkno + i,
                             first->blockno + i * step, size)) == 0)
      break;
    b->cnext = 0;
    *tail = b;
    tail = &b->cnext;
    got++;
  }
  if (got < 2) {
    bwrite(first);
    return;
  }

  acquiresleep(&clusterbuf.lock);
  memmove(cluster, first->data, size);
  for (b = list; b != 0; b = b->cnext)
    memmove(cluster + (b->lblkno - first->lblkno) * size, b->data, size);
  clusterbuf.flags = B_DIRTY;
  clusterbuf.dev = first->dev;
  clusterbuf.blockno = first->blockno;
  clusterbuf.addr = cluster;
  clusterbuf.bcount = got * size;
  blockdev_rw(&clusterbuf);
  if (clusterbuf.flags & B_ERROR) {
    releasesleep(&clusterbuf.lock);
    cluster_unwind(first, list);
    return;
  }
  releasesleep(&clusterbuf.lock);
  cluster_done(first, list);
}

void cluster_write(struct buf *first) {
  struct buf *list = 0, **tail, *b;
  uint n = cluster_size(first->dev, first->blockno), i, back = 0, got;
  uint64_t start;

  if (first->owner != 0) {
    cluster_write_file(first);
    return;
  }
  if (cluster == 0 || n < 2 || first->bufsize != BSIZE) {
    bwrite(first);
    return;
  }
  for (i = 1; i < n && i <= first->blockno; i++) {
    if ((b = bdirty_try(first->dev, first->blockno - i)) == 0)
      break;
    b->cnext = list;
    list = b;
    back++;
  }
  start = first->blockno - back;
  got = back + 1;
  for (tail = &list; *tail != 0; tail = &(*tail)->cnext)
    ;
  for (i = 1; got < n; i++) {
    if ((b = bdirty_try(first->dev, first->blockno + i)) == 0)
      break;
    b->cnext = 0;
    *tail = b;
    tail = &b->cnext;
    got++;
  }
  if (got < 2) {
    bwrite(first);
    return;
  }

  acquiresleep(&clusterbuf.lock);
  memmove(cluster + back * BSIZE, first->data, BSIZE);
  for (b = list; b != 0; b = b->cnext)
    memmove(cluster + (b->blockno - start) * BSIZE, b->data, BSIZE);
  clusterbuf.flags = B_DIRTY;
  clusterbuf.dev = first->dev;
  clusterbuf.blockno = start;
  clusterbuf.addr = cluster;
  clusterbuf.bcount = got * BSIZE;
  blockdev_rw(&clusterbuf);
  if (clusterbuf.flags & B_ERROR) {
    releasesleep(&clusterbuf.lock);
    cluster_unwind(first, list);
    return;
  }
  releasesleep(&clusterbuf.lock);
  cluster_done(first, list);
}

struct buf *bgetblk(uint dev, uint64_t blockno) {
  struct buf *b = bget(dev, blockno);

  b->flags |= B_VALID;
  return b;
}

uint bio_run_max(void) { return cluster != 0 ? clusterbytes : 0; }

uint64_t bio_capacity(void) { return bufregion_bytes; }

// RAW RUNS
int bio_read_run(uint dev, uint64_t blockno, uint nblk, uchar *dst) {
  struct buf *b;
  uint bytes = nblk * BSIZE;

  if (cluster == 0 || nblk == 0 || bytes > clusterbytes)
    return -1;
  acquiresleep(&clusterbuf.lock);
  clusterbuf.flags = 0;
  clusterbuf.dev = dev;
  clusterbuf.blockno = blockno;
  clusterbuf.addr = cluster;
  clusterbuf.bcount = bytes;
  blockdev_rw(&clusterbuf);
  if (clusterbuf.flags & B_ERROR) {
    releasesleep(&clusterbuf.lock);
    return -1;
  }
  memmove(dst, cluster, bytes);
  releasesleep(&clusterbuf.lock);

  mtx_enter(&bcache.lock);
  for (b = bcache.dirty.dnext; b != &bcache.dirty; b = b->dnext) {
    uint64_t lo, hi;

    if (b->dev != dev || b->flags & B_MARKER)
      continue;
    lo = b->blockno > blockno ? b->blockno : blockno;
    hi = b->blockno + b->bufsize / BSIZE;
    if (hi > blockno + nblk)
      hi = blockno + nblk;
    if (lo < hi)
      memmove(dst + (lo - blockno) * BSIZE,
              b->data + (lo - b->blockno) * BSIZE,
              (size_t)(hi - lo) * BSIZE);
  }
  mtx_leave(&bcache.lock);
  return 0;
}

int bio_write_run(uint dev, uint64_t blockno, uint nblk, const uchar *src) {
  struct buf *b;
  uint bytes = nblk * BSIZE, i;
  int err;

  if (cluster == 0 || nblk == 0 || bytes > clusterbytes)
    return -1;
  acquiresleep(&clusterbuf.lock);
  memmove(cluster, src, bytes);
  clusterbuf.flags = B_DIRTY;
  clusterbuf.dev = dev;
  clusterbuf.blockno = blockno;
  clusterbuf.addr = cluster;
  clusterbuf.bcount = bytes;
  blockdev_rw(&clusterbuf);
  err = (clusterbuf.flags & B_ERROR) != 0;
  releasesleep(&clusterbuf.lock);
  if (err)
    return -1;

  mtx_enter(&bcache.lock);
  for (i = 0; i < (uint)nbuf; i++) {
    uint64_t lo, hi;

    b = bcache.buf[i];
    if (b->dev != dev || b->data == 0)
      continue;
    lo = b->blockno > blockno ? b->blockno : blockno;
    hi = b->blockno + b->bufsize / BSIZE;
    if (hi > blockno + nblk)
      hi = blockno + nblk;
    if (lo >= hi)
      continue;
    pin_unlink(b);
    if (lo != b->blockno || hi != b->blockno + b->bufsize / BSIZE ||
        b->refcnt != 0) {
      if (b->flags & B_DELWRI)
        dirty_remove(b);
      b->flags &= ~(B_DIRTY | B_DELWRI | B_VALID);
      continue;
    }
    memmove(b->data, src + (lo - blockno) * BSIZE, b->bufsize);
    b->flags |= B_VALID;
    if (b->flags & B_DELWRI)
      dirty_remove(b);
    b->flags &= ~(B_DIRTY | B_DELWRI);
  }
  mtx_leave(&bcache.lock);
  return 0;
}

struct buf *bread(uint dev, uint64_t blockno) {
  struct buf *b;

  b = bget(dev, blockno);
  if ((b->flags & B_VALID) == 0 && cluster_read(dev, blockno, b) == 0)
    return b;
  if ((b->flags & B_VALID) == 0) {
    blockdev_rw(b);
    if (b->flags & B_ERROR)
      memset(b->data, 0, BSIZE);
  }
  return b;
}

struct buf *bread_size(uint dev, uint64_t blockno, uint size) {
  struct buf *b;

  if (size == BSIZE)
    return bread(dev, blockno);
  b = bget_size(dev, blockno, size);
  if ((b->flags & B_VALID) == 0) {
    blockdev_rw(b);
    if (b->flags & B_ERROR)
      memset(b->data, 0, size);
  }
  return b;
}

struct buf *bgetblk_size(uint dev, uint64_t blockno, uint size) {
  struct buf *b = bget_size(dev, blockno, size);

  b->flags |= B_VALID;
  return b;
}

static int regrow(struct buf *b, uint64_t blockno, uint size, uint64_t *gen) {
  uint was = b->bufsize;

  b->blockno = blockno;
  if (allocbuf(b, size, 1, gen) < 0)
    return -1;
  if (b->pinlist != 0) {
    mtx_enter(&bcache.lock);
    b->pinlist->bytes += (uint64_t)b->bufsize - was;
    mtx_leave(&bcache.lock);
  }
  return 0;
}

// A FILE'S OWN BLOCKS
static struct buf *bget_file(uint dev, uint64_t owner, uint64_t lblkno,
                             uint64_t blockno, uint size) {
  struct buf *b;
  uint64_t gen = 0;

again:
  mtx_enter(&bcache.lock);
  if ((b = hash_find(dev, owner, lblkno)) != 0) {
    if (b->next != 0)
      freelist_remove(b);
    b->refcnt++;
    mtx_leave(&bcache.lock);
    acquiresleep(&b->lock);
    b->blockno = blockno;
    if (b->bufsize != size && (b->flags & B_PINNED) != 0) {
      uint was = b->bufsize;

      if (regrow(b, blockno, size, &gen) < 0) {
        brelse(b);
        bufwait(gen);
        goto again;
      }
      if (size > was)
        memset(b->data + was, 0, size - was);
      return b;
    }
    if (b->bufsize < size && (b->flags & (B_DIRTY | B_DELWRI)) != 0)
      bwrite(b);
    if (b->bufsize != size &&
        allocbuf(b, size, b->bufsize > size ? 1 : 0, &gen) < 0) {
      brelse(b);
      bufwait(gen);
      goto again;
    }
    return b;
  }
  if ((b = recycle(dev, owner, lblkno, blockno)) != 0) {
    mtx_leave(&bcache.lock);
    acquiresleep(&b->lock);
    if (b->bufsize != size && allocbuf(b, size, 0, &gen) < 0) {
      brelse(b);
      bufwait(gen);
      goto again;
    }
    return b;
  }
  mtx_leave(&bcache.lock);

  if (bio_push_oldest() == 0)
    goto again;
  panic("bget_file: no buffers");
}

struct buf *bread_file(uint dev, uint64_t owner, uint64_t lblkno,
                       uint64_t blockno, uint size) {
  struct buf *b = bget_file(dev, owner, lblkno, blockno, size);

  if ((b->flags & B_VALID) == 0) {
    blockdev_rw(b);
    if (b->flags & B_ERROR)
      memset(b->data, 0, size);
  }
  return b;
}

struct buf *bgetblk_file(uint dev, uint64_t owner, uint64_t lblkno,
                         uint64_t blockno, uint size) {
  struct buf *b = bget_file(dev, owner, lblkno, blockno, size);

  b->flags |= B_VALID;
  return b;
}

int bio_regrow(struct buf *b, uint64_t blockno, uint size) {
  uint64_t gen;

  if (!holdingsleep(&b->lock))
    panic("bio_regrow");
  return regrow(b, blockno, size, &gen);
}

void bio_forget_file(uint dev, uint64_t owner, uint64_t from) {
  struct buf *b;
  int i;

  mtx_enter(&bcache.lock);
  for (i = 0; i < nbuf; i++) {
    b = bcache.buf[i];
    if (b->dev != dev || b->owner != owner || b->lblkno < from ||
        b->refcnt != 0)
      continue;
    pin_unlink(b);
    if (b->flags & B_DELWRI)
      dirty_remove(b);
    b->flags = 0;
    hash_remove(b);
    if (b->next != 0)
      freelist_remove(b);
    agelist_append(b);
  }
  mtx_leave(&bcache.lock);
}

void bio_forget_run(uint dev, uint64_t blockno, uint nblk) {
  struct buf *b;
  int i;

  mtx_enter(&bcache.lock);
  for (i = 0; i < nbuf; i++) {
    b = bcache.buf[i];
    if (b->dev != dev || b->owner != 0 || b->blockno < blockno ||
        b->blockno >= blockno + nblk || b->refcnt != 0)
      continue;
    pin_unlink(b);
    if (b->flags & B_DELWRI)
      dirty_remove(b);
    b->flags = 0;
    hash_remove(b);
    if (b->next != 0)
      freelist_remove(b);
    agelist_append(b);
  }
  mtx_leave(&bcache.lock);
}

void bio_forget_mount(uint dev, uint owner_high) {
  struct buf *b;
  int i;

  mtx_enter(&bcache.lock);
  for (i = 0; i < nbuf; i++) {
    b = bcache.buf[i];
    if (b->dev != dev || (uint)(b->owner >> 32) != owner_high ||
        b->refcnt != 0)
      continue;
    pin_unlink(b);
    if (b->flags & B_DELWRI)
      dirty_remove(b);
    b->flags = 0;
    hash_remove(b);
    if (b->next != 0)
      freelist_remove(b);
    agelist_append(b);
  }
  mtx_leave(&bcache.lock);
}

void bwrite(struct buf *b) {
  if (!holdingsleep(&b->lock)) {
    panic("bwrite");
  }
  if (b->flags & B_PINNED)
    panic("bwrite: a pinned block before its log has it");
  b->flags |= B_DIRTY;
  blockdev_rw(b);
  bclean(b);
}

static int ioflag(void) {
  struct proc *p = curproc;

  return p != 0 ? p->ioflag : 0;
}

void bdwrite(struct buf *b) {
  uint dev = b->dev, size = b->bufsize;
  uint64_t owner = b->owner, key = owner != 0 ? b->lblkno : b->blockno;
  uint64_t start = 0;
  int full;

  if ((ioflag() & (PIO_NOREUSE | PIO_SYNC)) && !(b->flags & B_PINNED)) {
    bwrite(b);
    brelse(b);
    return;
  }
  full = (b->flags & B_AGE) != 0 && wb_note(dev, owner, key, size, &start);
  bdirty(b);
  brelse(b);
  if (full)
    wb_push(dev, owner, start, size);
}

void bdirty(struct buf *b) {
  if (!holdingsleep(&b->lock))
    panic("bdirty");
  mtx_enter(&bcache.lock);
  if ((b->flags & B_DELWRI) == 0) {
    b->flags |= B_DELWRI;
    b->dirtyat = ticks;
    dirty_insert(b);
  }
  b->flags |= B_VALID;
  mtx_leave(&bcache.lock);

  if (bcache.ndirty > HIDIRTY)
    wakeup(&syncer_chan);
  while (bcache.ndirty > HIDIRTY && bio_push_oldest() == 0)
    ;
}

void bawrite(struct buf *b) {
  bwrite(b);
  brelse(b);
}

static int bio_push_oldest(void) {
  struct buf marker, *b;
  int found = -1;

  memset(&marker, 0, sizeof(marker));
  marker.flags = B_MARKER;
  mtx_enter(&bcache.lock);
  marker.dnext = bcache.dirty.dnext;
  marker.dprev = &bcache.dirty;
  bcache.dirty.dnext->dprev = &marker;
  bcache.dirty.dnext = &marker;
  while ((b = marker.dnext) != &bcache.dirty) {
    marker_advance(&marker);
    if ((b->flags & (B_MARKER | B_PINNED)) != 0 || b->refcnt != 0)
      continue;
    b->refcnt++;
    mtx_leave(&bcache.lock);
    if (trysleep(&b->lock)) {
      if ((b->flags & (B_DELWRI | B_PINNED)) == B_DELWRI)
        cluster_write(b);
      brelse(b);
      mtx_enter(&bcache.lock);
      found = 0;
      break;
    }
    mtx_enter(&bcache.lock);
    b->refcnt--;
  }
  dirty_remove(&marker);
  mtx_leave(&bcache.lock);
  return found;
}

// release a locked buffer
void brelse(struct buf *b) {
  if (!holdingsleep(&b->lock)) {
    panic("brelse");
  }

  releasesleep(&b->lock);
  // B_NOREUSE
  if (ioflag() & PIO_NOREUSE)
    b->flags |= B_AGE;

  mtx_enter(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0 && (b->flags & (B_DIRTY | B_DELWRI)) == 0) {
    if (b->flags & B_AGE)
      agelist_append(b);
    else
      freelist_insert(b);
    b->flags &= ~B_AGE;
    bufmem_wake();
  }
  mtx_leave(&bcache.lock);
}
