// WSMOUSE
#include "defs.h"
#include "devfs.h"
#include "file.h"
#include "malloc.h"
#include "mmu.h"
#include "uio.h"
#include "unit.h"
#include "inc/abi.h"
#include "inc/stat.h"
#include "param.h"
#include "proc.h"
#include "mutex.h"

#define WSEVENT_QSIZE 256

struct wsmouse_softc {
  struct wsmouse_softc *next;
  int unit;
  char name[16];
  int attached;
  int readers;
  int opens;
  struct fnu_wsevent event[WSEVENT_QSIZE];
  int head, tail, count, lost;
  uint32 down;
  uint32 type;
  int touchpad;
  int native;
  struct fnu_wsmouse_calib calib;
  int x, y, p, w, contacts; // as last sent
  struct selinfo sel;
};

static struct {
  struct mutex lock;
  struct wsmouse_softc *list;
  struct unrhdr units;
} ws;

static int wsmouseopen(struct inode *ip, int omode);
static void wsmouseclose(struct inode *ip);
static int wsmouseread(struct inode *ip, struct uio *uio);
static int wsmouseioctl(struct inode *ip, uint64_t cmd, uint64_t arg);
static int wsmousepoll(struct inode *ip, int want);

void wsmouseinit(void) {
  mtx_init(&ws.lock, "wsmouse");
  init_unrhdr(&ws.units, "wsmouseunit");
  devsw[WSMOUSE].class = FNU_DEVCLASS_MOUSE;
  devsw[WSMOUSE].open = wsmouseopen;
  devsw[WSMOUSE].close = wsmouseclose;
  devsw[WSMOUSE].read = wsmouseread;
  devsw[WSMOUSE].ioctl = wsmouseioctl;
  devsw[WSMOUSE].poll = wsmousepoll;
}

// with the lock held
static struct wsmouse_softc *unit_sc(int unit) {
  struct wsmouse_softc *sc;

  for (sc = ws.list; sc != 0; sc = sc->next)
    if (sc->unit == unit)
      return sc;
  return 0;
}

// lock held
static int unlink_if_done(struct wsmouse_softc *sc) {
  struct wsmouse_softc **pp;

  if (sc->attached || sc->readers > 0 || sc->opens > 0)
    return 0;
  for (pp = &ws.list; *pp != 0; pp = &(*pp)->next)
    if (*pp == sc) {
      *pp = sc->next;
      break;
    }
  return 1;
}

// with the lock held
static void put(struct wsmouse_softc *sc, uint32 type, int32_t value,
                const struct fnu_timespec *now) {
  struct fnu_wsevent *e = &sc->event[sc->head];

  e->type = type;
  e->value = value;
  e->time = *now;
  sc->head = (sc->head + 1) % WSEVENT_QSIZE;
  if (sc->count == WSEVENT_QSIZE) {
    sc->tail = (sc->tail + 1) % WSEVENT_QSIZE;
    sc->lost++;
  } else
    sc->count++;
}

void wsmouse_input(int unit, int dx, int dy, int dz, uint32 buttons) {
  struct wsmouse_softc *sc;
  struct fnu_timespec now;
  uint32 changed;
  int i;

  time_now(&now);
  mtx_enter(&ws.lock);
  if ((sc = unit_sc(unit)) == 0 || !sc->attached || sc->native) {
    mtx_leave(&ws.lock);
    return;
  }
  if (dx)
    put(sc, FNU_WSEVENT_MOUSE_DELTA_X, dx, &now);
  if (dy)
    put(sc, FNU_WSEVENT_MOUSE_DELTA_Y, dy, &now);
  if (dz)
    put(sc, FNU_WSEVENT_MOUSE_DELTA_Z, dz, &now);
  changed = buttons ^ sc->down;
  for (i = 0; i < 32 && changed; i++)
    if (changed & (1u << i)) {
      put(sc, buttons & (1u << i) ? FNU_WSEVENT_MOUSE_DOWN : FNU_WSEVENT_MOUSE_UP, i, &now);
      changed &= ~(1u << i);
    }
  sc->down = buttons;
  wakeup(sc);
  selwakeup(&sc->sel);
  mtx_leave(&ws.lock);
}

// kind of pointer; a touchpad with its range
void wsmouse_set_hw(int unit, uint32 type, const struct fnu_wsmouse_calib *calib) {
  struct wsmouse_softc *sc;

  mtx_enter(&ws.lock);
  if ((sc = unit_sc(unit)) != 0) {
    sc->type = type;
    sc->touchpad = calib != 0;
    if (calib != 0)
      sc->calib = *calib;
  }
  mtx_leave(&ws.lock);
}

// NATIVE: first finger, pressure, width, count, SYNC
void wsmouse_touch(int unit, int x, int y, int p, int w, int contacts, uint32 buttons) {
  struct wsmouse_softc *sc;
  struct fnu_timespec now;
  uint32 changed;
  int i;

  time_now(&now);
  mtx_enter(&ws.lock);
  if ((sc = unit_sc(unit)) == 0 || !sc->attached || !sc->native) {
    mtx_leave(&ws.lock);
    return;
  }
  if (x >= 0 && x != sc->x)
    put(sc, FNU_WSEVENT_MOUSE_ABSOLUTE_X, sc->x = x, &now);
  if (y >= 0 && y != sc->y)
    put(sc, FNU_WSEVENT_MOUSE_ABSOLUTE_Y, sc->y = y, &now);
  if (p != sc->p)
    put(sc, FNU_WSEVENT_TOUCH_PRESSURE, sc->p = p, &now);
  if (w != sc->w)
    put(sc, FNU_WSEVENT_TOUCH_WIDTH, sc->w = w, &now);
  if (contacts != sc->contacts)
    put(sc, FNU_WSEVENT_TOUCH_CONTACTS, sc->contacts = contacts, &now);
  changed = buttons ^ sc->down;
  for (i = 0; i < 32 && changed; i++)
    if (changed & (1u << i)) {
      put(sc, buttons & (1u << i) ? FNU_WSEVENT_MOUSE_DOWN : FNU_WSEVENT_MOUSE_UP, i, &now);
      changed &= ~(1u << i);
    }
  sc->down = buttons;
  put(sc, FNU_WSEVENT_SYNC, 0, &now);
  wakeup(sc);
  selwakeup(&sc->sel);
  mtx_leave(&ws.lock);
}

// with the lock held
static void set_mode(struct wsmouse_softc *sc, int native, const struct fnu_timespec *now) {
  if (sc->native == native)
    return;
  sc->native = native;
  sc->x = sc->y = sc->p = sc->w = sc->contacts = 0;
  if (native)
    put(sc, FNU_WSEVENT_TOUCH_RESET, 0, now);
}

static int wsmouseopen(struct inode *ip, int omode __unused) {
  struct wsmouse_softc *sc;
  int error = 0;

  mtx_enter(&ws.lock);
  if ((sc = unit_sc(ip->minor)) == 0 || !sc->attached)
    error = FNU_ERR_NODEVICE;
  else
    sc->opens++;
  mtx_leave(&ws.lock);
  return error;
}

static void wsmouseclose(struct inode *ip) {
  struct wsmouse_softc *sc;
  struct fnu_timespec now;
  int done = 0;

  time_now(&now);
  mtx_enter(&ws.lock);
  if ((sc = unit_sc(ip->minor)) != 0 && sc->opens > 0 && --sc->opens == 0) {
    set_mode(sc, 0, &now);
    done = unlink_if_done(sc);
  }
  mtx_leave(&ws.lock);
  if (done) {
    free_unr(&ws.units, sc->unit);
    free(sc, M_DEVBUF);
  }
}

static int wsmouseioctl(struct inode *ip, uint64_t cmd, uint64_t arg) {
  pte_t *pgdir = curproc->p_p->pgdir;
  struct wsmouse_softc *sc;
  struct fnu_wsmouse_calib calib;
  struct fnu_timespec now;
  uint32 type = 0;
  int mode, error = 0;

  if (cmd == FNU_WSMOUSESETMODE &&
      copyin(pgdir, &mode, (uintptr_t)arg, sizeof(mode)) < 0)
    return FNU_ERR_FAULT;
  time_now(&now);
  mtx_enter(&ws.lock);
  if ((sc = unit_sc(ip->minor)) == 0 || !sc->attached) {
    mtx_leave(&ws.lock);
    return FNU_ERR_NODEVICE;
  }
  switch (cmd) {
  case FNU_WSMOUSEGTYPE:
    type = sc->type;
    break;
  case FNU_WSMOUSEGCALIB:
    if (!sc->touchpad)
      error = FNU_ERR_BADREQUEST;
    calib = sc->calib;
    break;
  case FNU_WSMOUSESETMODE:
    if (mode != FNU_WSMOUSE_COMPAT && (mode != FNU_WSMOUSE_NATIVE || !sc->touchpad))
      error = FNU_ERR_INVALID;
    else
      set_mode(sc, mode == FNU_WSMOUSE_NATIVE, &now);
    break;
  default:
    error = FNU_ERR_BADREQUEST;
  }
  mtx_leave(&ws.lock);
  if (error != 0)
    return error;
  if (cmd == FNU_WSMOUSEGTYPE)
    return copyout(pgdir, (uintptr_t)arg, &type, sizeof(type)) < 0 ? FNU_ERR_FAULT : 0;
  if (cmd == FNU_WSMOUSEGCALIB)
    return copyout(pgdir, (uintptr_t)arg, &calib, sizeof(calib)) < 0 ? FNU_ERR_FAULT : 0;
  return 0;
}

const char *wsmouse_name(int unit) {
  struct wsmouse_softc *sc;
  const char *name;

  mtx_enter(&ws.lock);
  sc = unit_sc(unit);
  name = sc != 0 ? sc->name : "wsmouse?";
  mtx_leave(&ws.lock);
  return name;
}

int wsmouse_new(void) {
  struct wsmouse_softc *sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);

  sc->unit = alloc_unr(&ws.units);
  snprintf(sc->name, sizeof(sc->name), "wsmouse%d", sc->unit);
  mtx_enter(&ws.lock);
  sc->next = ws.list;
  ws.list = sc;
  mtx_leave(&ws.lock);
  return sc->unit;
}

void wsmouse_release(int unit) {
  struct wsmouse_softc *sc;
  int done;

  mtx_enter(&ws.lock);
  if ((sc = unit_sc(unit)) == 0) {
    mtx_leave(&ws.lock);
    return;
  }
  sc->attached = 0;
  done = unlink_if_done(sc);
  mtx_leave(&ws.lock);
  if (done) {
    free_unr(&ws.units, unit);
    free(sc, M_DEVBUF);
  }
}

void wsmouse_attach(int unit) {
  struct wsmouse_softc *sc;

  mtx_enter(&ws.lock);
  if ((sc = unit_sc(unit)) != 0)
    sc->attached = 1;
  mtx_leave(&ws.lock);
  if (sc != 0)
    devfs_add(sc->name, FNU_TYPE_DEVICE, WSMOUSE, unit, 0600);
}

void wsmouse_detach(int unit) {
  struct wsmouse_softc *sc;
  char name[16];

  mtx_enter(&ws.lock);
  if ((sc = unit_sc(unit)) == 0) {
    mtx_leave(&ws.lock);
    return;
  }
  strlcpy(name, sc->name, sizeof(name));
  mtx_leave(&ws.lock);
  devfs_remove(name);
  mtx_enter(&ws.lock);
  wakeup(sc);
  selwakeup(&sc->sel);
  mtx_leave(&ws.lock);
  wsmouse_release(unit);
}

static int wsmousepoll(struct inode *ip, int want) {
  struct wsmouse_softc *sc;
  int ready;

  mtx_enter(&ws.lock);
  sc = unit_sc(ip->minor);
  ready = sc == 0 || sc->count > 0 || !sc->attached;
  if (!ready)
    selrecord(&sc->sel);
  mtx_leave(&ws.lock);
  return ready ? want & FNU_SEL_READ : 0;
}

static int wsmouseread(struct inode *ip, struct uio *uio) {
  struct wsmouse_softc *sc;
  struct fnu_wsevent e;
  int got = 0, error = 0, done;

  if (uio->uio_resid < sizeof(e))
    return -FNU_ERR_INVALID;
  iunlock(ip);
  mtx_enter(&ws.lock);
  if ((sc = unit_sc(ip->minor)) == 0 || !sc->attached) {
    mtx_leave(&ws.lock);
    ilock(ip);
    return -FNU_ERR_IO;
  }
  sc->readers++;
  while (sc->count == 0 && error == 0) {
    if (proc_interrupted())
      error = -FNU_ERR_INTERRUPTED;
    else if (!sc->attached)
      error = -FNU_ERR_IO;
    else
      msleep(sc, &ws.lock, PZERO | PCATCH, "wsmouse", 0);
  }
  while (error == 0 && uio->uio_resid >= sizeof(e) && sc->count > 0) {
    e = sc->event[sc->tail];
    sc->tail = (sc->tail + 1) % WSEVENT_QSIZE;
    sc->count--;
    mtx_leave(&ws.lock);
    if (uiomove(&e, sizeof(e), uio) != 0)
      error = -FNU_ERR_FAULT;
    else
      got++;
    mtx_enter(&ws.lock);
  }
  sc->readers--;
  done = unlink_if_done(sc);
  mtx_leave(&ws.lock);
  if (done) {
    free_unr(&ws.units, sc->unit);
    free(sc, M_DEVBUF);
  }
  ilock(ip);
  return got > 0 ? got * (int)sizeof(e) : error;
}
