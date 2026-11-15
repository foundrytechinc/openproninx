// ATKBD
#include "atkbdcreg.h"
#include "defs.h"
#include "kbdreg.h"
#include "proc.h"
#include "task.h"
#include "x86.h"

typedef struct atkbd_state {
  KBDC kbdc;
  struct kbdxlate x;
  struct task inittask;
} atkbd_state_t;

#define KB_CONF_FAIL_IF_NO_KBD (1 << 0)
#define KB_CONF_NO_RESET (1 << 1)
#define KB_CONF_ALT_SCANCODESET (1 << 2)
#define KB_CONF_NO_PROBE_TEST (1 << 3)

#define DEFAULT_DELAY 0x1
#define DEFAULT_RATE 0x10

static const int kbdelays[] = {250, 500, 750, 1000};
static const int kbrates[] = {34,  38,  42,  46,  50,  55,  59,  63,  68,  76,  84,
                              92,  100, 110, 118, 126, 136, 152, 168, 184, 200, 220,
                              236, 252, 272, 304, 336, 368, 400, 440, 472, 504};

static int verbose;

static keyboard_t default_kbd;
static atkbd_state_t default_kbd_state;
static struct fnu_keymap default_keymap;
static struct fnu_accentmap default_accentmap;
static struct fnu_fkeytab default_fkeytab[FNU_KB_NFKEYS];

static int atkbd_ioctl(keyboard_t *kbd, uint64_t cmd, void *arg);
static void atkbd_clear_state(keyboard_t *kbd);
static int atkbd_reset(KBDC kbdc, int flags, int c);
static int init_keyboard(KBDC kbdc, int *type, int flags);
static void atkbd_init_task(void *arg);
static int probe_keyboard(KBDC kbdc, int flags);
static int write_kbd(KBDC kbdc, int command, int data);
static int get_kbd_id(KBDC kbdc);
static int typematic(int delay, int rate);
static int typematic_delay(int i);
static int typematic_rate(int i);
static int set_typematic(keyboard_t *kbd);

// THE KEYS

static uint32 atkbd_read_char(keyboard_t *kbd) {
  atkbd_state_t *state = (atkbd_state_t *)kbd->kb_data;
  uint32 action;
  int scancode;

  for (;;) {
    if ((action = genkbd_pending(&state->x)) != FNU_NOKEY)
      return action;
    if ((scancode = read_kbd_data_no_wait(state->kbdc)) == -1)
      return FNU_NOKEY;
    if ((action = genkbd_scancode(kbd, &state->x, scancode)) != FNU_NOKEY)
      return action;
  }
}

// at panic, unlocked
int atkbd_cncheckc(void) {
  keyboard_t *kbd = &default_kbd;
  uint32 c;

  if (kbd->kb_sw == 0 || !KBD_HAS_DEVICE(kbd))
    return -1;
  while ((c = atkbd_read_char(kbd)) != FNU_NOKEY)
    if (!(c & (FNU_RELKEY | FNU_ERRKEY)))
      return (int)c;
  return -1;
}

static int atkbd_ioctl(keyboard_t *kbd, uint64_t cmd, void *arg) {
  static const uchar ledmap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
  atkbd_state_t *state = kbd->kb_data;
  int error, i;

  switch (cmd) {
  case FNU_KBSETLEDS:
    if (*(int *)arg & ~FNU_KB_LOCKS)
      return FNU_ERR_INVALID;
    i = *(int *)arg;
    if (state->x.mode == FNU_KB_XLATE && kbd->kb_keymap->n_keys > FNU_KB_ALTGR) {
      if (i & FNU_KB_ALKED)
        i |= FNU_KB_CLKED;
      else
        i &= ~FNU_KB_CLKED;
    }
    if (KBD_HAS_DEVICE(kbd)) {
      error = write_kbd(state->kbdc, KBDC_SET_LEDS, ledmap[i & FNU_KB_LEDS]);
      if (error)
        return error;
    }
    KBD_LED_VAL(kbd) = *(int *)arg;
    return 0;

  case FNU_KBSETREPEAT:
    if (!KBD_HAS_DEVICE(kbd))
      return 0;
    i = typematic(((int *)arg)[0], ((int *)arg)[1]);
    error = write_kbd(state->kbdc, KBDC_SET_TYPEMATIC, i);
    if (error == 0) {
      kbd->kb_delay1 = typematic_delay(i);
      kbd->kb_delay2 = typematic_rate(i);
    }
    return error;

  case FNU_KBSETRATE:
    if (!KBD_HAS_DEVICE(kbd))
      return 0;
    error = write_kbd(state->kbdc, KBDC_SET_TYPEMATIC, *(int *)arg);
    if (error == 0) {
      kbd->kb_delay1 = typematic_delay(*(int *)arg);
      kbd->kb_delay2 = typematic_rate(*(int *)arg);
    }
    return error;

  default:
    return genkbd_modeioctl(kbd, &state->x, cmd, arg);
  }
}

static void atkbd_clear_state(keyboard_t *kbd) {
  atkbd_state_t *state = (atkbd_state_t *)kbd->kb_data;

  genkbd_clear_state(&state->x);
}

static const struct keyboard_switch atkbdsw = {
    .ioctl = atkbd_ioctl,
    .clear_state = atkbd_clear_state,
};

// THE DEVICE

static int atkbd_reset(KBDC kbdc, int flags, int c) {
  if (!(flags & KB_CONF_NO_RESET) && !reset_kbd(kbdc)) {
    empty_both_buffers(kbdc, 10);
    test_controller(kbdc);
    test_kbd_port(kbdc);
    set_controller_command_byte(kbdc, 0xff, c);
    if (verbose)
      printf("atkbd: failed to reset the keyboard.\n");
    return -1;
  }
  return 0;
}

static int set_typematic(keyboard_t *kbd) {
  int val, error;
  atkbd_state_t *state = kbd->kb_data;

  val = typematic(DEFAULT_DELAY, DEFAULT_RATE);
  error = write_kbd(state->kbdc, KBDC_SET_TYPEMATIC, val);
  if (error == 0) {
    kbd->kb_delay1 = typematic_delay(val);
    kbd->kb_delay2 = typematic_rate(val);
  }
  return error;
}

static void atkbd_setup(keyboard_t *kbd) {
  atkbd_state_t *state = kbd->kb_data;
  int delay[2];

  atkbd_ioctl(kbd, FNU_KBSETLEDS, &state->x.state);
  set_typematic(kbd);
  delay[0] = kbd->kb_delay1;
  delay[1] = kbd->kb_delay2;
  atkbd_ioctl(kbd, FNU_KBSETREPEAT, delay);
}

static int setup_kbd_port(KBDC kbdc, int port, int intr) {
  if (!set_controller_command_byte(kbdc, KBD_KBD_CONTROL_BITS,
                                   ((port) ? KBD_ENABLE_KBD_PORT : KBD_DISABLE_KBD_PORT) |
                                       ((intr) ? KBD_ENABLE_KBD_INT : KBD_DISABLE_KBD_INT)))
    return 1;
  return 0;
}

static int get_kbd_echo(KBDC kbdc) {
  int data;

  if (setup_kbd_port(kbdc, 1, 0))
    return -1;

  write_kbd_command(kbdc, KBDC_ECHO);
  data = read_kbd_data(kbdc);
  if (data == KBD_ACK)
    data = read_kbd_data(kbdc);
  if (data != KBD_ECHO) {
    empty_both_buffers(kbdc, 10);
    test_controller(kbdc);
    test_kbd_port(kbdc);
    return -1;
  }

  if (setup_kbd_port(kbdc, 1, 1))
    return -1;
  return 0;
}

static int probe_keyboard(KBDC kbdc, int flags) {
  int err, c, m;

  if (!kbdc_lock(kbdc, 1))
    return -1;

  // no reset, no tail. a byte takes a ms on the wire. slow as hell
  write_controller_command(kbdc, KBDC_DISABLE_KBD_PORT);
  empty_both_buffers(kbdc, (flags & KB_CONF_NO_RESET) ? 10 : 100);

  m = kbdc_get_device_mask(kbdc) & ~KBD_KBD_CONTROL_BITS;
  c = get_controller_command_byte(kbdc);
  if (c == -1) {
    kbdc_set_device_mask(kbdc, m);
    kbdc_lock(kbdc, 0);
    return -1;
  }

  test_controller(kbdc);
  if (!(flags & KB_CONF_NO_PROBE_TEST))
    test_kbd_port(kbdc);

  err = get_kbd_echo(kbdc);

  kbdc_set_device_mask(kbdc, m | KBD_KBD_CONTROL_BITS);
  setup_kbd_port(kbdc, 1, 1);
  kbdc_lock(kbdc, 0);
  return err;
}

static int init_keyboard(KBDC kbdc, int *type, int flags) {
  int codeset, id, c;

  if (!kbdc_lock(kbdc, 1))
    return -1;

  write_controller_command(kbdc, KBDC_DISABLE_KBD_PORT);
  empty_both_buffers(kbdc, (flags & KB_CONF_NO_RESET) ? 10 : 200);
  c = get_controller_command_byte(kbdc);
  if (c == -1) {
    kbdc_lock(kbdc, 0);
    printf("atkbd: unable to get the current command byte value.\n");
    return -1;
  }
  if (verbose)
    printf("atkbd: the current kbd controller command byte %x\n", c);

  if (setup_kbd_port(kbdc, 1, 0)) {
    printf("atkbd: unable to set the command byte.\n");
    kbdc_lock(kbdc, 0);
    return -1;
  }

  codeset = -1;
  if (flags & KB_CONF_ALT_SCANCODESET)
    codeset = 1;

  *type = FNU_KB_OTHER;
  id = get_kbd_id(kbdc);
  switch (id) {
  case 0x41ab:
  case 0x83ab:
  case 0x54ab:
  case 0x84ab:
    *type = FNU_KB_101;
    break;
  case -1:
    *type = FNU_KB_84;
    break;
  default:
    break;
  }
  if (verbose)
    printf("atkbd: keyboard ID %x (%d)\n", id, *type);

  if (atkbd_reset(kbdc, flags, c)) {
    kbdc_lock(kbdc, 0);
    return -1;
  }

  if (codeset == 1) {
    if (send_kbd_command_and_data(kbdc, KBDC_SET_SCANCODE_SET, codeset) == KBD_ACK) {
      c &= ~KBD_TRANSLATION;
    } else {
      set_controller_command_byte(kbdc, 0xff, c);
      kbdc_lock(kbdc, 0);
      printf("atkbd: unable to set the XT keyboard mode.\n");
      return -1;
    }
  }

  if (!set_controller_command_byte(kbdc,
                                   KBD_KBD_CONTROL_BITS | KBD_TRANSLATION | KBD_OVERRIDE_KBD_LOCK,
                                   (c & (KBD_TRANSLATION | KBD_OVERRIDE_KBD_LOCK)) |
                                       KBD_ENABLE_KBD_PORT | KBD_ENABLE_KBD_INT)) {
    set_controller_command_byte(kbdc, 0xff, c);
    kbdc_lock(kbdc, 0);
    printf("atkbd: unable to enable the keyboard port and intr.\n");
    return -1;
  }

  kbdc_lock(kbdc, 0);
  return 0;
}

static int write_kbd(KBDC kbdc, int command, int data) {
  if (!kbdc_lock(kbdc, 1))
    return FNU_ERR_BUSY;
  spinlock_enter();
  if (send_kbd_command_and_data(kbdc, command, data) != KBD_ACK)
    send_kbd_command(kbdc, KBDC_ENABLE_KBD);
  spinlock_exit();
  kbdc_lock(kbdc, 0);
  return 0;
}

static int get_kbd_id(KBDC kbdc) {
  int id1, id2;

  empty_both_buffers(kbdc, 10);
  id1 = id2 = -1;
  if (send_kbd_command(kbdc, KBDC_SEND_DEV_ID) != KBD_ACK)
    return -1;
  delay(10000);
  id1 = read_kbd_data(kbdc);
  if (id1 != -1)
    id2 = read_kbd_data(kbdc);
  if ((id1 == -1) || (id2 == -1)) {
    empty_both_buffers(kbdc, 10);
    test_controller(kbdc);
    test_kbd_port(kbdc);
    return -1;
  }
  return (id2 << 8) | id1;
}

static int typematic_delay(int i) { return kbdelays[(i >> 5) & 3]; }

static int typematic_rate(int i) { return kbrates[i & 0x1f]; }

static int typematic(int delay, int rate) {
  int value, i;

  for (i = 4 - 1; i > 0; --i) {
    if (delay >= kbdelays[i])
      break;
  }
  value = i << 5;
  for (i = 32 - 1; i > 0; --i) {
    if (rate >= kbrates[i])
      break;
  }
  value |= i;
  return value;
}

// THE ATTACH

void atkbdinit(void) {
  keyboard_t *kbd = &default_kbd;
  atkbd_state_t *state = &default_kbd_state;

  if (inb(KBD_STATUS_PORT) == 0xff) {
    printf("atkbd: no i8042 controller\n");
    return;
  }
  state->kbdc = atkbdc_open();
  task_set(&state->inittask, atkbd_init_task, kbd);
  kbd_init_struct(kbd, "atkbd", FNU_KB_OTHER, 0, 0);
  kbd_default_maps(&default_keymap, &default_accentmap, default_fkeytab);
  kbd_set_maps(kbd, &default_keymap, &default_accentmap, default_fkeytab, FNU_KB_NFKEYS);
  kbd->kb_data = state;
  kbd->kb_sw = &atkbdsw;

  if (probe_keyboard(state->kbdc, 0) == 0)
    KBD_FOUND_DEVICE(kbd);
  atkbd_clear_state(kbd);
  state->x.mode = FNU_KB_XLATE;
  KBD_PROBE_DONE(kbd);

  if (KBD_HAS_DEVICE(kbd))
    init_keyboard(state->kbdc, &kbd->kb_type, kbd->kb_config);
  atkbd_setup(kbd);
  KBD_INIT_DONE(kbd);

  kbd_register(kbd);
  KBD_CONFIG_DONE(kbd);
  printf("atkbd0: %s keyboard%s\n", kbd_type_name(kbd->kb_type),
          KBD_HAS_DEVICE(kbd) ? "" : ", none answered");
}

// SLEEP
void atkbd_resume(void) {
  keyboard_t *kbd = &default_kbd;
  atkbd_state_t *state = (atkbd_state_t *)kbd->kb_data;
  int type = kbd->kb_type;

  if (state == 0)
    return;
  mtx_enter(&kbd_lock);
  atkbd_clear_state(kbd);
  mtx_leave(&kbd_lock);
  if (probe_keyboard(state->kbdc, KB_CONF_NO_RESET) != 0)
    return;
  // the firmware reset it on the way up already
  init_keyboard(state->kbdc, &type, kbd->kb_config | KB_CONF_NO_RESET);
  mtx_enter(&kbd_lock);
  kbd->kb_type = type;
  atkbd_setup(kbd);
  mtx_leave(&kbd_lock);
}

// a keyboard plugged in late is reset by a thread
static void atkbd_init_task(void *arg) {
  keyboard_t *kbd = arg;
  atkbd_state_t *state = (atkbd_state_t *)kbd->kb_data;
  int type = kbd->kb_type;

  if (init_keyboard(state->kbdc, &type, kbd->kb_config) != 0)
    return;
  mtx_enter(&kbd_lock);
  kbd->kb_type = type;
  KBD_FOUND_DEVICE(kbd);
  atkbd_setup(kbd);
  mtx_leave(&kbd_lock);
}

// kbd_lock held
void atkbd_intr_locked(void) {
  keyboard_t *kbd = &default_kbd;
  atkbd_state_t *state = (atkbd_state_t *)kbd->kb_data;
  uint32 c;

  if (kbd->kb_sw == 0)
    return;
  if (!KBD_HAS_DEVICE(kbd)) {
    task_add(systq, &state->inittask);
    return;
  }
  while ((c = atkbd_read_char(kbd)) != FNU_NOKEY)
    console_key(kbd, (int)c);
}
