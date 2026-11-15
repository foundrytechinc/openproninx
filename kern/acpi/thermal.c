// THERMAL ZONES

#include "acpi.h"
#include "aml.h"
#include "defs.h"
#include "malloc.h"
#include "sensors.h"

#define NOTIFY_TEMP 0x80
#define NOTIFY_TRIPS 0x81
#define NOTIFY_DEVICES 0x82

#define NAC 10
#define NONE (-1)
#define PERFSTEP 10
#define TMP_RETRY 3
#define ZERO_C 2732

struct zone {
  struct zone *link;
  struct aml_node *node;
  int64_t tmp, psv, hot, crt;
  int64_t tc1, tc2, tsp, tzp;
  int64_t prev;
  int passive;
  int cap;      // to this percentage
  int64_t ac[NAC];
  int on[NAC];
  int critical;
  struct sensordev sd;
  struct sensor temp;
};

static int units;
static struct zone *zones;

// the slowest any zone wants
static void cap_all(void) {
  struct zone *z;
  int cap = 100;

  for (z = zones; z != 0; z = z->link)
    if (z->cap < cap)
      cap = z->cap;
  acpi_cpu_cap(cap);
}

static int64_t reading(struct aml_node *node, const char *name) {
  int64_t v;

  if (aml_eval_int(node, name, 0, 0, &v) != 0 || (uint32_t)v == 0xffffffffu)
    return NONE;
  return v;
}

static int64_t temperature(struct aml_node *node, const char *name) {
  int64_t v = NONE;
  int i;

  for (i = 0; i < TMP_RETRY; i++) {
    if ((v = reading(node, name)) == NONE || v >= ZERO_C)
      return v;
    aml_msleep(1000);
  }
  printf("acpi: %s: %s reads %ld, below freezing\n", aml_nodename(node), name,
          (long)v);
  return NONE;
}

static int64_t trip(struct aml_node *node, const char *name) {
  return temperature(node, name);
}

static void trips(struct zone *z) {
  char name[8];
  int i;

  z->psv = trip(z->node, "_PSV");
  z->hot = trip(z->node, "_HOT");
  z->crt = trip(z->node, "_CRT");
  z->tc1 = reading(z->node, "_TC1");
  z->tc2 = reading(z->node, "_TC2");
  z->tsp = reading(z->node, "_TSP");
  z->tzp = reading(z->node, "_TZP");
  for (i = 0; i < NAC; i++) {
    snprintf(name, sizeof(name), "_AC%d", i);
    z->ac[i] = reading(z->node, name);
  }
}

static void power(struct aml_node *node, int on) {
  struct aml_value res;
  int i;

  if (node == 0 || node->value == 0)
    return;
  if (node->value->type == AML_POWERRSRC) {
    aml_eval_name(node, on ? "_ON_" : "_OFF", 0, 0, 0);
    return;
  }
  if (node->value->type != AML_DEVICE ||
      aml_eval_name(node, "_PR0", 0, 0, &res) != 0)
    return;
  if (res.type == AML_PACKAGE) {
    for (i = 0; i < res.length; i++) {
      struct aml_value *r = res.pkg[i];

      if (r->type == AML_OBJREF)
        r = r->objref.ref;
      if (r != 0 && r->node != 0 && r->type == AML_POWERRSRC)
        power(r->node, on);
    }
  }
  aml_freevalue(&res);
}

static void cool(struct zone *z, int level, int on) {
  struct aml_value res;
  char name[8];
  int i;

  snprintf(name, sizeof(name), "_AL%d", level);
  if (aml_eval_name(z->node, name, 0, 0, &res) != 0)
    return;
  if (res.type == AML_PACKAGE) {
    for (i = 0; i < res.length; i++) {
      struct aml_value *r = res.pkg[i];

      if (r->type == AML_OBJREF)
        r = r->objref.ref;
      if (r != 0)
        power(r->node, on);
    }
  }
  aml_freevalue(&res);
  printf("acpi: %s cooling %d %s at %d.%d degC\n", aml_nodename(z->node), level,
          on ? "on" : "off", (int)((z->tmp - ZERO_C) / 10),
          (int)((z->tmp - ZERO_C) % 10));
}

static void refresh(void *arg);

// PASSIVE COOLING
static void passive(struct zone *z) {
  int64_t trend;

  if (z->psv == NONE || z->tc1 == NONE || z->tc2 == NONE ||
      !acpi_cpu_has_perf())
    return;
  if (z->tmp >= z->psv && !z->passive) {
    z->passive = 1;
    if (z->tsp != NONE && z->tsp > 0)
      acpi_poll_every(refresh, z, (int)z->tsp);
  }
  if (z->prev == NONE) {
    z->prev = z->tmp;
    return;
  }
  if (z->tmp >= z->psv) {
    trend = z->tc1 * (z->tmp - z->prev) + z->tc2 * (z->tmp - z->psv);
    z->cap += trend > 0 ? -PERFSTEP : PERFSTEP;
  } else {
    z->cap += PERFSTEP;
  }
  z->prev = z->tmp;
  if (z->cap > 100)
    z->cap = 100;
  if (z->cap < 0)
    z->cap = 0;
  if (z->passive && z->cap == 100 && z->tmp < z->psv) {
    z->passive = 0;
    acpi_poll_every(refresh, z, z->tzp > 0 ? (int)z->tzp : 100);
  }
  cap_all();
}

static void refresh(void *arg) {
  struct zone *z = arg;
  int64_t tmp;
  int i;

  if ((tmp = temperature(z->node, "_TMP")) == NONE) {
    z->temp.flags |= SENSOR_FINVALID;
    return;
  }
  z->tmp = tmp;
  z->temp.flags &= ~SENSOR_FINVALID;
  z->temp.value = tmp * 100000 - 50000;
  z->temp.status = SENSOR_S_OK;
  if (z->psv != NONE && tmp >= z->psv)
    z->temp.status = SENSOR_S_WARN;
  if (z->hot != NONE && tmp >= z->hot)
    z->temp.status = SENSOR_S_WARN;
  if (z->crt != NONE && tmp >= z->crt)
    z->temp.status = SENSOR_S_CRIT;

  for (i = 0; i < NAC; i++) {
    if (z->ac[i] == NONE)
      continue;
    if (z->on[i] != 1 && tmp >= z->ac[i]) {
      z->on[i] = 1;
      cool(z, i, 1);
    } else if (z->on[i] != 0 && tmp < z->ac[i]) {
      z->on[i] = 0;
      cool(z, i, 0);
    }
  }
  passive(z);
  if (z->temp.status == SENSOR_S_CRIT && !z->critical) {
    z->critical = 1;
    printf("acpi: %s is critically hot, powering down\n", aml_nodename(z->node));
    acpi_powerbutton();
  }
}

static void notify(struct aml_node *node __unused, int value, void *arg) {
  struct zone *z = arg;

  if (value == NOTIFY_TRIPS || value == NOTIFY_DEVICES)
    trips(z);
  if (value == NOTIFY_TEMP || value == NOTIFY_TRIPS || value == NOTIFY_DEVICES)
    refresh(z);
}

void acpi_thermal_attach(struct aml_node *node) {
  struct aml_value arg;
  struct zone *z;
  int i;

  if (node->attached)
    return;
  node->attached = 1;
  z = malloc(sizeof(*z), M_ACPI, M_WAITOK | M_ZERO);
  z->node = node;
  z->cap = 100;
  z->prev = NONE;
  for (i = 0; i < NAC; i++)
    z->on[i] = NONE;
  z->link = zones;
  zones = z;
  node->cookie = z;
  memset(&arg, 0, sizeof(arg));
  arg.type = AML_INTEGER;
  aml_eval_name(node, "_SCP", 1, &arg, 0);
  trips(z);
  snprintf(z->sd.xname, sizeof(z->sd.xname), "acpitz%d", units++);
  z->temp.type = SENSOR_TEMP;
  strlcpy(z->temp.desc, "zone temperature", sizeof(z->temp.desc));
  z->temp.flags = SENSOR_FINVALID;
  acpi_poll_every(refresh, z, z->tzp > 0 ? (int)z->tzp : 100);
  refresh(z);
  sensor_attach(&z->sd, &z->temp);
  sensordev_install(&z->sd);
  aml_notify_register(node, notify, z);
  printf("acpi: thermal zone %s", aml_nodename(node));
  if (!(z->temp.flags & SENSOR_FINVALID))
    printf(" %d.%d degC", (int)((z->tmp - ZERO_C) / 10),
            (int)((z->tmp - ZERO_C) % 10));
  if (z->crt != NONE)
    printf(", critical at %d degC", (int)((z->crt - ZERO_C) / 10));
  printf("\n");
}
