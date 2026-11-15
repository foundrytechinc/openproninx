#ifndef FNU_PRONINX_UFS2LOG_H
#define FNU_PRONINX_UFS2LOG_H

#include "inc/types.h"
#include "blockdev.h"
#include "buf.h"

#define ULOG_MIN (1024 * 1024)
#define ULOG_MAX (64 * 1024 * 1024)
#define ULOG_SCALE 1024

struct ulog_extent {
  uint64_t off;
  uint64_t sector;
  uint64_t len;
};

struct ulog_place {
  struct blockdev dev;
  struct ulog_extent *ext;
  uint next;
  uint64_t size;
  uint ino;
  uint32_t fsid[2];
  uint64_t mtime;
};

#define ULOG_STALE 0
#define ULOG_CLEAN 1
#define ULOG_REPLAYED 2

struct ulog;

int ulog_replay(const struct ulog_place *at, uint32_t **orphans, uint *norphans);
struct ulog *ulog_open(const struct ulog_place *at);
void ulog_close(struct ulog *l);
void ulog_abandon(struct ulog *l);
struct bpinlist *ulog_pins(struct ulog *l);
int ulog_revoke(struct ulog *l, uint64_t sector, uint bytes);
uint64_t ulog_pending(struct ulog *l, uint norphans);
uint64_t ulog_room(struct ulog *l);
int ulog_commit(struct ulog *l, const uint32_t *orphans, uint norphans);

struct ulog_overlay;

struct ulog_overlay *ulog_overlay_open(const struct ulog_place *at);
void ulog_overlay_close(struct ulog_overlay *o);

#endif
