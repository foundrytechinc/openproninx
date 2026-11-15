#ifndef PRONINX_KBDREG_H
#define PRONINX_KBDREG_H

#include "inc/abi.h"
#include "mutex.h"

struct keyboard;
typedef struct keyboard keyboard_t;

struct keyboard_switch {
  int (*ioctl)(keyboard_t *kbd, uint64_t cmd, void *arg);
  void (*clear_state)(keyboard_t *kbd);
};

struct keyboard {
  int kb_index;
  int kb_flags;
#define KB_VALID (1 << 16)
#define KB_NO_DEVICE (1 << 17)
#define KB_PROBED (1 << 18)
#define KB_INITIALIZED (1 << 19)
#define KB_REGISTERED (1 << 20)
  int kb_config;
#define KB_CONF_PROBE_ONLY (1 << 16)
  const char *kb_name;
  int kb_unit;
  int kb_type;
  int kb_led;
  void *kb_data;
  struct fnu_keymap *kb_keymap;
  struct fnu_accentmap *kb_accentmap;
  struct fnu_fkeytab *kb_fkeytab;
  int kb_fkeytab_size;
  int kb_delay1;
  int kb_delay2;
  uchar kb_lastact[FNU_KB_NKEYS / 2];
  const struct keyboard_switch *kb_sw;
  keyboard_t *kb_next;
};

#define KBD_DELAY1 500
#define KBD_DELAY2 100

#define KBD_VALID(k) ((k)->kb_flags |= KB_VALID)
#define KBD_INVALID(k) ((k)->kb_flags &= ~KB_VALID)
#define KBD_HAS_DEVICE(k) (!((k)->kb_flags & KB_NO_DEVICE))
#define KBD_FOUND_DEVICE(k) ((k)->kb_flags &= ~KB_NO_DEVICE)
#define KBD_PROBE_DONE(k) ((k)->kb_flags |= KB_PROBED)
#define KBD_INIT_DONE(k) ((k)->kb_flags |= KB_INITIALIZED)
#define KBD_CONFIG_DONE(k) ((k)->kb_flags |= KB_REGISTERED)
#define KBD_LED_VAL(k) ((k)->kb_led)

#define kbdd_ioctl(k, c, a) ((k)->kb_sw->ioctl((k), (c), (a)))
#define kbdd_clear_state(k) ((k)->kb_sw->clear_state((k)))

extern struct mutex kbd_lock;

// shift and lock state
#define SHIFTS1 (1 << 16)
#define SHIFTS2 (1 << 17)
#define SHIFTS (SHIFTS1 | SHIFTS2)
#define CTLS1 (1 << 18)
#define CTLS2 (1 << 19)
#define CTLS (CTLS1 | CTLS2)
#define ALTS1 (1 << 20)
#define ALTS2 (1 << 21)
#define ALTS (ALTS1 | ALTS2)
#define AGRS1 (1 << 22)
#define AGRS2 (1 << 23)
#define AGRS (AGRS1 | AGRS2)
#define METAS1 (1 << 24)
#define METAS2 (1 << 25)
#define METAS (METAS1 | METAS2)
#define NLKDOWN (1 << 26)
#define SLKDOWN (1 << 27)
#define CLKDOWN (1 << 28)
#define ALKDOWN (1 << 29)
#define SHIFTAON (1 << 30)

struct kbdxlate {
  int mode;
  int flags;
#define COMPOSE (1 << 0)
  int state;
  int accents;
  uint composed_char;
  uchar prefix;
};

void kbd_init_struct(keyboard_t *kbd, const char *name, int type, int unit,
                     int config);
void kbd_set_maps(keyboard_t *kbd, struct fnu_keymap *keymap,
                  struct fnu_accentmap *accmap, struct fnu_fkeytab *fkeymap,
                  int fkeymap_size);
void kbd_default_maps(struct fnu_keymap *keymap, struct fnu_accentmap *accmap,
                      struct fnu_fkeytab *fkeymap);
int kbd_register(keyboard_t *kbd);
void kbd_unregister(keyboard_t *kbd);
int genkbd_keyaction(keyboard_t *kbd, int keycode, int up, int *shiftstate,
                     int *accents);
uint32 genkbd_pending(struct kbdxlate *x);
uint32 genkbd_scancode(keyboard_t *kbd, struct kbdxlate *x, int scancode);
void genkbd_clear_state(struct kbdxlate *x);
int genkbd_modeioctl(keyboard_t *kbd, struct kbdxlate *x, uint64_t cmd, void *arg);
void console_key(keyboard_t *kbd, int c);
int genkbd_commonioctl(keyboard_t *kbd, uint64_t cmd, void *arg);
int kbd_ioctl_user(uint64_t cmd, uint64_t arg);
int kbd_lockstate(void);
void kbd_set_lockstate(int state);
int kbd_scancode_keycode(keyboard_t *kbd, uchar *prefix, int scancode);
const char *kbd_type_name(int type);

#endif
