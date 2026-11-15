// THE BATTERY

#include "acpi.h"
#include "aml.h"
#include "defs.h"
#include "inc/abi.h"
#include "malloc.h"
#include "sensors.h"

#define NOTIFY_STATUS 0x80
#define NOTIFY_INFO 0x81
#define NOTIFY_CAPACITY 0x82

#define BIF_UNKNOWN 0xffffffffll
#define BST_DISCHARGE 0x01
#define BST_CHARGE 0x02
#define BST_CRITICAL 0x04


enum {
  S_DESIGN,
  S_LASTFULL,
  S_WARN,
  S_LOW,
  S_VDESIGN,
  S_VNOW,
  S_RATE,
  S_REMAINING,
  S_STATE,
  S_PERCENT,
  S_COUNT
};

struct battery {
  struct battery *link;
  struct aml_node *node;
  int present;
  int mah;
  int64_t design, lastfull, warn, low, vdesign;
  int64_t state, rate, remaining, volts;
  struct sensordev sd;
  struct sensor s[S_COUNT];
};

static struct battery *bats;
static int nbats;

static int life = -1, left = -1, state = 0, units;

static int64_t element(struct aml_value *pkg, int i) {
  if (i >= pkg->length || pkg->pkg[i]->type != AML_INTEGER)
    return BIF_UNKNOWN;
  return pkg->pkg[i]->integer & 0xffffffffll;
}

static int parse_info(struct battery *b, struct aml_value *res, int bix) {
  int o = bix ? 1 : 0;

  if (res->type != AML_PACKAGE || res->length < 7 + o)
    return -1;
  b->mah = element(res, o) == 1;
  b->design = element(res, o + 1);
  b->lastfull = element(res, o + 2);
  b->vdesign = element(res, o + 4);
  b->warn = element(res, o + 5);
  b->low = element(res, o + 6);
  return 0;
}

static void info(struct battery *b) {
  struct aml_value res;
  int64_t sta;
  int i, err = -1;

  b->present = 0;
  if (aml_eval_int(b->node, "_STA", 0, 0, &sta) != 0)
    sta = STA_PRESENT | STA_ENABLED | STA_BATTERY;
  if (!(sta & STA_BATTERY))
    return;
  if (aml_eval_name(b->node, "_BIX", 0, 0, &res) == 0) {
    err = parse_info(b, &res, 1);
    aml_freevalue(&res);
  }
  if (err != 0 && aml_eval_name(b->node, "_BIF", 0, 0, &res) == 0) {
    err = parse_info(b, &res, 0);
    aml_freevalue(&res);
  }
  if (err != 0)
    return;
  b->present = 1;
  for (i = S_DESIGN; i <= S_LOW; i++)
    b->s[i].type = b->mah ? SENSOR_AMPHOUR : SENSOR_WATTHOUR;
  b->s[S_RATE].type = b->mah ? SENSOR_AMPS : SENSOR_WATTS;
  b->s[S_REMAINING].type = b->mah ? SENSOR_AMPHOUR : SENSOR_WATTHOUR;
}

static void set(struct sensor *s, int64_t milli) {
  if (milli == BIF_UNKNOWN || milli < 0) {
    s->flags |= SENSOR_FUNKNOWN;
    return;
  }
  s->flags &= ~SENSOR_FUNKNOWN;
  s->value = milli * 1000;
}

static const char *statename(int64_t st) {
  if (st & BST_CRITICAL)
    return "critical";
  if (st & BST_CHARGE)
    return "charging";
  if (st & BST_DISCHARGE)
    return "discharging";
  return "idle";
}

// how it is
static void status(struct battery *b) {
  struct aml_value res;
  int i;

  b->state = BIF_UNKNOWN;
  if (b->present && aml_eval_name(b->node, "_BST", 0, 0, &res) == 0) {
    if (res.type == AML_PACKAGE && res.length >= 4) {
      b->state = element(&res, 0);
      b->rate = element(&res, 1);
      b->remaining = element(&res, 2);
      b->volts = element(&res, 3);
    }
    aml_freevalue(&res);
  }
  for (i = 0; i < S_COUNT; i++) {
    if (b->present && (b->state != BIF_UNKNOWN || i < S_VNOW))
      b->s[i].flags &= ~SENSOR_FINVALID;
    else
      b->s[i].flags |= SENSOR_FINVALID;
  }
  if (!b->present || b->state == BIF_UNKNOWN)
    return;
  set(&b->s[S_DESIGN], b->design);
  set(&b->s[S_LASTFULL], b->lastfull);
  set(&b->s[S_WARN], b->warn);
  set(&b->s[S_LOW], b->low);
  set(&b->s[S_VDESIGN], b->vdesign);
  set(&b->s[S_VNOW], b->volts);
  set(&b->s[S_RATE], b->rate);
  set(&b->s[S_REMAINING], b->remaining);
  b->s[S_STATE].value = b->state;
  b->s[S_STATE].flags &= ~SENSOR_FUNKNOWN;
  strlcpy(b->s[S_STATE].desc, statename(b->state), sizeof(b->s[S_STATE].desc));
  b->s[S_STATE].status = (b->state & BST_CRITICAL) ? SENSOR_S_CRIT : SENSOR_S_OK;
  if (b->lastfull != BIF_UNKNOWN && b->lastfull != 0 &&
      b->remaining != BIF_UNKNOWN) {
    b->s[S_PERCENT].value = b->remaining * 100000 / b->lastfull;
    b->s[S_PERCENT].flags &= ~SENSOR_FUNKNOWN;
  } else {
    b->s[S_PERCENT].flags |= SENSOR_FUNKNOWN;
  }
  b->s[S_REMAINING].status = SENSOR_S_OK;
  if (b->remaining != BIF_UNKNOWN && b->low != BIF_UNKNOWN &&
      b->remaining <= b->low)
    b->s[S_REMAINING].status = SENSOR_S_CRIT;
  else if (b->remaining != BIF_UNKNOWN && b->warn != BIF_UNKNOWN &&
           b->remaining <= b->warn)
    b->s[S_REMAINING].status = SENSOR_S_WARN;
}

static void total(void) {
  int64_t rem = 0, full = 0, rate = 0;
  struct battery *b;
  int any = 0, discharging = 0;

  state = 0;
  units = 0;
  for (b = bats; b != 0; b = b->link) {

    if (!b->present || b->state == BIF_UNKNOWN)
      continue;
    units++;
    any = 1;
    if (b->remaining != BIF_UNKNOWN && b->lastfull != BIF_UNKNOWN) {
      rem += b->remaining;
      full += b->lastfull;
    }
    if (b->state & BST_DISCHARGE) {
      discharging = 1;
      if (b->rate != BIF_UNKNOWN)
        rate += b->rate;
    }
    state |= (int)(b->state & 7);
  }
  life = any && full > 0 ? (int)(rem * 100 / full) : -1;
  left = discharging && rate > 0 ? (int)(rem * 60 / rate) : -1;
}

static void refresh(void *arg) {
  struct battery *b = arg;

  status(b);
  total();
}

static void notify(struct aml_node *node __unused, int value, void *arg) {
  struct battery *b = arg;

  if (value == NOTIFY_INFO || value == NOTIFY_CAPACITY)
    info(b);
  if (value == NOTIFY_STATUS || value == NOTIFY_INFO || value == NOTIFY_CAPACITY)
    refresh(b);
}

static const struct {
  const char *desc;
  enum sensor_type type;
} layout[S_COUNT] = {
    {"design capacity", SENSOR_WATTHOUR},
    {"last full capacity", SENSOR_WATTHOUR},
    {"warning capacity", SENSOR_WATTHOUR},
    {"low capacity", SENSOR_WATTHOUR},
    {"design voltage", SENSOR_VOLTS_DC},
    {"voltage", SENSOR_VOLTS_DC},
    {"rate", SENSOR_WATTS},
    {"remaining capacity", SENSOR_WATTHOUR},
    {"battery", SENSOR_INTEGER},
    {"charge", SENSOR_PERCENT},
};

static void attach(struct aml_node *dev, const char *hid __unused) {
  static int first = 1;
  struct battery *b;
  int i;

  b = malloc(sizeof(*b), M_ACPI, M_WAITOK | M_ZERO);
  b->node = dev;
  dev->cookie = b;
  snprintf(b->sd.xname, sizeof(b->sd.xname), "acpibat%d", nbats);
  info(b);
  for (i = 0; i < S_COUNT; i++) {
    b->s[i].type = layout[i].type;
    if (b->mah && layout[i].type == SENSOR_WATTHOUR)
      b->s[i].type = SENSOR_AMPHOUR;
    if (b->mah && layout[i].type == SENSOR_WATTS)
      b->s[i].type = SENSOR_AMPS;
    strlcpy(b->s[i].desc, layout[i].desc, sizeof(b->s[i].desc));
    b->s[i].flags = SENSOR_FINVALID;
    sensor_attach(&b->sd, &b->s[i]);
  }
  b->link = bats;
  bats = b;
  nbats++;
  status(b);
  total();
  sensordev_install(&b->sd);
  aml_notify_register(dev, notify, b);
  acpi_poll_register(refresh, b);
  if (first) {
    first = 0;
    sysctl_add_int("hw.acpi.battery.life", &life, 0, -1, 100);
    sysctl_add_int("hw.acpi.battery.time", &left, 0, -1, 0x7fffffff);
    sysctl_add_int("hw.acpi.battery.state", &state, 0, 0, 7);
    sysctl_add_int("hw.acpi.battery.units", &units, 0, 0, 0);
  }
  printf("acpi: battery %s %s\n", aml_nodename(dev),
          b->present ? "present" : "absent");
}

static const char *const hids[] = {ACPI_DEV_CMB, 0};

const struct acpi_driver acpi_battery_driver = {"acpibat", hids, 0, attach};
