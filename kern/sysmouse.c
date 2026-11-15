// SYSMOUSE

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

#define SYSMOUSE_MAXFRAMES 250

static struct {
  struct mutex lock;
  struct selinfo sel;
  int open;
  int level;
  struct fnu_mousestatus status;
  uchar frame[SYSMOUSE_MAXFRAMES][FNU_MOUSE_SYS_PACKETSIZE];
  uint start, length;
} sm;

static void frame_store(const uchar *buf) {
  if (!sm.open || sm.length == SYSMOUSE_MAXFRAMES)
    return;
  memmove(sm.frame[(sm.start + sm.length) % SYSMOUSE_MAXFRAMES], buf,
          FNU_MOUSE_SYS_PACKETSIZE);
  sm.length++;
  wakeup(&sm.length);
  selwakeup(&sm.sel);
}

// MOUSE_BUTTONnDOWN -> MOUSE_MSC_BUTTONnUP
static const int buttonmap[8] = {
    FNU_MOUSE_MSC_BUTTON1UP | FNU_MOUSE_MSC_BUTTON2UP | FNU_MOUSE_MSC_BUTTON3UP,
    FNU_MOUSE_MSC_BUTTON2UP | FNU_MOUSE_MSC_BUTTON3UP,
    FNU_MOUSE_MSC_BUTTON1UP | FNU_MOUSE_MSC_BUTTON3UP,
    FNU_MOUSE_MSC_BUTTON3UP,
    FNU_MOUSE_MSC_BUTTON1UP | FNU_MOUSE_MSC_BUTTON2UP,
    FNU_MOUSE_MSC_BUTTON2UP,
    FNU_MOUSE_MSC_BUTTON1UP,
    0,
};

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void process_event(const struct fnu_mouse_info *mi) {
  uchar buf[FNU_MOUSE_SYS_PACKETSIZE];
  int x, y, iy, z;

  mtx_enter(&sm.lock);
  switch (mi->operation) {
  case FNU_MOUSE_ACTION:
    sm.status.button = mi->u.data.buttons;
    // fallthrough
  case FNU_MOUSE_MOTION_EVENT:
    x = mi->u.data.x;
    y = mi->u.data.y;
    z = mi->u.data.z;
    break;
  case FNU_MOUSE_BUTTON_EVENT:
    x = y = z = 0;
    if (mi->u.event.value > 0)
      sm.status.button |= mi->u.event.id;
    else
      sm.status.button &= ~mi->u.event.id;
    break;
  default:
    mtx_leave(&sm.lock);
    return;
  }

  sm.status.dx += x;
  sm.status.dy += y;
  sm.status.dz += z;
  sm.status.flags |= ((x || y || z) ? FNU_MOUSE_POSCHANGED : 0) |
                     (sm.status.obutton ^ sm.status.button);
  if (sm.status.flags == 0) {
    mtx_leave(&sm.lock);
    return;
  }

  buf[0] = (uchar)(FNU_MOUSE_MSC_SYNC | buttonmap[sm.status.button & FNU_MOUSE_STDBUTTONS]);
  x = clamp(x, -256, 255);
  buf[1] = (uchar)(x >> 1);
  buf[3] = (uchar)(x - (int8_t)buf[1]);
  iy = -clamp(y, -256, 255);
  buf[2] = (uchar)(iy >> 1);
  buf[4] = (uchar)(iy - (int8_t)buf[2]);
  z = clamp(z, -128, 127);
  buf[5] = (uchar)((z >> 1) & 0x7f);
  buf[6] = (uchar)((z - (z >> 1)) & 0x7f);
  buf[7] = (uchar)((~sm.status.button >> 3) & 0x7f);
  frame_store(buf);
  mtx_leave(&sm.lock);

  console_mouse_event(mi->operation, x, y, mi->u.event.id, mi->u.event.value);
}

static int consolectlioctl(struct inode *ip __unused, uint64_t cmd, uint64_t arg) {
  struct proc *p = curproc;
  struct fnu_mouse_info mi;

  if (cmd != FNU_CONSMOUSE)
    return FNU_ERR_BADREQUEST;
  if (copyin(p->p_p->pgdir, &mi, (uintptr_t)arg, sizeof(mi)) < 0)
    return FNU_ERR_FAULT;
  switch (mi.operation) {
  case FNU_MOUSE_SHOW:
    console_mouse_show(1);
    return 0;
  case FNU_MOUSE_HIDE:
    console_mouse_show(0);
    return 0;
  case FNU_MOUSE_MOVEABS:
  case FNU_MOUSE_MOVEREL:
    console_mouse_move(mi.u.data.x, mi.u.data.y, mi.operation == FNU_MOUSE_MOVEREL);
    return 0;
  case FNU_MOUSE_GETINFO:
    console_mouse_info(&mi.u.data.x, &mi.u.data.y, &mi.u.data.buttons);
    mi.u.data.z = 0;
    return copyout(p->p_p->pgdir, (uintptr_t)arg, &mi, sizeof(mi)) < 0 ? FNU_ERR_FAULT : 0;
  case FNU_MOUSE_ACTION:
  case FNU_MOUSE_MOTION_EVENT:
  case FNU_MOUSE_BUTTON_EVENT:
    process_event(&mi);
    return 0;
  default:
    return FNU_ERR_INVALID;
  }
}

static int sysmouseopen(struct inode *ip __unused, int omode __unused) {
  mtx_enter(&sm.lock);
  if (!sm.open) {
    sm.start = sm.length = 0;
    sm.level = 0;
  }
  sm.open++;
  mtx_leave(&sm.lock);
  return 0;
}

static void sysmouseclose(struct inode *ip __unused) {
  mtx_enter(&sm.lock);
  if (sm.open > 0 && --sm.open == 0)
    sm.level = 0;
  mtx_leave(&sm.lock);
}

static int sysmousepoll(struct inode *ip __unused, int want) {
  int has;

  mtx_enter(&sm.lock);
  if (!(has = sm.length > 0))
    selrecord(&sm.sel);
  mtx_leave(&sm.lock);
  return has ? want & FNU_SEL_READ : 0;
}

static int sysmouseread(struct inode *ip, struct uio *uio) {
  char frame[FNU_MOUSE_SYS_PACKETSIZE];
  int size, got = 0;

  iunlock(ip);
  mtx_enter(&sm.lock);
  size = sm.level >= 1 ? FNU_MOUSE_SYS_PACKETSIZE : FNU_MOUSE_MSC_PACKETSIZE;
  while (uio->uio_resid >= (size_t)size) {
    if (sm.length == 0) {
      if (got > 0)
        break;
      if (proc_interrupted()) {
        got = -1;
        break;
      }
      msleep(&sm.length, &sm.lock, PZERO | PCATCH, "sysmouse", 0);
      continue;
    }
    memmove(frame, sm.frame[sm.start], size);
    sm.start = (sm.start + 1) % SYSMOUSE_MAXFRAMES;
    sm.length--;
    mtx_leave(&sm.lock);
    if (uiomove(frame, (size_t)size, uio) != 0) {
      mtx_enter(&sm.lock);
      got = got > 0 ? got : -1;
      break;
    }
    mtx_enter(&sm.lock);
    got += size;
  }
  mtx_leave(&sm.lock);
  ilock(ip);
  return got;
}

static int sysmouseioctl(struct inode *ip __unused, uint64_t cmd, uint64_t arg) {
  struct proc *p = curproc;
  struct fnu_mousestatus st;
  int level;

  switch (cmd) {
  case FNU_MOUSEGETLEVEL:
    return copyout(p->p_p->pgdir, (uintptr_t)arg, &sm.level, sizeof(sm.level)) < 0 ? FNU_ERR_FAULT : 0;
  case FNU_MOUSESETLEVEL:
    if (copyin(p->p_p->pgdir, &level, (uintptr_t)arg, sizeof(level)) < 0)
      return FNU_ERR_FAULT;
    if (level != 0 && level != 1)
      return FNU_ERR_INVALID;
    sm.level = level;
    return 0;
  case FNU_MOUSESTATUS:
    mtx_enter(&sm.lock);
    st = sm.status;
    sm.status.flags = 0;
    sm.status.obutton = sm.status.button;
    sm.status.dx = sm.status.dy = sm.status.dz = 0;
    mtx_leave(&sm.lock);
    return copyout(p->p_p->pgdir, (uintptr_t)arg, &st, sizeof(st)) < 0 ? FNU_ERR_FAULT : 0;
  default:
    return FNU_ERR_BADREQUEST;
  }
}

void sysmouseinit(void) {
  mtx_init(&sm.lock, "sysmouse");
  devsw[CONSOLECTL].ioctl = consolectlioctl;
  devsw[SYSMOUSE].class = FNU_DEVCLASS_MOUSE;
  devsw[SYSMOUSE].open = sysmouseopen;
  devsw[SYSMOUSE].close = sysmouseclose;
  devsw[SYSMOUSE].read = sysmouseread;
  devsw[SYSMOUSE].ioctl = sysmouseioctl;
  devsw[SYSMOUSE].poll = sysmousepoll;
  devfs_add("consolectl", FNU_TYPE_DEVICE, CONSOLECTL, 0, 0600);
  devfs_add("sysmouse", FNU_TYPE_DEVICE, SYSMOUSE, 0, 0600);
}
