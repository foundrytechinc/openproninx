// KEYBOARD LIGHT
// survives sleep. mostly
#include "defs.h"
#include "inc/abi.h"
#include "kbdlight.h"

static const struct kbdlight *light;
static int level, level_max, saved = -1;

static int now(void);

static int written(void *arg, int old) {
  (void)arg;
  if (light == 0) {
    level = old;
    return FNU_ERR_NODEVICE;
  }
  light->set(light->arg, level);
  return 0;
}

static void read_now(void *arg) {
  (void)arg;
  if (light != 0)
    now();
}

void kbdlight_attach(const struct kbdlight *l) {
  if (light != 0 || l->max <= 0)
    return;
  light = l;
  level_max = l->max;
  level = l->get(l->arg);
  if (level < 0)
    level = 0;
  sysctl_add_int("hw.kbd.backlight", &level, FNU_CTLF_WRITE, 0, level_max);
  sysctl_on_write("hw.kbd.backlight", written, 0);
  sysctl_on_read("hw.kbd.backlight", read_now, 0);
  sysctl_add_int("hw.kbd.backlight_max", &level_max, 0, 0, level_max);
  printf("kbd: keyboard light from %s, levels 0..%d, now %d\n", l->name,
         level_max, level);
}

// the machine changed it itself: its own key
void kbdlight_changed(int now) {
  if (light != 0 && now >= 0 && now <= level_max)
    level = now;
}

// the firmware lights the keys too: read each time
static int now(void) {
  int v = light->get(light->arg);

  if (v >= 0 && v <= level_max)
    level = v;
  return level;
}

void kbdlight_suspend(void) { saved = light != 0 ? now() : -1; }

// only if the firmware did not bring it back. it never does
void kbdlight_resume(void) {
  if (light == 0 || saved < 0)
    return;
  if (now() != saved) {
    level = saved;
    light->set(light->arg, level);
  }
  saved = -1;
}
