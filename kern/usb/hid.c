// HID REPORT DESCRIPTORS

#include "defs.h"
#include "hid.h"
#include "mutex.h"

#define MAXUSAGE 64
#define MAXPUSH 4
#define MAXID 16
#define MAXFIELDS 2048
#define NPARSE 4

enum { MAIN, GLOBAL, LOCAL };

enum {
  MAIN_INPUT = 8,
  MAIN_OUTPUT = 9,
  MAIN_COLLECTION = 10,
  MAIN_FEATURE = 11,
  MAIN_ENDCOLLECTION = 12,
};

enum {
  G_PAGE,
  G_LMIN,
  G_LMAX,
  G_PMIN,
  G_PMAX,
  G_UNITEXP,
  G_UNIT,
  G_SIZE,
  G_ID,
  G_COUNT,
  G_PUSH,
  G_POP,
};

enum {
  L_USAGE,
  L_USAGEMIN,
  L_USAGEMAX,
};

// what a push saves
struct hid_global {
  uint32_t page;
  int32_t lmin, lmax, pmin, pmax;
  uint32_t size, count;
  uint32_t id;
};

struct hid_data {
  const uint8_t *p, *end;
  enum hid_kind kind;
  struct hid_global g;
  struct hid_global stack[MAXPUSH];
  int level;
  struct {
    uint32_t id, pos;
  } pos[MAXID];
  uint32_t cur;
  int collevel;
  uint32_t usage_min[MAXUSAGE], usage_max[MAXUSAGE];
  int nusage, iusage;
  uint32_t ousage;
  uint32_t last_usage;
  uint32_t min, max;
  int havemin, havemax;
  uint32_t nfields, ifield;
  int used;
  uint32_t flags;
  enum hid_kind mkind;
};

static struct {
  struct mutex lock;
  int taken[NPARSE];
  struct hid_data data[NPARSE];
} parses;

void hid_init(void) { mtx_init(&parses.lock, "hid"); }

struct hid_data *hid_start_parse(const void *desc, int len, enum hid_kind kind) {
  struct hid_data *d = 0;
  int i;

  mtx_enter(&parses.lock);
  for (i = 0; i < NPARSE && d == 0; i++)
    if (!parses.taken[i]) {
      parses.taken[i] = 1;
      d = &parses.data[i];
    }
  mtx_leave(&parses.lock);
  if (d == 0) {
    printf("hid: no room to read another report descriptor\n");
    return 0;
  }
  memset(d, 0, sizeof(*d));
  d->p = desc;
  d->end = (const uint8_t *)desc + (len > 0 ? len : 0);
  d->kind = kind;
  return d;
}

void hid_end_parse(struct hid_data *d) {
  int i;

  if (d == 0)
    return;
  mtx_enter(&parses.lock);
  for (i = 0; i < NPARSE; i++)
    if (&parses.data[i] == d)
      parses.taken[i] = 0;
  mtx_leave(&parses.lock);
}

static int slot(struct hid_data *d, uint32_t id) {
  int i;

  if (id == 0)
    return 0;
  for (i = 1; i < MAXID; i++)
    if (d->pos[i].id == id || d->pos[i].id == 0)
      return i;
  return -1;
}

static void report_id(struct hid_data *d, uint32_t id) {
  int i;

  if (d->g.id == id)
    return;
  if ((i = slot(d, d->g.id)) >= 0) {
    d->pos[i].id = d->g.id;
    d->pos[i].pos = d->cur;
  }
  d->g.id = id;
  if ((i = slot(d, id)) >= 0) {
    d->pos[i].id = id;
    d->cur = d->pos[i].pos;
  } else {
    d->cur = 0;
  }
}

static void clear_local(struct hid_data *d) {
  d->nfields = d->ifield = 0;
  d->nusage = d->iusage = 0;
  d->ousage = 0;
  d->havemin = d->havemax = 0;
  d->used = 0;
}

static uint32_t read_le(struct hid_data *d, int n) {
  uint32_t v = 0;
  int i;

  for (i = 0; i < n && d->p < d->end; i++)
    v |= (uint32_t)*d->p++ << (8 * i);
  return v;
}

static uint32_t with_page(struct hid_data *d, uint32_t v, int size) {
  return size == 4 ? v : d->g.page | v;
}

static void add_usage(struct hid_data *d, uint32_t lo, uint32_t hi) {
  if (d->nusage == MAXUSAGE)
    return;
  d->usage_min[d->nusage] = lo;
  d->usage_max[d->nusage] = hi;
  d->nusage++;
}

static void local_item(struct hid_data *d, int tag, uint32_t v, int size) {
  switch (tag) {
  case L_USAGE:
    d->last_usage = with_page(d, v, size);
    add_usage(d, d->last_usage, d->last_usage);
    d->havemin = d->havemax = 0;
    break;
  case L_USAGEMIN:
    d->min = with_page(d, v, size);
    d->havemin = 1;
    break;
  case L_USAGEMAX:
    d->max = with_page(d, v, size);
    d->havemax = 1;
    break;
  }
  if (d->havemin && d->havemax) {
    if (d->min <= d->max)
      add_usage(d, d->min, d->max);
    d->havemin = d->havemax = 0;
  }
}

static void global_item(struct hid_data *d, int tag, uint32_t u, int32_t v) {
  struct hid_global saved;

  switch (tag) {
  case G_PAGE:
    d->g.page = u << 16;
    break;
  case G_LMIN:
    d->g.lmin = v;
    break;
  case G_LMAX:
    d->g.lmax = v;
    break;
  case G_PMIN:
    d->g.pmin = v;
    break;
  case G_PMAX:
    d->g.pmax = v;
    break;
  case G_SIZE:
    d->g.size = u;
    break;
  case G_ID:
    report_id(d, (uint32_t)v);
    break;
  case G_COUNT:
    d->g.count = u;
    break;
  case G_PUSH:
    if (d->level < MAXPUSH) {
      d->stack[d->level++] = d->g;
    }
    break;
  case G_POP:
    if (d->level > 0) {
      saved = d->stack[--d->level];
      report_id(d, saved.id);
      d->g = saved;
    }
    break;
  }
}

static int give(struct hid_data *d, struct hid_item *h) {
  uint32_t usage;

  if (d->iusage < d->nusage) {
    usage = d->usage_min[d->iusage] + d->ousage;
    d->last_usage = usage;
    if (usage == d->usage_max[d->iusage]) {
      d->iusage++;
      d->ousage = 0;
    } else {
      d->ousage++;
    }
  } else {
    usage = d->last_usage;
  }
  d->ifield++;
  if (d->kind != hid_all && d->kind != d->mkind)
    return 0;
  memset(h, 0, sizeof(*h));
  h->usage_page = d->g.page;
  h->logical_minimum = d->g.lmin;
  h->logical_maximum = d->g.lmax;
  h->physical_minimum = d->g.pmin;
  h->physical_maximum = d->g.pmax;
  h->report_ID = d->g.id;
  h->usage = usage;
  h->collevel = d->collevel;
  h->kind = d->mkind;
  h->flags = d->flags;
  h->loc.size = d->g.size;
  h->loc.count = (d->flags & HIO_VARIABLE) ? 1 : d->g.count;
  h->loc.pos = d->cur;
  d->cur += h->loc.size * h->loc.count;
  return 1;
}

static void collection_item(struct hid_data *d, struct hid_item *h,
                            enum hid_kind kind, uint32_t v) {
  memset(h, 0, sizeof(*h));
  h->kind = kind;
  h->collection = v;
  h->usage = d->last_usage;
  h->report_ID = d->g.id;
  h->collevel = d->collevel;
}

int hid_get_item(struct hid_data *d, struct hid_item *h) {
  uint32_t u;
  int32_t v;
  int size, type, tag, prefix;

  if (d == 0)
    return 0;
  for (;;) {
    while (d->ifield < d->nfields)
      if (give(d, h))
        return 1;
    if (d->used)
      clear_local(d);
    if (d->p >= d->end)
      return 0;
    prefix = *d->p++;
    if (prefix == 0xfe) {
      size = (int)read_le(d, 1);
      read_le(d, 1);
      d->p = d->p + size > d->end ? d->end : d->p + size;
      continue;
    }
    tag = prefix >> 4;
    type = (prefix >> 2) & 3;
    size = prefix & 3;
    if (size == 3)
      size = 4;
    u = read_le(d, size);
    v = size == 1 ? (int8_t)u : size == 2 ? (int16_t)u : (int32_t)u;
    if (type == GLOBAL) {
      global_item(d, tag, u, v);
      continue;
    }
    if (type == LOCAL) {
      local_item(d, tag, u, size);
      continue;
    }
    if (type != MAIN)
      continue;
    switch (tag) {
    case MAIN_INPUT:
    case MAIN_OUTPUT:
    case MAIN_FEATURE:
      d->mkind = tag == MAIN_INPUT ? hid_input
                 : tag == MAIN_OUTPUT ? hid_output
                                      : hid_feature;
      d->flags = u;
      d->ifield = 0;
      d->nfields = (u & HIO_VARIABLE) ? d->g.count : 1;
      if (d->nfields > MAXFIELDS)
        d->nfields = MAXFIELDS;
      d->used = 1;
      break;
    case MAIN_COLLECTION:
      d->collevel++;
      collection_item(d, h, hid_collection, u);
      d->used = 1;
      return 1;
    case MAIN_ENDCOLLECTION:
      if (d->collevel == 0)
        return 0;
      d->collevel--;
      collection_item(d, h, hid_endcollection, 0);
      d->used = 1;
      return 1;
    }
  }
}

static uint32_t field(const uint8_t *report, int len, const struct hid_location *loc,
                      int is_signed) {
  uint32_t size = loc->size, pos = loc->pos, data = 0, at;
  int n, i;

  if (size == 0)
    return 0;
  if (size > 32)
    size = 32;
  n = (int)((size + 7) / 8);
  at = pos / 8;
  for (i = 0; i < n; i++)
    if (at + (uint32_t)i < (uint32_t)len)
      data |= (uint32_t)report[at + (uint32_t)i] << (8 * i);
  data >>= pos % 8;
  if (is_signed)
    return (uint32_t)((int32_t)(data << (32 - size)) >> (32 - size));
  return (data << (32 - size)) >> (32 - size);
}

int32_t hid_get_data(const uint8_t *report, int len, const struct hid_location *loc) {
  return (int32_t)field(report, len, loc, 1);
}

uint32_t hid_get_udata(const uint8_t *report, int len, const struct hid_location *loc) {
  return field(report, len, loc, 0);
}

int hid_report_size(const void *desc, int len, enum hid_kind kind, uint8_t id) {
  struct hid_data *d = hid_start_parse(desc, len, kind);
  struct hid_item h;
  int lo = -1, hi = -1;

  while (hid_get_item(d, &h))
    if (h.report_ID == id && h.kind == kind) {
      if (lo < 0)
        lo = (int)h.loc.pos;
      hi = (int)(h.loc.pos + h.loc.size * h.loc.count);
    }
  hid_end_parse(d);
  return (hi - lo + 7) / 8;
}

int hid_locate(const void *desc, int len, uint32_t usage, uint8_t id,
               enum hid_kind kind, struct hid_location *loc, uint32_t *flags) {
  struct hid_data *d = hid_start_parse(desc, len, kind);
  struct hid_item h;

  while (hid_get_item(d, &h))
    if (h.kind == kind && !(h.flags & HIO_CONST) && h.usage == usage &&
        h.report_ID == id) {
      if (loc != 0)
        *loc = h.loc;
      if (flags != 0)
        *flags = h.flags;
      hid_end_parse(d);
      return 1;
    }
  hid_end_parse(d);
  if (loc != 0)
    loc->size = 0;
  if (flags != 0)
    *flags = 0;
  return 0;
}

int hid_is_collection(const void *desc, int len, uint8_t id, int32_t usage) {
  struct hid_data *d = hid_start_parse(desc, len, hid_all);
  struct hid_item h;
  uint32_t app = ~0u;

  while (hid_get_item(d, &h)) {
    if (h.kind == hid_collection && h.collection == HCOLL_APPLICATION)
      app = h.usage;
    if (h.kind == hid_endcollection && app == (uint32_t)usage &&
        h.report_ID == id) {
      hid_end_parse(d);
      return 1;
    }
  }
  hid_end_parse(d);
  return 0;
}
