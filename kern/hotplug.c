// HOTPLUG

#include "defs.h"
#include "devfs.h"
#include "file.h"
#include "mmu.h"
#include "uio.h"
#include "inc/abi.h"
#include "inc/stat.h"
#include "param.h"
#include "proc.h"
#include "mutex.h"

#define NHOTPLUGEVENT 16
#define NHOTPLUGCLIENT 8

struct client {
  pid_t pid;
  int opens;
  struct fnu_hotplug_event event[NHOTPLUGEVENT];
  int head;
  int tail;
  int count;
  int lost;
  struct selinfo sel;
};

static struct {
  struct mutex lock;
  struct client client[NHOTPLUGCLIENT];
  struct client backlog;
} hotplug;

static int hotplugopen(struct inode *ip, int omode);
static void hotplugclose(struct inode *ip);
static int hotplugread(struct inode *ip, struct uio *uio);
static int hotplugpoll(struct inode *ip, int want);

void hotpluginit(void) {
  mtx_init(&hotplug.lock, "hotplug");
  devsw[HOTPLUG].open = hotplugopen;
  devsw[HOTPLUG].close = hotplugclose;
  devsw[HOTPLUG].read = hotplugread;
  devsw[HOTPLUG].poll = hotplugpoll;
  devfs_add("hotplug", FNU_TYPE_DEVICE, HOTPLUG, 0, 0400);
}

// call with the lock held
static void enqueue(struct client *c, const struct fnu_hotplug_event *e) {
  c->event[c->head] = *e;
  c->head = (c->head + 1) % NHOTPLUGEVENT;
  if (c->count == NHOTPLUGEVENT) {
    c->tail = (c->tail + 1) % NHOTPLUGEVENT;
    if (c->pid > 0 && c->lost++ == 0)
      printf("hotplug: pid %d is not reading /dev/hotplug, its events are "
              "being dropped\n", c->pid);
  } else {
    c->count++;
  }
}

static struct client *client_of(pid_t pid) {
  int i;

  for (i = 0; i < NHOTPLUGCLIENT; i++)
    if (hotplug.client[i].opens > 0 && hotplug.client[i].pid == pid)
      return &hotplug.client[i];
  return 0;
}

static void put(int type, int devclass, const char *name, uint flags, uint dev) {
  struct fnu_hotplug_event e;
  struct client *c;
  int i;

  memset(&e, 0, sizeof(e));
  e.type = type;
  e.devclass = devclass;
  e.flags = flags;
  e.dev = dev;
  strlcpy(e.name, (char *)name, FNU_HOTPLUG_NAMELEN);
  enqueue(&hotplug.backlog, &e);
  for (i = 0; i < NHOTPLUGCLIENT; i++) {
    c = &hotplug.client[i];
    if (c->opens > 0) {
      enqueue(c, &e);
      selwakeup(&c->sel);
    }
  }
  wakeup(&hotplug.client);
}

void hotplug_attach(int devclass, const char *name, uint flags, uint dev) {
  mtx_enter(&hotplug.lock);
  put(FNU_HOTPLUG_ATTACH, devclass, name, flags, dev);
  mtx_leave(&hotplug.lock);
}

void hotplug_detach(int devclass, const char *name, uint flags, uint dev) {
  mtx_enter(&hotplug.lock);
  put(FNU_HOTPLUG_DETACH, devclass, name, flags, dev);
  mtx_leave(&hotplug.lock);
}

static void reap(void) {
  struct fnu_procids ids;
  int i;

  for (i = 0; i < NHOTPLUGCLIENT; i++)
    if (hotplug.client[i].opens > 0 && proc_ids(hotplug.client[i].pid, &ids) != 0)
      hotplug.client[i].opens = 0;
}

static int hotplugopen(struct inode *ip __unused, int omode __unused) {
  struct client *c;
  int i;

  reap();
  mtx_enter(&hotplug.lock);
  if ((c = client_of(curproc->p_p->pid)) != 0) {
    c->opens++;
    mtx_leave(&hotplug.lock);
    return 0;
  }
  for (i = 0; i < NHOTPLUGCLIENT; i++)
    if (hotplug.client[i].opens == 0) {
      c = &hotplug.client[i];
      *c = hotplug.backlog;
      c->pid = curproc->p_p->pid;
      c->opens = 1;
      c->lost = 0;
      mtx_leave(&hotplug.lock);
      return 0;
    }
  mtx_leave(&hotplug.lock);
  return FNU_ERR_BUSY;
}

static void hotplugclose(struct inode *ip __unused) {
  struct client *c;

  mtx_enter(&hotplug.lock);
  if ((c = client_of(curproc->p_p->pid)) != 0)
    c->opens--;
  mtx_leave(&hotplug.lock);
}

static int hotplugpoll(struct inode *ip __unused, int want) {
  struct client *c;
  int has;

  mtx_enter(&hotplug.lock);
  c = client_of(curproc->p_p->pid);
  has = c != 0 && c->count > 0;
  if (c != 0 && !has)
    selrecord(&c->sel);
  mtx_leave(&hotplug.lock);
  return has ? want & FNU_SEL_READ : 0;
}

static int hotplugread(struct inode *ip, struct uio *uio) {
  struct fnu_hotplug_event e;
  struct client *c;

  if (uio->uio_resid != sizeof(e))
    return -FNU_ERR_INVALID;

  iunlock(ip);
  mtx_enter(&hotplug.lock);
  for (;;) {
    if ((c = client_of(curproc->p_p->pid)) == 0 || proc_interrupted()) {
      mtx_leave(&hotplug.lock);
      ilock(ip);
      return c == 0 ? -FNU_ERR_BADHANDLE : -FNU_ERR_INTERRUPTED;
    }
    if (c->count > 0)
      break;
    msleep(&hotplug.client, &hotplug.lock, PZERO | PCATCH, "hotplug", 0);
  }
  e = c->event[c->tail];
  c->tail = (c->tail + 1) % NHOTPLUGEVENT;
  c->count--;
  mtx_leave(&hotplug.lock);
  ilock(ip);

  if (uiomove(&e, sizeof(e), uio) != 0)
    return -FNU_ERR_FAULT;
  return (int)sizeof(e);
}
