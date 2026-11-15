// BUTTONS

#include "acpi.h"
#include "aml.h"
#include "defs.h"
#include "malloc.h"
#include "sensors.h"

#define NOTIFY_PRESSED 0x80
#define NOTIFY_LID 0x80

struct button {
  struct aml_node *node;
  int kind;
  struct sensordev sd;
  struct sensor lid;
};

enum { POWER, SLEEP, LID };

static int units;

static void lid_read(struct button *b) {
  int64_t open;

  if (aml_eval_int(b->node, "_LID", 0, 0, &open) != 0) {
    b->lid.flags |= SENSOR_FINVALID;
    return;
  }
  b->lid.flags &= ~SENSOR_FINVALID;
  b->lid.value = open != 0;
}

static void notify(struct aml_node *node __unused, int value, void *arg) {
  struct button *b = arg;

  switch (b->kind) {
  case POWER:
    if (value == NOTIFY_PRESSED)
      acpi_power_pressed();
    break;
  case SLEEP:
    if (value == NOTIFY_PRESSED)
      acpi_sleep_pressed();
    break;
  case LID:
    if (value != NOTIFY_LID)
      break;
    lid_read(b);
    if (!(b->lid.flags & SENSOR_FINVALID) && b->lid.value == 0)
      acpi_lid_closed();
    else if (!(b->lid.flags & SENSOR_FINVALID))
      acpi_lid_opened();
    break;
  }
}

static void lid_poll(void *arg) { lid_read(arg); }

static void attach(struct aml_node *dev, const char *hid) {
  struct button *b = malloc(sizeof(*b), M_ACPI, M_WAITOK | M_ZERO);

  b->node = dev;
  b->kind = strcmp(hid, ACPI_DEV_PBD) == 0   ? POWER
            : strcmp(hid, ACPI_DEV_SBD) == 0 ? SLEEP
                                             : LID;
  dev->cookie = b;
  aml_notify_register(dev, notify, b);
  acpi_wake_device(dev);
  printf("acpi: %s button %s\n",
          b->kind == POWER ? "power" : b->kind == SLEEP ? "sleep" : "lid",
          aml_nodename(dev));
  if (b->kind != LID)
    return;
  snprintf(b->sd.xname, sizeof(b->sd.xname), "acpibtn%d", units++);
  b->lid.type = SENSOR_INDICATOR;
  strlcpy(b->lid.desc, "lid open", sizeof(b->lid.desc));
  lid_read(b);
  acpi_poll_register(lid_poll, b);
  sensor_attach(&b->sd, &b->lid);
  sensordev_install(&b->sd);
}

static const char *const hids[] = {ACPI_DEV_PBD, ACPI_DEV_SBD, ACPI_DEV_LD, 0};

const struct acpi_driver acpi_button_driver = {"acpibtn", hids, 0, attach};
