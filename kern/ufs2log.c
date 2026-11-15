// THE UFS2 LOG
#include "defs.h"
#include "malloc.h"
#include "param.h"
#include "sleeplock.h"
#include "ufs2log.h"
#include "ulogfmt.h"

struct revoke {
  uint64_t sector;
  uint bytes;
  uint64_t seq;
};

struct ulog {
  struct ulog_place at;
  uint64_t area; // bytes of the ring
  uint64_t head;
  uint64_t tail;
  uint64_t seq;  // the next transaction's number
  uint64_t tail_seq;
  uint64_t gen;
  int live;
  struct bpinlist pins;
  struct revoke *rv;
  uint nrv, rvcap;
  uchar *stage;
  uint staged, stagecap;
  uint64_t wpos;
  uint64_t txlen;
  uint32_t crc;
};

struct loghdr {
  uint64_t gen, area, tail, tail_seq, mtime;
  uint32_t fsid[2], flags, ino;
};

static void put16(uchar *p, uint16_t v) {
  p[0] = (uchar)v;
  p[1] = (uchar)(v >> 8);
}
static void put32(uchar *p, uint32_t v) {
  put16(p, (uint16_t)v);
  put16(p + 2, (uint16_t)(v >> 16));
}
static void put64(uchar *p, uint64_t v) {
  put32(p, (uint32_t)v);
  put32(p + 4, (uint32_t)(v >> 32));
}
static uint16_t get16(const uchar *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uchar *p) {
  return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}
static uint64_t get64(const uchar *p) {
  return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static int file_sector(const struct ulog_place *at, uint64_t off,
                       uint64_t *sector, uint64_t *run) {
  uint i;

  for (i = 0; i < at->next; i++) {
    const struct ulog_extent *e = &at->ext[i];

    if (off >= e->off && off < e->off + e->len) {
      *sector = e->sector + (off - e->off) / ULOG_BLK;
      *run = e->len - (off - e->off);
      return 0;
    }
  }
  return -1;
}

static int file_io(const struct ulog_place *at, uint64_t off, uchar *buf,
                   uint64_t len, int write) {
  uint max = bio_run_max() & ~(ULOG_BLK - 1);

  if (max == 0)
    return -1;
  while (len > 0) {
    uint64_t sector, run, n = len;
    int r;

    if (file_sector(at, off, &sector, &run) < 0)
      return -1;
    if (n > run)
      n = run;
    if (n > max)
      n = max;
    if (write)
      r = bio_write_run(at->dev.device, at->dev.first_lba + sector,
                        (uint)(n / ULOG_BLK), buf);
    else
      r = bio_read_run(at->dev.device, at->dev.first_lba + sector,
                       (uint)(n / ULOG_BLK), buf);
    if (r < 0)
      return -1;
    off += n;
    buf += n;
    len -= n;
  }
  return 0;
}

static int ring_io(const struct ulog_place *at, uint64_t area, uint64_t pos,
                   uchar *buf, uint64_t len, int write) {
  while (len > 0) {
    uint64_t n = area - pos < len ? area - pos : len;

    if (file_io(at, ULOG_AREA + pos, buf, n, write) < 0)
      return -1;
    pos = (pos + n) % area;
    buf += n;
    len -= n;
  }
  return 0;
}

static uint64_t ring_area(const struct ulog_place *at) {
  return (at->size - ULOG_AREA) & ~(uint64_t)(ULOG_BLK - 1);
}

// HEADERS
static void hdr_put(uchar *b, const struct loghdr *h) {
  memset(b, 0, ULOG_BLK);
  memmove(b, ulog_magic, sizeof(ulog_magic));
  put32(b + ULOG_H_VERSION, ULOG_VERSION);
  put64(b + ULOG_H_GEN, h->gen);
  put32(b + ULOG_H_FSID, h->fsid[0]);
  put32(b + ULOG_H_FSID + 4, h->fsid[1]);
  put64(b + ULOG_H_MTIME, h->mtime);
  put64(b + ULOG_H_AREA, h->area);
  put64(b + ULOG_H_TAIL, h->tail);
  put64(b + ULOG_H_TAILSEQ, h->tail_seq);
  put32(b + ULOG_H_FLAGS, h->flags);
  put32(b + ULOG_H_INO, h->ino);
  put32(b + ULOG_H_CRC, ulog_crc(0, b, ULOG_BLK));
}

static int hdr_get(uchar *b, struct loghdr *h) {
  uint32_t want = get32(b + ULOG_H_CRC);

  if (memcmp(b, ulog_magic, sizeof(ulog_magic)) != 0 ||
      get32(b + ULOG_H_VERSION) != ULOG_VERSION)
    return -1;
  put32(b + ULOG_H_CRC, 0);
  if (ulog_crc(0, b, ULOG_BLK) != want)
    return -1;
  h->gen = get64(b + ULOG_H_GEN);
  h->fsid[0] = get32(b + ULOG_H_FSID);
  h->fsid[1] = get32(b + ULOG_H_FSID + 4);
  h->mtime = get64(b + ULOG_H_MTIME);
  h->area = get64(b + ULOG_H_AREA);
  h->tail = get64(b + ULOG_H_TAIL);
  h->tail_seq = get64(b + ULOG_H_TAILSEQ);
  h->flags = get32(b + ULOG_H_FLAGS);
  h->ino = get32(b + ULOG_H_INO);
  return 0;
}

static int hdr_read(const struct ulog_place *at, uchar *b, struct loghdr *h) {
  struct loghdr t;
  int found = -1, s;

  for (s = 0; s < 2; s++) {
    if (file_io(at, (uint64_t)s * ULOG_BLK, b, ULOG_BLK, 0) < 0 ||
        hdr_get(b, &t) < 0)
      continue;
    if (found < 0 || t.gen > h->gen) {
      *h = t;
      found = s;
    }
  }
  return found;
}

static int hdr_write(struct ulog *l) {
  struct loghdr h;
  uchar b[ULOG_BLK];

  memset(&h, 0, sizeof(h));
  h.gen = l->gen + 1;
  h.fsid[0] = l->at.fsid[0];
  h.fsid[1] = l->at.fsid[1];
  h.mtime = l->at.mtime;
  h.area = l->area;
  h.tail = l->tail;
  h.tail_seq = l->tail_seq;
  h.flags = l->live ? ULOG_LIVE : 0;
  h.ino = l->at.ino;
  hdr_put(b, &h);
  if (file_io(&l->at, (h.gen % 2) * ULOG_BLK, b, ULOG_BLK, 1) < 0 ||
      blockdev_flush(l->at.dev.device) < 0)
    return -1;
  l->gen = h.gen;
  return 0;
}

static void rec_head(uchar *b, uint type, uint count, uint64_t seq, uint len,
                     uint flags) {
  memset(b, 0, ULOG_BLK);
  put32(b, LR_MAGIC);
  put16(b + LR_TYPE, (uint16_t)type);
  put16(b + LR_COUNT, (uint16_t)count);
  put64(b + LR_SEQ, seq);
  put32(b + LR_LEN, len);
  put32(b + LR_FLAGS, flags);
}

// RECORDS ON THEIR WAY OUT
static int stage_out(struct ulog *l) {
  if (l->staged == 0)
    return 0;
  if (ring_io(&l->at, l->area, l->wpos, l->stage, l->staged, 1) < 0)
    return -1;
  l->wpos = (l->wpos + l->staged) % l->area;
  l->staged = 0;
  return 0;
}

static int emit(struct ulog *l, const uchar *p, uint64_t n, int sum) {
  if (sum) {
    l->crc = ulog_crc(l->crc, p, n);
    l->txlen += n;
  }
  while (n > 0) {
    uint take = l->stagecap - l->staged;

    if (take > n)
      take = (uint)n;
    memmove(l->stage + l->staged, p, take);
    l->staged += take;
    p += take;
    n -= take;
    if (l->staged == l->stagecap && stage_out(l) < 0)
      return -1;
  }
  return 0;
}

static uint64_t howmany64(uint64_t n, uint64_t per) { return (n + per - 1) / per; }

static uint64_t tx_bytes(struct ulog *l, uint norphans) {
  uint64_t n = 0;
  struct buf *b;

  for (b = l->pins.first; b != 0; b = b->pnext)
    n += b->bufsize;
  n += howmany64(l->pins.count, LR_ENTRIES) * ULOG_BLK;
  n += howmany64(l->nrv, LR_ENTRIES) * ULOG_BLK;
  n += (norphans == 0 ? 1 : howmany64(norphans, LR_INOS)) * ULOG_BLK;
  return n + ULOG_BLK;
}

static uint64_t ring_used(struct ulog *l) {
  return l->live ? (l->head + l->area - l->tail) % l->area : 0;
}

struct bpinlist *ulog_pins(struct ulog *l) { return &l->pins; }

uint64_t ulog_pending(struct ulog *l, uint norphans) {
  return l->pins.bytes + howmany64(l->pins.count, LR_ENTRIES) * ULOG_BLK +
         howmany64(l->nrv, LR_ENTRIES) * ULOG_BLK +
         (norphans == 0 ? 1 : howmany64(norphans, LR_INOS)) * ULOG_BLK + ULOG_BLK;
}

uint64_t ulog_room(struct ulog *l) { return l->area; }

int ulog_revoke(struct ulog *l, uint64_t sector, uint bytes) {
  if (l->nrv == l->rvcap) {
    uint cap = l->rvcap == 0 ? 64 : l->rvcap * 2;
    struct revoke *n = malloc(cap * sizeof(*n), M_UFS, M_WAITOK);

    if (l->nrv > 0)
      memmove(n, l->rv, l->nrv * sizeof(*n));
    if (l->rv != 0)
      free(l->rv, M_UFS);
    l->rv = n;
    l->rvcap = cap;
  }
  l->rv[l->nrv].sector = sector;
  l->rv[l->nrv].bytes = bytes;
  l->rv[l->nrv].seq = l->seq;
  l->nrv++;
  return 0;
}

int ulog_commit(struct ulog *l, const uint32_t *orphans, uint norphans) {
  uint64_t need = tx_bytes(l, norphans);
  uchar d[ULOG_BLK];
  struct buf *b, *first;
  uint i, n;

  if (need >= l->area) {
    printf("UFS2: a transaction of %lu bytes does not fit a log of %lu\n",
            need, l->area);
    return -1;
  }
  bio_flush_device(l->at.dev.device);
  if (blockdev_flush(l->at.dev.device) < 0)
    return -1;
  if (!l->live || ring_used(l) + need >= l->area) {
    l->tail = l->head;
    l->tail_seq = l->seq;
    l->live = 1;
    if (hdr_write(l) < 0)
      return -1;
  }

  l->wpos = l->head;
  l->staged = 0;
  l->txlen = 0;
  l->crc = 0;
  for (b = l->pins.first; b != 0;) {
    uint len = ULOG_BLK;

    first = b;
    rec_head(d, LR_BLOCKS, 0, l->seq, 0, 0);
    for (n = 0; b != 0 && n < LR_ENTRIES; n++, b = b->pnext) {
      put64(d + LR_HDR + n * 16, b->blockno - l->at.dev.first_lba);
      put32(d + LR_HDR + n * 16 + 8, b->bufsize);
      len += b->bufsize;
    }
    put16(d + 6, (uint16_t)n);
    put32(d + 16, len);
    if (emit(l, d, ULOG_BLK, 1) < 0)
      return -1;
    for (b = first, i = 0; i < n; i++, b = b->pnext) {
      int r;

      acquiresleep(&b->lock);
      r = emit(l, b->data, b->bufsize, 1);
      releasesleep(&b->lock);
      if (r < 0)
        return -1;
    }
  }
  for (i = 0; i < l->nrv;) {
    rec_head(d, LR_REVOKE, 0, l->seq, ULOG_BLK, 0);
    for (n = 0; i < l->nrv && n < LR_ENTRIES; n++, i++) {
      put64(d + LR_HDR + n * 16, l->rv[i].sector);
      put32(d + LR_HDR + n * 16 + 8, l->rv[i].bytes);
    }
    put16(d + 6, (uint16_t)n);
    if (emit(l, d, ULOG_BLK, 1) < 0)
      return -1;
  }
  i = 0;
  do {
    rec_head(d, LR_ORPHANS, 0, l->seq, ULOG_BLK, i == 0 ? LR_FIRST : 0);
    for (n = 0; i < norphans && n < LR_INOS; n++, i++)
      put32(d + LR_HDR + n * 4, orphans[i]);
    put16(d + 6, (uint16_t)n);
    if (emit(l, d, ULOG_BLK, 1) < 0)
      return -1;
  } while (i < norphans);
  rec_head(d, LR_COMMIT, 0, l->seq, ULOG_BLK, 0);
  put32(d + LR_HDR, l->crc);
  put32(d + LR_HDR + 4, (uint32_t)(l->txlen / ULOG_BLK));
  {
    struct fnu_timespec now;

    time_now(&now);
    put64(d + LR_HDR + 8, (uint64_t)now.sec);
  }
  if (emit(l, d, ULOG_BLK, 0) < 0 || stage_out(l) < 0 ||
      blockdev_flush(l->at.dev.device) < 0)
    return -1;

  l->head = l->wpos;
  l->seq++;
  l->nrv = 0;
  while ((b = l->pins.first) != 0) {
    acquiresleep(&b->lock);
    bunpin(b);
    releasesleep(&b->lock);
  }
  return 0;
}

// A NEW WRITER
struct ulog *ulog_open(const struct ulog_place *at) {
  struct ulog *l;
  struct loghdr h;
  uchar b[ULOG_BLK];
  uint cap = bio_run_max() & ~(ULOG_BLK - 1);

  if (at->size <= ULOG_AREA + 4 * ULOG_BLK || cap == 0)
    return 0;
  l = malloc(sizeof(*l), M_UFS, M_WAITOK | M_ZERO);
  l->at = *at;
  l->at.ext = malloc(at->next * sizeof(*at->ext), M_UFS, M_WAITOK);
  memmove(l->at.ext, at->ext, at->next * sizeof(*at->ext));
  l->area = ring_area(at);
  if (hdr_read(at, b, &h) >= 0)
    l->gen = h.gen;
  random_bytes(&l->seq, sizeof(l->seq));
  l->tail_seq = l->seq;
  if ((l->stage = malloc(cap, M_UFS, M_WAITOK | M_CANFAIL)) == 0) {
    free(l->at.ext, M_UFS);
    free(l, M_UFS);
    return 0;
  }
  l->stagecap = cap;
  if (hdr_write(l) < 0) {
    ulog_close(l);
    return 0;
  }
  return l;
}

void ulog_abandon(struct ulog *l) {
  struct buf *b;

  while ((b = l->pins.first) != 0) {
    acquiresleep(&b->lock);
    bunpin(b);
    releasesleep(&b->lock);
  }
  l->live = 0;
  ulog_close(l);
}

void ulog_close(struct ulog *l) {
  if (l->live) {
    l->live = 0;
    l->tail = l->head = 0;
    hdr_write(l);
  }
  if (l->rv != 0)
    free(l->rv, M_UFS);
  free(l->stage, M_UFS);
  free(l->at.ext, M_UFS);
  free(l, M_UFS);
}

// REPLAY
struct txmark {
  uint64_t pos, seq;
};

static void *grow(void *v, uint *cap, uint used, uint size) {
  void *n;

  if (used < *cap)
    return v;
  *cap = *cap == 0 ? 32 : *cap * 2;
  n = malloc((uint64_t)*cap * size, M_UFS, M_WAITOK);
  if (used > 0)
    memmove(n, v, (uint64_t)used * size);
  if (v != 0)
    free(v, M_UFS);
  return n;
}

static int revoked(const struct revoke *rv, uint nrv, uint64_t sector,
                   uint bytes, uint64_t seq) {
  uint i;

  for (i = 0; i < nrv; i++)
    if (rv[i].seq >= seq && rv[i].sector < sector + bytes / ULOG_BLK &&
        sector < rv[i].sector + rv[i].bytes / ULOG_BLK)
      return 1;
  return 0;
}

static int rec_check(const uchar *b, uint64_t seq, uint64_t area,
                     uint64_t fs_sectors) {
  uint type = get16(b + LR_TYPE), count = get16(b + LR_COUNT), len = get32(b + LR_LEN), i;
  uint64_t sum = ULOG_BLK;

  if (get32(b) != LR_MAGIC || get64(b + LR_SEQ) != seq || len < ULOG_BLK ||
      len % ULOG_BLK != 0 || len >= area)
    return -1;
  switch (type) {
  case LR_BLOCKS:
    if (count > LR_ENTRIES)
      return -1;
    for (i = 0; i < count; i++) {
      uint64_t sector = get64(b + LR_HDR + i * 16);
      uint bytes = get32(b + LR_HDR + i * 16 + 8);

      if (bytes == 0 || bytes % ULOG_BLK != 0 ||
          sector + bytes / ULOG_BLK > fs_sectors)
        return -1;
      sum += bytes;
    }
    return sum == len ? 0 : -1;
  case LR_REVOKE:
    return count <= LR_ENTRIES && len == ULOG_BLK ? 0 : -1;
  case LR_ORPHANS:
    return count <= LR_INOS && len == ULOG_BLK ? 0 : -1;
  case LR_COMMIT:
    return len == ULOG_BLK ? 0 : -1;
  }
  return -1;
}

// THE SCAN
struct scan {
  uint64_t area;
  uint max;
  uchar *buf;
  struct txmark *tx;
  struct revoke *rv;
  uint32_t *done;
  uint ntx, txcap, nrv, rvcap, ndone, donecap;
};

static void scan_free(struct scan *s) {
  if (s->buf != 0)
    free(s->buf, M_UFS);
  if (s->tx != 0)
    free(s->tx, M_UFS);
  if (s->rv != 0)
    free(s->rv, M_UFS);
  if (s->done != 0)
    free(s->done, M_UFS);
  memset(s, 0, sizeof(*s));
}

// ULOG_STALE
static int log_scan(const struct ulog_place *at, struct scan *s) {
  uint64_t area = ring_area(at), pos, seq, scanned = 0;
  uint32_t *pend = 0;
  uint npend = 0, pendcap = 0;
  struct loghdr h;
  uchar b[ULOG_BLK];

  memset(s, 0, sizeof(*s));
  s->area = area;
  s->max = bio_run_max() & ~(ULOG_BLK - 1);
  if (at->size <= ULOG_AREA + 4 * ULOG_BLK || s->max == 0)
    return -1;
  if (hdr_read(at, b, &h) < 0 || h.fsid[0] != at->fsid[0] ||
      h.fsid[1] != at->fsid[1] || h.ino != at->ino || h.area != area ||
      h.tail >= area) {
    printf("UFS2: the log is not this filesystem's\n");
    return ULOG_STALE;
  }
  if (h.mtime != at->mtime) {
    printf("UFS2: the volume was mounted or checked elsewhere since the log "
            "was written\n");
    return ULOG_STALE;
  }
  if (!(h.flags & ULOG_LIVE))
    return ULOG_CLEAN;
  if ((s->buf = malloc(s->max, M_UFS, M_WAITOK | M_CANFAIL)) == 0)
    return -1;

  pos = h.tail;
  seq = h.tail_seq;
  for (;;) {
    uint64_t start = pos;
    uint32_t crc = 0;
    uint keep = s->nrv;
    int ok = 0;

    npend = 0;
    for (;;) {
      uint type, count, i;

      if (scanned + ULOG_BLK > area ||
          ring_io(at, area, pos, b, ULOG_BLK, 0) < 0 ||
          rec_check(b, seq, area, at->dev.sector_count) < 0)
        break;
      type = get16(b + LR_TYPE);
      count = get16(b + LR_COUNT);
      if (type == LR_COMMIT) {
        ok = get32(b + LR_HDR) == crc &&
             (uint64_t)get32(b + LR_HDR + 4) * ULOG_BLK ==
                 (pos + area - start) % area;
        pos = (pos + ULOG_BLK) % area;
        scanned += ULOG_BLK;
        break;
      }
      crc = ulog_crc(crc, b, ULOG_BLK);
      if (type == LR_BLOCKS) {
        uint64_t left = get32(b + LR_LEN) - ULOG_BLK, at_pos = (pos + ULOG_BLK) % area;

        if (scanned + get32(b + LR_LEN) > area)
          break;
        while (left > 0) {
          uint64_t n = left < s->max ? left : s->max;

          if (ring_io(at, area, at_pos, s->buf, n, 0) < 0)
            break;
          crc = ulog_crc(crc, s->buf, n);
          at_pos = (at_pos + n) % area;
          left -= (uint)n;
        }
        if (left > 0)
          break;
      } else if (type == LR_REVOKE) {
        for (i = 0; i < count; i++) {
          s->rv = grow(s->rv, &s->rvcap, s->nrv, sizeof(*s->rv));
          s->rv[s->nrv].sector = get64(b + LR_HDR + i * 16);
          s->rv[s->nrv].bytes = get32(b + LR_HDR + i * 16 + 8);
          s->rv[s->nrv].seq = seq;
          s->nrv++;
        }
      } else {
        if (get32(b + LR_FLAGS) & LR_FIRST)
          npend = 0;
        for (i = 0; i < count; i++) {
          pend = grow(pend, &pendcap, npend, sizeof(*pend));
          pend[npend++] = get32(b + LR_HDR + i * 4);
        }
      }
      scanned += get32(b + LR_LEN);
      pos = (pos + get32(b + LR_LEN)) % area;
    }
    if (!ok) {
      s->nrv = keep;
      break;
    }
    s->tx = grow(s->tx, &s->txcap, s->ntx, sizeof(*s->tx));
    s->tx[s->ntx].pos = start;
    s->tx[s->ntx].seq = seq;
    s->ntx++;
    seq++;
    {
      uint32_t *swap = s->done;
      uint swapcap = s->donecap;

      s->done = pend;
      s->donecap = pendcap;
      s->ndone = npend;
      pend = swap;
      pendcap = swapcap;
    }
  }
  if (pend != 0)
    free(pend, M_UFS);
  return s->ntx > 0 ? ULOG_REPLAYED : ULOG_CLEAN;
}

static int log_walk(const struct ulog_place *at, struct scan *s,
                    int (*fn)(const struct ulog_place *, struct scan *, uint64_t,
                              uint, uint64_t, void *),
                    void *arg) {
  uchar b[ULOG_BLK];
  uint t;

  for (t = 0; t < s->ntx; t++) {
    uint64_t pos = s->tx[t].pos;

    for (;;) {
      uint64_t data;
      uint i, count;

      if (ring_io(at, s->area, pos, b, ULOG_BLK, 0) < 0)
        return -1;
      if (get16(b + LR_TYPE) == LR_COMMIT)
        break;
      count = get16(b + LR_COUNT);
      data = (pos + ULOG_BLK) % s->area;
      for (i = 0; get16(b + LR_TYPE) == LR_BLOCKS && i < count; i++) {
        uint64_t sector = get64(b + LR_HDR + i * 16);
        uint bytes = get32(b + LR_HDR + i * 16 + 8);

        if (!revoked(s->rv, s->nrv, sector, bytes, s->tx[t].seq) &&
            fn(at, s, sector, bytes, data, arg) < 0)
          return -1;
        data = (data + bytes) % s->area;
      }
      pos = (pos + get32(b + LR_LEN)) % s->area;
    }
  }
  return 0;
}

static int go_home(const struct ulog_place *at, struct scan *s, uint64_t sector,
                   uint bytes, uint64_t data, void *arg __unused) {
  uint done = 0;

  while (done < bytes) {
    uint n = bytes - done < s->max ? bytes - done : s->max;

    if (ring_io(at, s->area, (data + done) % s->area, s->buf, n, 0) < 0 ||
        bio_write_run(at->dev.device, at->dev.first_lba + sector + done / ULOG_BLK,
                      n / ULOG_BLK, s->buf) < 0)
      return -1;
    done += n;
  }
  return 0;
}

int ulog_replay(const struct ulog_place *at, uint32_t **orphans,
                uint *norphans) {
  struct scan s;
  int r;

  *orphans = 0;
  *norphans = 0;
  if ((r = log_scan(at, &s)) == ULOG_REPLAYED) {
    if (log_walk(at, &s, go_home, 0) < 0 || blockdev_flush(at->dev.device) < 0)
      r = -1;
    else {
      printf("UFS2: %u transaction%s from the log %s home\n", s.ntx,
             s.ntx == 1 ? "" : "s", s.ntx == 1 ? "is" : "are");
      *orphans = s.done;
      *norphans = s.ndone;
      s.done = 0;
    }
  }
  scan_free(&s);
  return r;
}

// A READ-ONLY MOUNT OVER A LIVE LOG
#define NOWHERE (~(uint64_t)0)

struct ovl_ent {
  uint64_t sector, pos;
  uint nsect;
};

struct ulog_overlay {
  struct ulog_place at;
  uint64_t area;
  struct ovl_ent *e;
  uint n, cap;
  struct sleeplock lock;
  struct buf io;
  uchar *mem;
  uint ref, gone;
  struct ulog_overlay *next;
};

static struct ulog_overlay *overlays;
static struct mutex ovlock = MUTEX_INITIALIZER("ulog overlay");

static int ovl_add(const struct ulog_place *at __unused, struct scan *s __unused, uint64_t sector,
                   uint bytes, uint64_t data, void *arg) {
  struct ulog_overlay *o = arg;

  o->e = grow(o->e, &o->cap, o->n, sizeof(*o->e));
  o->e[o->n].sector = sector;
  o->e[o->n].nsect = bytes / ULOG_BLK;
  o->e[o->n].pos = data;
  o->n++;
  return 0;
}

struct ulog_overlay *ulog_overlay_open(const struct ulog_place *at) {
  struct ulog_overlay *o;
  struct scan s;
  uint i;

  if (log_scan(at, &s) != ULOG_REPLAYED) {
    scan_free(&s);
    return 0;
  }
  o = malloc(sizeof(*o), M_UFS, M_WAITOK | M_ZERO);
  o->at = *at;
  o->at.ext = malloc((uint64_t)at->next * sizeof(*at->ext), M_UFS, M_WAITOK);
  for (i = 0; i < at->next; i++)
    o->at.ext[i] = at->ext[i];
  o->area = s.area;
  if (log_walk(at, &s, ovl_add, o) < 0 ||
      (o->mem = malloc(BSIZE, M_UFS, M_WAITOK | M_CANFAIL)) == 0) {
    scan_free(&s);
    if (o->e != 0)
      free(o->e, M_UFS);
    free(o->at.ext, M_UFS);
    free(o, M_UFS);
    return 0;
  }
  printf("UFS2: %u transactions read through the log, none written home\n", s.ntx);
  scan_free(&s);
  initsleeplock(&o->lock, "ulog overlay");
  initsleeplock(&o->io.lock, "ulog overlay io");
  mtx_enter(&ovlock);
  o->next = overlays;
  overlays = o;
  mtx_leave(&ovlock);
  return o;
}

static void ovl_put(struct ulog_overlay *o) {
  int last;

  mtx_enter(&ovlock);
  last = --o->ref == 0 && o->gone;
  mtx_leave(&ovlock);
  if (!last)
    return;
  free(o->mem, M_UFS);
  if (o->e != 0)
    free(o->e, M_UFS);
  free(o->at.ext, M_UFS);
  free(o, M_UFS);
}

void ulog_overlay_close(struct ulog_overlay *o) {
  struct ulog_overlay **pp;

  mtx_enter(&ovlock);
  for (pp = &overlays; *pp != 0 && *pp != o; pp = &(*pp)->next)
    ;
  if (*pp == o)
    *pp = o->next;
  o->gone = 1;
  o->ref++;
  mtx_leave(&ovlock);
  ovl_put(o);
}

static struct ulog_overlay *ovl_get(uint dev) {
  struct ulog_overlay *o;

  if (overlays == 0)
    return 0;
  mtx_enter(&ovlock);
  for (o = overlays; o != 0 && o->at.dev.device != dev; o = o->next)
    ;
  if (o != 0)
    o->ref++;
  mtx_leave(&ovlock);
  return o;
}

static int ovl_sector(struct ulog_overlay *o, uint64_t pos, uchar *dst) {
  uint64_t sector, run;

  if (file_sector(&o->at, ULOG_AREA + pos, &sector, &run) < 0)
    return -1;
  acquiresleep(&o->io.lock);
  o->io.flags = B_LOGREAD;
  o->io.dev = o->at.dev.device;
  o->io.blockno = o->at.dev.first_lba + sector;
  o->io.addr = o->mem;
  o->io.bcount = BSIZE;
  blockdev_rw(&o->io);
  if (o->io.flags & B_ERROR) {
    releasesleep(&o->io.lock);
    return -1;
  }
  memmove(dst, o->mem, BSIZE);
  releasesleep(&o->io.lock);
  return 0;
}

void ulog_overlay_read(uint dev, uint64_t blockno, uint nblk, uchar *data) {
  struct ulog_overlay *o = ovl_get(dev);
  uint64_t first, k;
  uchar *settled;
  int i;

  if (o == 0)
    return;
  first = o->at.dev.first_lba;
  settled = malloc((nblk + 7) / 8, M_UFS, M_WAITOK | M_ZERO);
  acquiresleep(&o->lock);
  for (i = (int)o->n - 1; i >= 0; i--) {
    const struct ovl_ent *e = &o->e[i];
    uint64_t lo = first + e->sector, hi = lo + e->nsect;

    if (hi <= blockno || lo >= blockno + nblk)
      continue;
    if (lo < blockno)
      lo = blockno;
    if (hi > blockno + nblk)
      hi = blockno + nblk;
    for (k = lo; k < hi; k++) {
      uint64_t at = k - blockno;

      if (settled[at / 8] & (1 << (at % 8)))
        continue;
      settled[at / 8] |= (uchar)(1 << (at % 8));
      if (e->pos != NOWHERE &&
          ovl_sector(o, (e->pos + (k - first - e->sector) * ULOG_BLK) % o->area,
                     data + at * BSIZE) < 0)
        printf("UFS2: the log could not be read for block %lu\n", k);
    }
  }
  releasesleep(&o->lock);
  free(settled, M_UFS);
  ovl_put(o);
}

void ulog_overlay_write(uint dev, uint64_t blockno, uint nblk) {
  struct ulog_overlay *o = ovl_get(dev);
  uint i;

  if (o == 0)
    return;
  acquiresleep(&o->lock);
  for (i = 0; i < o->n; i++) {
    uint64_t lo = o->at.dev.first_lba + o->e[i].sector;

    if (o->e[i].pos != NOWHERE && lo < blockno + nblk &&
        blockno < lo + o->e[i].nsect)
      break;
  }
  if (i < o->n && blockno >= o->at.dev.first_lba) {
    o->e = grow(o->e, &o->cap, o->n, sizeof(*o->e));
    o->e[o->n].sector = blockno - o->at.dev.first_lba;
    o->e[o->n].nsect = nblk;
    o->e[o->n].pos = NOWHERE;
    o->n++;
  }
  releasesleep(&o->lock);
  ovl_put(o);
}
