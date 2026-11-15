// THE TABLES
#include "inc/fnu.h"

struct table_head {
  uint64 len;
  uint64 pad;
};

static void *table_alloc(uint64 bytes) {
  struct table_head *h;
  uint64 len = (sizeof(*h) + bytes + 4095) & ~(uint64)4095;
  long at = fnu_map(0, len, FNU_MEM_READ | FNU_MEM_WRITE,
                     FNU_MEM_PRIVATE | FNU_MEM_ANON, -1, 0);

  if (at < 0)
    return 0;
  h = (struct table_head *)at;
  h->len = len;
  return h + 1;
}

void fnu_table_free(void *table) {
  struct table_head *h;

  if (table == 0)
    return;
  h = (struct table_head *)table - 1;
  fnu_unmap(h, h->len);
}

static int whole(int (*call)(void *, int), uint64 size, void **table) {
  void *buf;
  int n, got;

  for (;;) {
    if ((n = call(0, 0)) < 0)
      return n;
    if ((buf = table_alloc(((uint64)n + 1) * size)) == 0)
      return -FNU_ERR_NOMEMORY;
    if ((got = call(buf, n + 1)) < 0) {
      fnu_table_free(buf);
      return got;
    }
    if (got <= n) {
      *table = buf;
      return got;
    }
    fnu_table_free(buf);
  }
}

static int volumes(void *buf, int n) { return fnu_fsstat(buf, n); }
static int disks(void *buf, int n) { return fnu_diskinfo(buf, n); }
static int processes(void *buf, int n) { return fnu_procinfo(buf, n); }
static int interfaces(void *buf, int n) { return fnu_netinfo(buf, n); }

int fnu_volumes(struct fnu_fsstat **table) {
  return whole(volumes, sizeof(**table), (void **)table);
}

int fnu_disks(struct fnu_diskinfo **table) {
  return whole(disks, sizeof(**table), (void **)table);
}

int fnu_processes(struct fnu_procinfo **table) {
  return whole(processes, sizeof(**table), (void **)table);
}

int fnu_interfaces(struct fnu_ifinfo **table) {
  return whole(interfaces, sizeof(**table), (void **)table);
}
