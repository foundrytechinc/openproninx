// AML NAMESPACE

#include "acpi.h"
#include "aml.h"
#include "aml_priv.h"
#include "defs.h"
#include "malloc.h"
#include "proc.h"
#include "sleeplock.h"

static struct aml_node root;
struct aml_value *aml_global_lock;
int aml_error;
int aml_depth;
uint8_t *aml_base;
static int evalid;
static int halting;

struct aml_node *aml_root(void) { return &root; }

// THE LOCK
// lock held

static struct {
  struct sleeplock lk;
  struct proc *owner;
  int count;
} big;

void aml_lock(void) {
  struct proc *p = curproc;

  if (halting || p == 0) {
    big.count++;
    return;
  }
  if (big.owner == p) {
    big.count++;
    return;
  }
  acquiresleep(&big.lk);
  big.owner = p;
  big.count = 1;
}

void aml_unlock(void) {
  struct proc *p = curproc;

  if (halting || p == 0) {
    big.count--;
    return;
  }
  if (big.owner != p)
    panic("aml_unlock: not the owner");
  if (--big.count == 0) {
    big.owner = 0;
    releasesleep(&big.lk);
  }
}

void aml_halt(void) {
  aml_lock();
  halting = 1;
}

int aml_cold(void) { return halting || curproc == 0; }

void aml_msleep(int ms) {
  if (ms < 1)
    ms = 1;
  if (aml_cold())
    delay((uint)ms * 1000);
  else
    tsleep(&evalid, PWAIT, "amlsleep", (ms * HZ + 999) / 1000);
}

// NAMES

struct aml_node *aml_search_seg(struct aml_node *rt, const uint8_t *seg,
                                int create) {
  struct aml_node *node, *last = 0;

  if (rt == 0)
    return 0;
  for (node = rt->son; node != 0; node = node->sib) {
    if (strncmp(node->name, (const char *)seg, AML_NAMESEG_LEN) == 0)
      return node;
    last = node;
  }
  if (!create)
    return 0;
  node = aml_alloc(sizeof(*node));
  memcpy(node->name, seg, AML_NAMESEG_LEN);
  node->value = aml_newvalue(AML_UNINIT, 0, 0);
  node->value->node = node;
  node->parent = rt;
  if (last != 0)
    last->sib = node;
  else
    rt->son = node;
  return node;
}

static void nodepath(struct aml_node *node, char *buf, size_t size) {
  if (node == 0 || node == &root)
    return;
  nodepath(node->parent, buf, size);
  if (node->parent != &root && strlen(buf) + 1 < size)
    strncpy(buf + strlen(buf), ".", size - strlen(buf) - 1);
  if (strlen(buf) + AML_NAMESEG_LEN < size)
    strncpy(buf + strlen(buf), node->name, size - strlen(buf) - 1);
}

const char *aml_nodename(struct aml_node *node) {
  static char buf[128];

  memset(buf, 0, sizeof(buf));
  if (node == &root)
    buf[0] = '\\';
  else
    nodepath(node, buf, sizeof(buf));
  return buf;
}

const char *aml_getname(const uint8_t *name) {
  static char buf[128];
  char *p = buf;
  int count;

  while (*name == AMLOP_ROOTCHAR || *name == AMLOP_PARENTPREFIX)
    *p++ = (char)*name++;
  switch (*name) {
  case 0x00:
    count = 0;
    break;
  case AMLOP_MULTINAMEPREFIX:
    count = name[1];
    name += 2;
    break;
  case AMLOP_DUALNAMEPREFIX:
    count = 2;
    name += 1;
    break;
  default:
    count = 1;
  }
  while (count-- > 0 && p + 6 < buf + sizeof(buf)) {
    memcpy(p, name, 4);
    p[4] = '.';
    p += 5;
    name += 4;
    if (*name == '.')
      name++;
  }
  if (p > buf && p[-1] == '.')
    p--;
  *p = 0;
  return buf;
}

const char *aml_eisaid(uint32_t pid) {
  static const char hex[] = "0123456789ABCDEF";
  static char id[8];

  id[0] = (char)('@' + ((pid >> 2) & 0x1F));
  id[1] = (char)('@' + ((pid << 3) & 0x18) + ((pid >> 13) & 0x7));
  id[2] = (char)('@' + ((pid >> 8) & 0x1F));
  id[3] = hex[(pid >> 20) & 0xF];
  id[4] = hex[(pid >> 16) & 0xF];
  id[5] = hex[(pid >> 28) & 0xF];
  id[6] = hex[(pid >> 24) & 0xF];
  id[7] = 0;
  return id;
}

struct aml_node *aml_search_create(struct aml_node *rt, const void *vname,
                                   int create) {
  const char *name = vname;
  uint8_t seg[AML_NAMESEG_LEN];
  int i;

  while (*name == AMLOP_ROOTCHAR) {
    rt = &root;
    name++;
  }
  while (*name != 0 && rt != 0) {
    memset(seg, '_', sizeof(seg));
    for (i = 0; i < AML_NAMESEG_LEN && *name != 0 && *name != '.'; i++)
      seg[i] = (uint8_t)*name++;
    if (*name == '.')
      name++;
    rt = aml_search_seg(rt, seg, create);
  }
  return rt;
}

struct aml_node *aml_search(struct aml_node *start, const char *name) {
  return aml_search_create(start, name, 0);
}

struct aml_node *aml_search_up(struct aml_node *start, const char *name) {
  struct aml_node *res;

  for (; start != 0; start = start->parent)
    if ((res = aml_search(start, name)) != 0)
      return res;
  return 0;
}

uint8_t *aml_parsename(struct aml_node *inode, uint8_t *pos,
                       struct aml_value **rval, int create) {
  struct aml_node *rel, *node = inode;
  uint8_t *start = pos;
  int i;

  if (*pos == AMLOP_ROOTCHAR) {
    pos++;
    node = &root;
  }
  while (*pos == AMLOP_PARENTPREFIX) {
    pos++;
    if ((node = node->parent) == 0)
      node = &root;
  }
  switch (*pos) {
  case 0x00:
    pos++;
    break;
  case AMLOP_MULTINAMEPREFIX:
    for (i = 0; i < pos[1]; i++)
      node = aml_search_seg(node, pos + 2 + i * AML_NAMESEG_LEN, create);
    pos += 2 + pos[1] * AML_NAMESEG_LEN;
    break;
  case AMLOP_DUALNAMEPREFIX:
    node = aml_search_seg(node, pos + 1, create);
    node = aml_search_seg(node, pos + 1 + AML_NAMESEG_LEN, create);
    pos += 1 + 2 * AML_NAMESEG_LEN;
    break;
  default:
    rel = node;
    do {
      node = aml_search_seg(rel, pos, create);
      rel = rel->parent;
    } while (node == 0 && pos == start && rel != 0);
    pos += AML_NAMESEG_LEN;
    break;
  }
  if (node != 0) {
    *rval = node->value;
    if (!create && (*rval)->type == AML_OBJREF &&
        (*rval)->objref.type == AMLOP_ALIAS && (*rval)->objref.ref != 0)
      *rval = (*rval)->objref.ref;
    aml_addref(*rval);
  } else {
    *rval = aml_newvalue(AML_NAMEREF, 0, start);
  }
  return pos;
}

void aml_delchildren(struct aml_node *node) {
  struct aml_node *child;

  if (node == 0)
    return;
  while ((child = node->son) != 0) {
    node->son = child->sib;
    aml_delchildren(child);
    if (child->value != 0 && child->value->refs > 1)
      child->value->node = 0;
    aml_delref(&child->value);
    aml_release(child);
  }
}

void aml_walk(struct aml_node *node, int post,
              int (*cb)(struct aml_node *, void *), void *arg) {
  struct aml_node *child;

  if (node == 0)
    return;
  if (!post && cb(node, arg))
    return;
  for (child = node->son; child != 0; child = child->sib)
    aml_walk(child, post, cb, arg);
  if (post)
    cb(node, arg);
}

void aml_find(struct aml_node *node, const char *name,
              int (*cb)(struct aml_node *, void *), void *arg) {
  struct aml_node *child;

  for (child = node->son; child != 0; child = child->sib)
    if (strcmp(name, child->name) == 0 && cb(child, arg) != 0)
      return;
  for (child = node->son; child != 0; child = child->sib)
    aml_find(child, name, cb, arg);
}

static int fixup(struct aml_node *node, void *arg) {
  struct aml_value *val = arg;
  int i;

  if (node->value == 0)
    return 0;
  if (val == 0) {
    fixup(node, node->value);
  } else if (val->type == AML_NAMEREF) {
    node = aml_search_up(node, aml_getname(val->nameref.name));
    if (node != 0 && node->value != 0)
      aml_setvalue(val, AML_OBJREF, AMLOP_NAMECHAR, node->value);
  } else if (val->type == AML_PACKAGE) {
    for (i = 0; i < val->length; i++)
      fixup(node, val->pkg[i]);
  }
  return 0;
}

void aml_fixup(void) { aml_walk(&root, 0, fixup, 0); }

// NOTIFY

struct notify {
  struct aml_node *node;
  void (*fn)(struct aml_node *, int, void *);
  void *arg;
  struct notify *next;
};

static struct notify *notifies;

void aml_notify_register(struct aml_node *node,
                         void (*fn)(struct aml_node *, int, void *),
                         void *arg) {
  struct notify *n = aml_alloc(sizeof(*n));

  n->node = node;
  n->fn = fn;
  n->arg = arg;
  n->next = notifies;
  notifies = n;
}

static void notify_task(void *node, int value) {
  struct notify *n;

  for (n = notifies; n != 0; n = n->next)
    if (n->node == node)
      n->fn(n->node, value, n->arg);
}

void aml_notify(struct aml_node *node, int value) {
  if (node == 0)
    return;
  acpi_task_add(notify_task, node, value);
}

void aml_notify_post(struct aml_node *node, int value) { aml_notify(node, value); }

// THE REGIONS A DRIVER OWNS

void aml_region_default(int space, void *cookie,
                        int (*fn)(void *, int, uint64_t, int, uint64_t *)) {
  aml_regions[space].cookie = cookie;
  aml_regions[space].fn = fn;
}

void aml_region_register(struct aml_node *dev, int space, void *cookie,
                         int (*fn)(void *, int, uint64_t, int, uint64_t *)) {
  struct aml_region_handler *h = aml_alloc(sizeof(*h));
  struct aml_value arg[2];
  struct aml_node *reg;

  h->dev = dev;
  h->space = space;
  h->cookie = cookie;
  h->fn = fn;
  h->next = aml_device_regions;
  aml_device_regions = h;
  memset(arg, 0, sizeof(arg));
  arg[0].type = AML_INTEGER;
  arg[0].integer = space;
  arg[1].type = AML_INTEGER;
  arg[1].integer = 1;
  if ((reg = aml_search(dev, "_REG")) != 0)
    aml_eval(reg, 2, arg, 0);
}

// WHAT EVERY MACHINE HAS

static const char *const osi_names[] = {
    "Windows 2000",       "Windows 2001",    "Windows 2001.1",
    "Windows 2001.1 SP1", "Windows 2001 SP0", "Windows 2001 SP1",
    "Windows 2001 SP2",   "Windows 2001 SP3", "Windows 2001 SP4",
    "Windows 2006",       "Windows 2006.1",  "Windows 2006 SP1",
    "Windows 2006 SP2",   "Windows 2009",    "Windows 2012",
    "Windows 2013",       "Windows 2015",    "Windows 2016",
    "Windows 2017",       "Windows 2017.2",  "Windows 2018",
    "Windows 2018.2",     "Windows 2019",    "Windows 2020",
    "Windows 2021",       "Windows 2022",
};

struct aml_value *aml_osi(struct aml_scope *scope) {
  struct aml_value *arg = aml_getstack(scope, AMLOP_ARG0);
  const char *vendor = hw_vendor_name();
  int i, result = 0;

  if (arg == 0 || arg->type != AML_STRING)
    return aml_newvalue(AML_INTEGER, 0, 0);
  if (vendor != 0 && (strcmp(vendor, "Apple Inc.") == 0 ||
                      strcmp(vendor, "Apple Computer, Inc.") == 0))
    return aml_newvalue(AML_INTEGER, strcmp(arg->str, "Darwin") == 0, 0);
  for (i = 0; i < (int)(sizeof(osi_names) / sizeof(osi_names[0])); i++)
    if (strcmp(arg->str, osi_names[i]) == 0) {
      result = 1;
      break;
    }
  return aml_newvalue(AML_INTEGER, result, 0);
}

static char osname[] = "Microsoft Windows NT";

static const struct {
  const char *name;
  int type;
  int64_t ival;
  const void *bval;
} defaults[] = {
    {"_OS_", AML_STRING, -1, osname},
    {"_REV", AML_INTEGER, 2, 0},
    {"_GL", AML_MUTEX, 1, 0},
    {"_OSI", AML_METHOD, 1, (const void *)aml_osi},
    {"_GPE", AML_DEVICE, 0, 0},
    {"_PR_", AML_DEVICE, 0, 0},
    {"_SB_", AML_DEVICE, 0, 0},
    {"_TZ_", AML_DEVICE, 0, 0},
    {"_SI_", AML_DEVICE, 0, 0},
};

void aml_defaults(void) {
  struct aml_value *tmp;
  uint i;

  root.name[0] = '\\';
  root.value = aml_newvalue(AML_UNINIT, 0, 0);
  root.value->node = &root;
  for (i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
    aml_parsename(&root, (uint8_t *)defaults[i].name, &tmp, 1);
    aml_setvalue(tmp, defaults[i].type, defaults[i].ival, defaults[i].bval);
    if (strcmp(defaults[i].name, "_GL") == 0)
      aml_global_lock = tmp;
    aml_delref(&tmp);
  }
}

// EVALUATING

int aml_eval_hid(struct aml_node *node, struct aml_value *val) {
  if (aml_eval_name(node, "_HID", 0, 0, val))
    return -1;
  if (val->type == AML_INTEGER) {
    int64_t id = val->integer;

    aml_setvalue(val, AML_STRING, -1, aml_eisaid((uint32_t)id));
  }
  return 0;
}

int aml_eval(struct aml_node *node, int argc, struct aml_value *argv,
             struct aml_value *res) {
  struct aml_value *xres;
  int err, saved;

  if (res != 0)
    memset(res, 0, sizeof(*res));
  if (node == 0 || node->value == 0)
    return 1;
  aml_lock();
  if (aml_depth == 0) {
    evalid++;
    aml_evalbegin();
  }
  aml_depth++;
  saved = aml_error;
  aml_error = 0;
  xres = aml_run(0, node->value, 't', argc, argv);
  if (xres != 0) {
    if (res != 0)
      aml_copyvalue(res, xres);
    if (xres != node->value)
      aml_delref(&xres);
  }
  err = aml_error;
  aml_error += saved;
  aml_depth--;
  aml_unlock();
  if (err) {
    printf("acpi: error evaluating %s\n", aml_nodename(node));
    return -1;
  }
  return 0;
}

int aml_set_int(struct aml_node *node, int64_t val) {
  struct aml_value env;

  if (node == 0)
    return 0;
  memset(&env, 0, sizeof(env));
  env.type = AML_INTEGER;
  env.integer = val;
  return aml_eval(node, 1, &env, 0);
}

int aml_eval_name(struct aml_node *parent, const char *name, int argc,
                  struct aml_value *argv, struct aml_value *res) {
  return aml_eval(aml_search(parent, name), argc, argv, res);
}

int aml_eval_int(struct aml_node *parent, const char *name, int argc,
                 struct aml_value *argv, int64_t *ival) {
  struct aml_value res;
  int rc = aml_eval_name(parent, name, argc, argv, &res);

  if (rc == 0) {
    *ival = aml_val2int(&res);
    aml_freevalue(&res);
  }
  return rc;
}

int aml_evalid(void) { return evalid; }

int aml_load(const uint8_t *aml, uint len, const char *rootpath) {
  struct aml_scope *scope;
  struct aml_value res;
  struct aml_node *where = &root;

  if (rootpath != 0 && (where = aml_search(&root, rootpath)) == 0) {
    printf("acpi: no %s to load a table in\n", rootpath);
    return -1;
  }
  aml_lock();
  aml_base = (uint8_t *)aml;
  memset(&res, 0, sizeof(res));
  res.type = AML_SCOPE;
  res.length = (int)len;
  res.buf = (uint8_t *)aml;
  aml_error = 0;
  scope = aml_pushscope(0, &res, where, AMLOP_SCOPE);
  aml_depth++;
  aml_parse(scope, 'T', "TopLevel");
  aml_depth--;
  aml_popscope(scope);
  aml_unlock();
  return aml_error ? -1 : 0;
}

void aml_init(void) {
  initsleeplock(&big.lk, "aml");
  aml_regions_init();
  aml_defaults();
}
