// MOUSED
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include <math.h>
#include <fnu/sys.h>
#include <err.h>
#include <util.h>

static int nodots(const struct dirent *d) {
  return strcmp(d->d_name, ".") != 0 && strcmp(d->d_name, "..") != 0;
}

static void freelist(struct dirent **list, int n) {
  int i;

  for (i = 0; i < n; i++)
    free(list[i]);
  free(list);
}

#define MAXRODENT 8
#define ZMAP_MAXBUTTON 4
#define DFLT_CLICKTHRESHOLD 500
#define DFLT_BUTTON2TIMEOUT 100
#define MAX_CLICKTHRESHOLD 2000
#define MAX_BUTTON2TIMEOUT 2000
#define DFLT_SCROLLTHRESHOLD 3
#define DFLT_SCROLLSPEED 2
#define BUTTON2_MAXMOVE 3
#define MOUSE_XAXIS (-1)
#define MOUSE_YAXIS (-2)

struct button_state {
  int count;
  int64_t ts;
};

struct btstate {
  int wmode;
  int clickthreshold;
  struct button_state bstate[FNU_MOUSE_MAXBUTTON];
  struct button_state *mstate[FNU_MOUSE_MAXBUTTON];
  int p2l[FNU_MOUSE_MAXBUTTON];
  int zmap[ZMAP_MAXBUTTON];
  struct button_state zstate[ZMAP_MAXBUTTON];
};

enum bt3_emul_state { S0, S1, S2, S3, S4, S5, S6, S7, S8, S9 };
#define A(b1, b3) (((b1) ? 2 : 0) | ((b3) ? 1 : 0))
#define A_TIMEOUT 4
#define S_DELAYED(st) (states[st].s[A_TIMEOUT] != (st))

static const struct {
  enum bt3_emul_state s[A_TIMEOUT + 1];
  int buttons;
  int mask;
  int timeout;
} states[10] = {
    {{S0, S2, S1, S3, S0}, 0, ~(FNU_MOUSE_BUTTON1DOWN | FNU_MOUSE_BUTTON3DOWN), 0},
    {{S4, S2, S1, S3, S5}, 0, ~FNU_MOUSE_BUTTON1DOWN, 0},
    {{S8, S2, S1, S3, S6}, 0, ~FNU_MOUSE_BUTTON3DOWN, 0},
    {{S0, S9, S9, S3, S3}, FNU_MOUSE_BUTTON2DOWN, ~0, 0},
    {{S0, S2, S1, S3, S0}, FNU_MOUSE_BUTTON1DOWN, ~0, 1},
    {{S0, S2, S5, S7, S5}, FNU_MOUSE_BUTTON1DOWN, ~0, 0},
    {{S0, S6, S1, S7, S6}, FNU_MOUSE_BUTTON3DOWN, ~0, 0},
    {{S0, S6, S5, S7, S7}, FNU_MOUSE_BUTTON1DOWN | FNU_MOUSE_BUTTON3DOWN, ~0, 0},
    {{S0, S2, S1, S3, S0}, FNU_MOUSE_BUTTON3DOWN, ~0, 1},
    {{S0, S9, S9, S3, S9}, 0, ~(FNU_MOUSE_BUTTON1DOWN | FNU_MOUSE_BUTTON3DOWN), 0},
};

struct e3bstate {
  int enabled;
  int button2timeout;
  enum bt3_emul_state mouse_button_state;
  int64_t mouse_button_state_ts;
  int mouse_move_delayed;
};

enum scroll_state { SCROLL_NOTSCROLLING, SCROLL_PREPARE, SCROLL_SCROLLING };

struct scroll {
  int enable_vert, enable_hor;
  int threshold, speed;
  enum scroll_state state;
  int movement, hmovement;
};

struct drift {
  int distance, time, after, terminate;
  int64_t current_ts, last_activity, since;
  int lastx, lasty, prevx, prevy;
};

struct accel {
  int is_exponential;
  double accelx, accely, accelz;
  double expoaccel, expoffset;
  double remainx, remainy, remainz;
  double lastlength[3];
};

struct rodent {
  int mfd;
  char port[32];
  struct btstate btstate;
  struct e3bstate e3b;
  struct drift drift;
  struct accel accel;
  struct scroll scroll;
  struct fnu_mousestatus action0, action, action2;
  int buttons;
};

static struct rodent rodents[MAXRODENT];
static int debug;
static volatile sig_atomic_t paused;
static int cfd = -1;
static int hfd = -1;
static const char *portname;
static volatile sig_atomic_t hup;
static sigset_t open_mask;

static int opt_clickthreshold = -1;
static int opt_e3b_enabled;
static int opt_e3b_button2timeout = -1;
static struct btstate opt_btstate;
static int opt_drift_terminate;
static int opt_drift_distance = 4, opt_drift_time = 500, opt_drift_after = 4000;
static double opt_accelx = 1.0, opt_accely = 1.0;
static int opt_exp_accel;
static double opt_expoaccel = 1.0, opt_expoffset = 1.0;
static int opt_virtual_scroll, opt_hvirtual_scroll;
static int opt_scroll_speed = -1, opt_scroll_threshold = -1;

static void logmsg(const char *fmt, ...) {
  char buf[256];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  dprintf(2, "moused: %s\n", buf);
}

#define debugmsg(...)                                                          \
  do {                                                                         \
    if (debug)                                                                 \
      logmsg(__VA_ARGS__);                                                     \
  } while (0)

static int64_t now_ms(void) {
  struct fnu_timespec t;

  fnu_gettime(&t, FNU_CLOCK_WALL);
  return t.sec * 1000 + t.nsec / 1000000;
}

static long lround_(double v) { return (long)(v < 0 ? v - 0.5 : v + 0.5); }

static void usage(void) {
  dprintf(2, "usage: moused [-3df] [-a X[,Y]] [-A exp[,offset]] [-C threshold]\n"
             "              [-E timeout] [-m N=M] [-w N] [-z target] [-VH]\n"
             "              [-U threshold] [-L distance] [-T distance[,time[,after]]]\n"
             "              [-p port] [-I pidfile]\n");
  exit(1);
}

static void linacc(struct accel *acc, int dx, int dy, int dz, int *movex,
                   int *movey, int *movez) {
  double fdx, fdy, fdz;

  if (dx == 0 && dy == 0 && dz == 0) {
    *movex = *movey = *movez = 0;
    return;
  }
  fdx = dx * acc->accelx + acc->remainx;
  fdy = dy * acc->accely + acc->remainy;
  fdz = dz * acc->accelz + acc->remainz;
  *movex = (int)lround_(fdx);
  *movey = (int)lround_(fdy);
  *movez = (int)lround_(fdz);
  acc->remainx = fdx - *movex;
  acc->remainy = fdy - *movey;
  acc->remainz = fdz - *movez;
}

static void expoacc(struct accel *acc, int dx, int dy, int dz, int *movex,
                    int *movey, int *movez) {
  double fdx, fdy, fdz, length, lbase, accel;

  if (dx == 0 && dy == 0 && dz == 0) {
    *movex = *movey = *movez = 0;
    return;
  }
  fdx = dx * acc->accelx;
  fdy = dy * acc->accely;
  fdz = dz * acc->accelz;
  length = sqrt((fdx * fdx) + (fdy * fdy));
  length = (length + acc->lastlength[0] + acc->lastlength[1] +
            acc->lastlength[2]) / 4;
  lbase = length / acc->expoffset;
  accel = pow(lbase, acc->expoaccel) / lbase;
  fdx = fdx * accel + acc->remainx;
  fdy = fdy * accel + acc->remainy;
  *movex = (int)lround_(fdx);
  *movey = (int)lround_(fdy);
  *movez = (int)lround_(fdz);
  acc->remainx = fdx - *movex;
  acc->remainy = fdy - *movey;
  acc->remainz = fdz - *movez;
  acc->lastlength[2] = acc->lastlength[1];
  acc->lastlength[1] = acc->lastlength[0];
  acc->lastlength[0] = length;
}

static void r_timestamp(struct rodent *r, struct fnu_mousestatus *act) {
  struct btstate *bt = &r->btstate;
  int64_t ts1 = now_ms(), ts = ts1 - bt->clickthreshold;
  int64_t ts2 = ts1 - r->e3b.button2timeout;
  int mask = act->flags & FNU_MOUSE_BUTTONS, button = FNU_MOUSE_BUTTON1DOWN, i;

  r->drift.current_ts = ts1;
  for (i = 0; i < FNU_MOUSE_MAXBUTTON && mask != 0; ++i) {
    if (mask & 1) {
      if (act->button & button) {
        if (ts > bt->bstate[i].ts)
          bt->bstate[i].count = 1;
        else
          ++bt->bstate[i].count;
      }
      bt->bstate[i].ts = ts1;
    } else if (act->button & button) {
      if (ts2 > bt->bstate[i].ts) {
        bt->bstate[i].count = 1;
        bt->bstate[i].ts = ts1;
        act->flags |= button;
        debugmsg("button %d timeout", i + 1);
      }
    }
    button <<= 1;
    mask >>= 1;
  }
}

static int r_timeout(struct e3bstate *e3b) {
  if (states[e3b->mouse_button_state].timeout)
    return 1;
  return now_ms() - e3b->button2timeout > e3b->mouse_button_state_ts;
}

static void r_move(struct rodent *r, struct fnu_mousestatus *act) {
  struct fnu_mouse_info mouse;

  memset(&mouse, 0, sizeof(mouse));
  if (r->accel.is_exponential)
    expoacc(&r->accel, act->dx, act->dy, act->dz, &mouse.u.data.x,
            &mouse.u.data.y, &mouse.u.data.z);
  else
    linacc(&r->accel, act->dx, act->dy, act->dz, &mouse.u.data.x,
           &mouse.u.data.y, &mouse.u.data.z);
  mouse.operation = FNU_MOUSE_MOTION_EVENT;
  mouse.u.data.buttons = act->button;
  if (debug < 2 && !paused)
    ioctl(cfd, FNU_CONSMOUSE, (uint64_t)(uintptr_t)&mouse);
}

static void r_click(struct rodent *r, struct fnu_mousestatus *act) {
  struct btstate *bt = &r->btstate;
  struct fnu_mouse_info mouse;
  int mask = act->flags & FNU_MOUSE_BUTTONS, button = FNU_MOUSE_BUTTON1DOWN, i;

  if (mask == 0)
    return;
  memset(&mouse, 0, sizeof(mouse));
  for (i = 0; i < FNU_MOUSE_MAXBUTTON && mask != 0; ++i) {
    if (mask & 1) {
      mouse.u.event.value = act->button & button ? bt->mstate[i]->count : 0;
      mouse.operation = FNU_MOUSE_BUTTON_EVENT;
      mouse.u.event.id = button;
      if (debug < 2 && !paused)
        ioctl(cfd, FNU_CONSMOUSE, (uint64_t)(uintptr_t)&mouse);
      debugmsg("button %d  count %d", i + 1, mouse.u.event.value);
    }
    button <<= 1;
    mask >>= 1;
  }
}

static int r_statetrans(struct rodent *r, struct fnu_mousestatus *a1,
                        struct fnu_mousestatus *a2, int trans) {
  struct e3bstate *e3b = &r->e3b;
  int changed = 0, flags;

  a2->dx = a1->dx;
  a2->dy = a1->dy;
  a2->dz = a1->dz;
  a2->obutton = a2->button;
  a2->button = a1->button;
  a2->flags = a1->flags;
  if (!e3b->enabled)
    return 0;

  if ((a2->dx != 0 || a2->dy != 0) &&
      S_DELAYED(states[e3b->mouse_button_state].s[trans])) {
    if (++e3b->mouse_move_delayed > BUTTON2_MAXMOVE) {
      e3b->mouse_move_delayed = 0;
      e3b->mouse_button_state = states[e3b->mouse_button_state].s[A_TIMEOUT];
      changed = 1;
    } else
      a2->dx = a2->dy = 0;
  } else
    e3b->mouse_move_delayed = 0;
  if (e3b->mouse_button_state != states[e3b->mouse_button_state].s[trans])
    changed = 1;
  if (changed)
    e3b->mouse_button_state_ts = now_ms();
  e3b->mouse_button_state = states[e3b->mouse_button_state].s[trans];
  a2->button &= ~(FNU_MOUSE_BUTTON1DOWN | FNU_MOUSE_BUTTON2DOWN | FNU_MOUSE_BUTTON3DOWN);
  a2->button &= states[e3b->mouse_button_state].mask;
  a2->button |= states[e3b->mouse_button_state].buttons;
  flags = a2->flags & FNU_MOUSE_POSCHANGED;
  flags |= a2->obutton ^ a2->button;
  if (flags & FNU_MOUSE_BUTTON2DOWN) {
    a2->flags = flags & FNU_MOUSE_BUTTON2DOWN;
    r_timestamp(r, a2);
  }
  a2->flags = flags;
  return changed;
}

static void r_vscroll_detect(struct rodent *r, struct fnu_mousestatus *act) {
  struct scroll *sc = &r->scroll;
  struct fnu_mousestatus newaction;

  if (act->button == FNU_MOUSE_BUTTON2DOWN) {
    if (sc->state == SCROLL_NOTSCROLLING) {
      sc->state = SCROLL_PREPARE;
      sc->movement = sc->hmovement = 0;
      debugmsg("PREPARING TO SCROLL");
    }
    return;
  }
  switch (sc->state) {
  case SCROLL_SCROLLING:
    sc->state = SCROLL_NOTSCROLLING;
    debugmsg("DONE WITH SCROLLING / %d", sc->state);
    break;
  case SCROLL_PREPARE:
    newaction = *act;
    r_timestamp(r, act);
    r_statetrans(r, act, &newaction,
                 A(newaction.button & FNU_MOUSE_BUTTON1DOWN,
                   act->button & FNU_MOUSE_BUTTON3DOWN));
    newaction.button = FNU_MOUSE_BUTTON2DOWN;
    r_click(r, &newaction);
    r_timestamp(r, &newaction);
    newaction.obutton = newaction.button;
    newaction.button = act->button;
    r_click(r, &newaction);
    break;
  default:
    break;
  }
}

static int iabs(int v) { return v < 0 ? -v : v; }

static void r_vscroll(struct scroll *sc, struct fnu_mousestatus *act) {
  switch (sc->state) {
  case SCROLL_PREPARE:
    if (act->dy == 0 && act->dx == 0)
      break;
    if (sc->enable_vert) {
      sc->movement += act->dy;
      if (iabs(sc->movement) > sc->threshold)
        sc->state = SCROLL_SCROLLING;
    }
    if (sc->enable_hor) {
      sc->hmovement += act->dx;
      if (iabs(sc->hmovement) > sc->threshold)
        sc->state = SCROLL_SCROLLING;
    }
    if (sc->state == SCROLL_SCROLLING)
      sc->movement = sc->hmovement = 0;
    break;
  case SCROLL_SCROLLING:
    if (sc->enable_vert) {
      sc->movement += act->dy;
      if (sc->movement < -sc->speed) {
        act->dz = -1;
        sc->movement = 0;
      } else if (sc->movement > sc->speed) {
        act->dz = 1;
        sc->movement = 0;
      }
    }
    if (sc->enable_hor) {
      sc->hmovement += act->dx;
      if (sc->hmovement < -sc->speed) {
        act->dz = -2;
        sc->hmovement = 0;
      } else if (sc->hmovement > sc->speed) {
        act->dz = 2;
        sc->hmovement = 0;
      }
    }
    act->dx = act->dy = 0;
    break;
  default:
    break;
  }
}

static int r_drift(struct drift *drift, struct fnu_mousestatus *act) {
  int64_t tmp = drift->current_ts - drift->last_activity;

  if (tmp <= drift->after)
    return 0;
  tmp = drift->current_ts - drift->since;
  if (tmp < drift->time) {
    drift->lastx += act->dx;
    drift->lasty += act->dy;
  } else {
    if (tmp > 2 * drift->time)
      drift->prevx = drift->prevy = 0;
    else {
      drift->prevx = drift->lastx;
      drift->prevy = drift->lasty;
    }
    drift->lastx = act->dx;
    drift->lasty = act->dy;
    drift->since = drift->current_ts;
  }
  if (iabs(drift->lastx) + iabs(drift->lasty) > drift->distance) {
    act->dx = drift->prevx + drift->lastx;
    act->dy = drift->prevy + drift->lasty;
    drift->since = 0;
    drift->lastx = drift->lasty = 0;
    drift->last_activity = drift->current_ts;
    return 0;
  }
  return 1;
}

static const char *skipspace(const char *s) {
  while (isspace(*s))
    ++s;
  return s;
}

static int r_installmap(const char *arg, struct btstate *bt) {
  long pbutton, lbutton;
  const char *s;

  while (*arg) {
    arg = skipspace(arg);
    s = arg;
    while (isdigit(*arg))
      ++arg;
    arg = skipspace(arg);
    if (arg <= s || *arg != '=')
      return 0;
    lbutton = strtol(s, 0, 10);
    arg = skipspace(++arg);
    s = arg;
    while (isdigit(*arg))
      ++arg;
    if (arg <= s || (!isspace(*arg) && *arg != 0))
      return 0;
    pbutton = strtol(s, 0, 10);
    if (lbutton <= 0 || lbutton > FNU_MOUSE_MAXBUTTON)
      return 0;
    if (pbutton <= 0 || pbutton > FNU_MOUSE_MAXBUTTON)
      return 0;
    bt->p2l[pbutton - 1] = 1 << (lbutton - 1);
    bt->mstate[lbutton - 1] = &bt->bstate[pbutton - 1];
  }
  return 1;
}

static int r_installzmap(const char *arg, char **argv, int argc, int *idx,
                         struct btstate *bt) {
  long i, j;

  if (strcmp(arg, "x") == 0) {
    bt->zmap[0] = MOUSE_XAXIS;
    return 1;
  }
  if (strcmp(arg, "y") == 0) {
    bt->zmap[0] = MOUSE_YAXIS;
    return 1;
  }
  i = strtol(arg, 0, 10);
  if (i <= 0 || i >= FNU_MOUSE_MAXBUTTON)
    return 0;
  bt->zmap[0] = (int)i;
  bt->zmap[1] = (int)i + 1;
  for (j = 1; j < ZMAP_MAXBUTTON; ++j) {
    if (*idx >= argc || !isdigit(*argv[*idx]))
      break;
    i = strtol(argv[*idx], 0, 10);
    if (i <= 0 || i >= FNU_MOUSE_MAXBUTTON)
      return 0;
    bt->zmap[j] = (int)i;
    ++*idx;
  }
  if (bt->zmap[2] != 0 && bt->zmap[3] == 0)
    bt->zmap[3] = bt->zmap[2] + 1;
  return 1;
}

static void r_map(struct fnu_mousestatus *act1, struct fnu_mousestatus *act2,
                  struct btstate *bt) {
  int pb, pbuttons = act1->button, lbuttons = 0;

  act2->obutton = act2->button;
  if (pbuttons & bt->wmode) {
    pbuttons &= ~bt->wmode;
    act1->dz = act1->dy;
    act1->dx = 0;
    act1->dy = 0;
  }
  act2->dx = act1->dx;
  act2->dy = act1->dy;
  act2->dz = act1->dz;

  switch (bt->zmap[0]) {
  case 0:
    break;
  case MOUSE_XAXIS:
    if (act1->dz != 0) {
      act2->dx = act1->dz;
      act2->dz = 0;
    }
    break;
  case MOUSE_YAXIS:
    if (act1->dz != 0) {
      act2->dy = act1->dz;
      act2->dz = 0;
    }
    break;
  default:
    pbuttons &= ~(bt->zmap[0] | bt->zmap[1] | bt->zmap[2] | bt->zmap[3]);
    if (act1->dz < -1 && bt->zmap[2]) {
      pbuttons |= bt->zmap[2];
      bt->zstate[2].count = 1;
    } else if (act1->dz < 0) {
      pbuttons |= bt->zmap[0];
      bt->zstate[0].count = 1;
    } else if (act1->dz > 1 && bt->zmap[3]) {
      pbuttons |= bt->zmap[3];
      bt->zstate[3].count = 1;
    } else if (act1->dz > 0) {
      pbuttons |= bt->zmap[1];
      bt->zstate[1].count = 1;
    }
    act2->dz = 0;
    break;
  }

  for (pb = 0; pb < FNU_MOUSE_MAXBUTTON && pbuttons != 0; ++pb) {
    lbuttons |= (pbuttons & 1) ? bt->p2l[pb] : 0;
    pbuttons >>= 1;
  }
  act2->button = lbuttons;
  act2->flags = ((act2->dx || act2->dy || act2->dz) ? FNU_MOUSE_POSCHANGED : 0) |
                (act2->obutton ^ act2->button);
}

static int r_protocol_wsmouse(struct rodent *r, const struct fnu_wsevent *ev, int n,
                              int *used, struct fnu_mousestatus *act) {
  int i, changed;

  act->dx = act->dy = act->dz = 0;
  act->obutton = act->button;
  for (i = 0; i < n; i++) {
    switch (ev[i].type) {
    case FNU_WSEVENT_MOUSE_DELTA_X: act->dx += ev[i].value; continue;
    case FNU_WSEVENT_MOUSE_DELTA_Y: act->dy -= ev[i].value; continue;
    case FNU_WSEVENT_MOUSE_DELTA_Z: act->dz -= ev[i].value; continue;
    case FNU_WSEVENT_MOUSE_DOWN:
      if (ev[i].value >= 0 && ev[i].value < 31)
        r->buttons |= 1 << ev[i].value;
      break;
    case FNU_WSEVENT_MOUSE_UP:
      if (ev[i].value >= 0 && ev[i].value < 31)
        r->buttons &= ~(1 << ev[i].value);
      break;
    default: continue;
    }
    i++;
    break;
  }
  *used = i;
  act->button = (r->buttons & ~7) | (r->buttons & 1 ? FNU_MOUSE_BUTTON1DOWN : 0) |
                (r->buttons & 4 ? FNU_MOUSE_BUTTON2DOWN : 0) |
                (r->buttons & 2 ? FNU_MOUSE_BUTTON3DOWN : 0);
  changed = act->obutton ^ act->button;
  act->flags = ((act->dx || act->dy || act->dz) ? FNU_MOUSE_POSCHANGED : 0) | changed;
  return act->flags;
}

static struct rodent *r_init(const char *path) {
  struct rodent *r = 0;
  int i, fd;

  for (i = 0; i < MAXRODENT; i++)
    if (rodents[i].mfd > 0 && strcmp(rodents[i].port, path) == 0)
      return 0;
  for (i = 0; i < MAXRODENT; i++)
    if (rodents[i].mfd <= 0) {
      r = &rodents[i];
      break;
    }
  if (r == 0 || (fd = open(path, O_RDONLY)) < 0)
    return 0;
  memset(r, 0, sizeof(*r));
  r->mfd = fd;
  strlcpy(r->port, (char *)path, sizeof(r->port));

  r->btstate = opt_btstate;
  r->btstate.clickthreshold =
      opt_clickthreshold < 0 ? DFLT_CLICKTHRESHOLD : opt_clickthreshold;
  for (i = 0; i < FNU_MOUSE_MAXBUTTON; i++) {
    if (r->btstate.p2l[i] == 0)
      r->btstate.p2l[i] = 1 << i;
    if (r->btstate.mstate[i] == 0)
      r->btstate.mstate[i] = &r->btstate.bstate[i];
    else
      r->btstate.mstate[i] =
          &r->btstate.bstate[r->btstate.mstate[i] - opt_btstate.bstate];
  }

  r->e3b.enabled = opt_e3b_enabled;
  r->e3b.button2timeout =
      opt_e3b_button2timeout < 0 ? DFLT_BUTTON2TIMEOUT : opt_e3b_button2timeout;
  r->e3b.mouse_button_state = S0;

  r->drift.terminate = opt_drift_terminate;
  r->drift.distance = opt_drift_distance;
  r->drift.time = opt_drift_time;
  r->drift.after = opt_drift_after;

  r->accel.is_exponential = opt_exp_accel;
  r->accel.accelx = opt_accelx;
  r->accel.accely = opt_accely;
  r->accel.accelz = 1.0;
  r->accel.expoaccel = opt_expoaccel;
  r->accel.expoffset = opt_expoffset;

  r->scroll.enable_vert = opt_virtual_scroll;
  r->scroll.enable_hor = opt_hvirtual_scroll;
  r->scroll.threshold =
      opt_scroll_threshold < 0 ? DFLT_SCROLLTHRESHOLD : opt_scroll_threshold;
  r->scroll.speed = opt_scroll_speed < 0 ? DFLT_SCROLLSPEED : opt_scroll_speed;
  r->scroll.state = SCROLL_NOTSCROLLING;

  debugmsg("%s attached", path);
  return r;
}

static void r_deinit(struct rodent *r) {
  if (r->mfd <= 0)
    return;
  close(r->mfd);
  r->mfd = 0;
  debugmsg("destroy device: port: %s", r->port);
}

static void r_init_all(void) {
  char path[32];
  struct dirent **names;
  int i, n;

  if ((n = scandir("/dev", &names, nodots, 0)) < 0) {
    logmsg("Failed to open /dev");
    exit(1);
  }
  for (i = 0; i < n; i++)
    if (strncmp(names[i]->d_name, "wsmouse", 7) == 0 && isdigit(names[i]->d_name[7])) {
      snprintf(path, sizeof(path), "/dev/%s", names[i]->d_name);
      r_init(path);
    }
  freelist(names, n);
}

static void r_deinit_all(void) {
  int i;

  for (i = 0; i < MAXRODENT; i++)
    r_deinit(&rodents[i]);
}

static int r_handle(struct rodent *r, const struct fnu_wsevent *ev, int n) {
  struct fnu_mousestatus *action0 = &r->action0, *action = &r->action;
  struct fnu_mousestatus *action2 = &r->action2;
  int flags, used = 0;

  if (n > 0) {
    flags = r_protocol_wsmouse(r, ev, n, &used, action0);
    if (flags == 0)
      return used;
    if (r->scroll.enable_vert || r->scroll.enable_hor)
      r_vscroll_detect(r, action0);
    r_timestamp(r, action0);
    r_statetrans(r, action0, action,
                 A(action0->button & FNU_MOUSE_BUTTON1DOWN,
                   action0->button & FNU_MOUSE_BUTTON3DOWN));
    debugmsg("flags:%08x buttons:%08x obuttons:%08x", action->flags,
             action->button, action->obutton);
  } else {
    // the 3 button emulation timer
    action0->button = action0->obutton;
    action0->dx = action0->dy = action0->dz = 0;
    action0->flags = flags = 0;
    if (!(r_timeout(&r->e3b) && r_statetrans(r, action0, action, A_TIMEOUT))) {
      action0->obutton = action0->button;
      return used;
    }
  }
  action0->obutton = action0->button;
  flags &= FNU_MOUSE_POSCHANGED;
  flags |= action->obutton ^ action->button;
  action->flags = flags;
  if (flags == 0)
    return used;

  r_map(action, action2, &r->btstate);
  debugmsg("activity : buttons 0x%08x  dx %d  dy %d  dz %d", action2->button,
           action2->dx, action2->dy, action2->dz);

  if (r->scroll.enable_vert || r->scroll.enable_hor)
    r_vscroll(&r->scroll, action2);

  if (r->drift.terminate) {
    if ((flags & FNU_MOUSE_POSCHANGED) == 0 || action->dz || action2->dz)
      r->drift.last_activity = r->drift.current_ts;
    else if (r_drift(&r->drift, action2))
      return used;
  }

  if (r->scroll.state == SCROLL_NOTSCROLLING)
    r_click(r, action2);
  if (action2->flags & FNU_MOUSE_POSCHANGED)
    r_move(r, action2);

  if (r->btstate.zmap[0] > 0 && action->dz != 0) {
    action->obutton = action->button;
    action->dx = action->dy = action->dz = 0;
    r_map(action, action2, &r->btstate);
    r_click(r, action2);
  }
  return used;
}

static int connect_hotplug(void) { return open("/dev/hotplug", O_RDONLY); }

static void fetch_and_parse_hotplug(void) {
  struct fnu_hotplug_event e;
  char path[32];

  if (read(hfd, &e, sizeof(e)) != (ssize_t)sizeof(e)) {
    close(hfd);
    hfd = -1;
    return;
  }
  if (e.type != FNU_HOTPLUG_ATTACH || e.devclass != FNU_DEVCLASS_MOUSE)
    return;
  snprintf(path, sizeof(path), "/dev/%s", e.name);
  (void)r_init(path);
}

static void moused(void) {
  struct fnu_wsevent ev[16];
  struct timespec tick = {0, 20 * 1000 * 1000}, second = {1, 0};
  struct timespec *timeout;
  fd_set rd;
  int i, n, c, armed;

  for (;;) {
    if (hfd == -1 && portname == 0)
      hfd = connect_hotplug();
    FD_ZERO(&rd);
    armed = 0;
    for (i = 0; i < MAXRODENT; i++)
      if (rodents[i].mfd > 0) {
        FD_SET(rodents[i].mfd, &rd);
        if (rodents[i].e3b.enabled &&
            S_DELAYED(rodents[i].e3b.mouse_button_state))
          armed = 1;
      }
    if (hfd >= 0)
      FD_SET(hfd, &rd);
    timeout = armed ? &tick : (hfd == -1 && portname == 0) ? &second : 0;
    n = pselect(FD_SETSIZE, &rd, 0, 0, timeout, &open_mask);
    if (hup) {
      hup = 0;
      r_deinit_all();
      if (portname != 0)
        r_init(portname);
      else
        r_init_all();
      continue;
    }
    if (paused == 2)
      return;
    if (n < 0) {
      if (errno != EINTR)
        logmsg("select: %s", strerror(errno));
      continue;
    }
    if (n == 0) {
      if (armed)
        for (i = 0; i < MAXRODENT; i++)
          if (rodents[i].mfd > 0 && rodents[i].e3b.enabled)
            r_handle(&rodents[i], 0, 0);
      continue;
    }
    if (hfd >= 0 && FD_ISSET(hfd, &rd))
      fetch_and_parse_hotplug();
    for (i = 0; i < MAXRODENT; i++) {
      struct rodent *r = &rodents[i];

      if (r->mfd <= 0 || !FD_ISSET(r->mfd, &rd))
        continue;
      n = read(r->mfd, ev, sizeof(ev));
      if (n <= 0) {
        if (portname == 0) {
          r_deinit(r);
          continue;
        }
        return;
      }
      n /= (int)sizeof(ev[0]);
      for (c = 0; c < n; c += r_handle(r, ev + c, n - c))
        ;
    }
  }
}

static void on_pause(int sig __unused) { paused = !paused; }
static void on_hup(int sig __unused) { hup = 1; }
static void on_term(int sig __unused) { paused = 2; }

static int parse_two(const char *s, double *a, double *b) {
  char *end;

  *a = strtod(s, &end);
  if (end == s)
    return 0;
  if (*end == ',') {
    s = end + 1;
    *b = strtod(s, &end);
    if (end == s)
      return 0;
    return 2;
  }
  return *end == 0 ? 1 : 0;
}

int main(int argc, char **argv) {
  struct sigaction act;
  sigset_t blocked;
  int i = 1, foreground = 0, n;
  const char *pidfile = 0;
  struct pidfh *pfh;
  pid_t other;
  double a, b;

  for (i = 0; i < FNU_MOUSE_MAXBUTTON; i++)
    opt_btstate.p2l[i] = 1 << i;

  for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
    const char *p = argv[i] + 1;

    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (; *p != 0; p++) {
      const char *v;

      switch (*p) {
      case '3': opt_e3b_enabled = 1; continue;
      case 'd': debug++; continue;
      case 'f': foreground = 1; continue;
      case 'V': opt_virtual_scroll = 1; continue;
      case 'H': opt_hvirtual_scroll = 1; continue;
      case 'a': case 'A': case 'C': case 'E': case 'm': case 'w': case 'z':
      case 'U': case 'L': case 'T': case 'p': case 'I':
        break;
      default:
        usage();
      }
      v = p[1] != 0 ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
      if (v == 0)
        usage();
      switch (*p) {
      case 'a':
        n = parse_two(v, &a, &b);
        if (n == 0)
          usage();
        opt_accelx = a;
        opt_accely = n == 2 ? b : a;
        break;
      case 'A':
        n = parse_two(v, &a, &b);
        if (n == 0 || a <= 0.0 || (n == 2 && b <= 0.0))
          usage();
        opt_exp_accel = 1;
        opt_expoaccel = a;
        if (n == 2)
          opt_expoffset = b;
        break;
      case 'C':
        opt_clickthreshold = atoi(v);
        if (opt_clickthreshold < 0 || opt_clickthreshold > MAX_CLICKTHRESHOLD)
          usage();
        break;
      case 'E':
        opt_e3b_button2timeout = atoi(v);
        if (opt_e3b_button2timeout < 0 ||
            opt_e3b_button2timeout > MAX_BUTTON2TIMEOUT)
          usage();
        break;
      case 'm':
        if (!r_installmap(v, &opt_btstate))
          usage();
        break;
      case 'w':
        n = atoi(v);
        if (n <= 0 || n > FNU_MOUSE_MAXBUTTON)
          usage();
        opt_btstate.wmode = 1 << (n - 1);
        break;
      case 'z': {
        int idx = i + 1;

        if (!r_installzmap(v, argv, argc, &idx, &opt_btstate))
          usage();
        i = idx - 1;
        break;
      }
      case 'U':
        opt_scroll_threshold = atoi(v);
        if (opt_scroll_threshold < 0)
          usage();
        break;
      case 'L':
        opt_scroll_speed = atoi(v);
        if (opt_scroll_speed < 0)
          usage();
        break;
      case 'T': {
        char *end;

        opt_drift_terminate = 1;
        opt_drift_distance = (int)strtol(v, &end, 10);
        if (*end == ',') {
          opt_drift_time = (int)strtol(end + 1, &end, 10);
          if (*end == ',')
            opt_drift_after = (int)strtol(end + 1, &end, 10);
        }
        if (*end != 0 || opt_drift_distance <= 0 || opt_drift_time <= 0 ||
            opt_drift_after <= 0)
          usage();
        break;
      }
      case 'p':
        portname = v;
        break;
      case 'I':
        pidfile = v;
        break;
      }
      p += strlen(p) - 1;
    }
  }
  if (i < argc)
    usage();

  if ((pfh = pidfile_open(pidfile, 0600, &other)) == 0) {
    if (errno == EEXIST) {
      logmsg("already running, pid %d", (int)other);
      exit(1);
    }
    logmsg("cannot open pid file: %s", strerror(errno));
  }
  if ((cfd = open("/dev/consolectl", O_RDWR)) < 0) {
    logmsg("/dev/consolectl: %s", strerror(errno));
    exit(1);
  }

  memset(&act, 0, sizeof(act));
  act.sa_handler = on_hup;
  sigaction(SIGHUP, &act, 0);
  act.sa_handler = on_term;
  sigaction(SIGINT, &act, 0);
  sigaction(SIGTERM, &act, 0);
  act.sa_handler = on_pause;
  sigaction(SIGUSR1, &act, 0);
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGHUP);
  sigaddset(&blocked, SIGINT);
  sigaddset(&blocked, SIGTERM);
  sigaddset(&blocked, SIGUSR1);
  sigprocmask(SIG_BLOCK, &blocked, &open_mask);

  if (portname == 0)
    r_init_all();
  else if (r_init(portname) == 0) {
    logmsg("Can not initialize device");
    exit(1);
  }

  if (!foreground && debug == 0) {
    if (fork() > 0)
      exit(0);
    setsid();
  }
  pidfile_write(pfh);
  if (portname == 0 && (hfd = connect_hotplug()) == -1)
    logmsg("/dev/hotplug: %s", strerror(errno));

  moused();
  r_deinit_all();
  if (hfd != -1)
    close(hfd);
  close(cfd);
  pidfile_remove(pfh);
  exit(0);
}
