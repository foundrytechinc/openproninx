#ifndef PRONINX_X86_64_BUF_H
#define PRONINX_X86_64_BUF_H

#include "fs.h"
#include "inc/types.h"
#include "sleeplock.h"

struct bpinlist;

struct buf {
  int flags;
  uint dev;
  uint64_t owner;
  uint64_t lblkno;
  uint64_t blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev;
  struct buf *next;
  struct buf *dprev;
  struct buf *dnext;
  struct buf *qnext;
  struct buf *hnext;
  struct buf *cnext;
  uchar *addr;
  uint bcount;
  uint bufsize;
  uint64_t dirtyat;
  uint dirtyseq;
  uchar *data;
  struct bpinlist *pinlist;
  struct buf *pnext;
  struct buf *pprev;
};

struct bpinlist {
  struct buf *first;
  uint count;
  uint64_t bytes;
};

static inline uchar *bufdata(struct buf *b) {
  return b->addr != 0 ? b->addr : b->data;
}

static inline uint bufcount(struct buf *b) {
  return b->bcount != 0 ? b->bcount : (b->bufsize != 0 ? b->bufsize : BSIZE);
}
#define B_VALID 0x2
#define B_DIRTY 0x4
#define B_DELWRI 0x10
#define B_ERROR 0x8
#define B_AGE 0x20
#define B_FLUSH 0x40
#define B_PINNED 0x80
#define B_MARKER 0x100
#define B_LOGREAD 0x200

#endif
