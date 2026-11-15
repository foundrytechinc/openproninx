// THE ADAPTER

#include "acpi.h"
#include "aml.h"
#include "defs.h"
#include "inc/abi.h"
#include "malloc.h"
#include "sensors.h"

#define NOTIFY_STATUS 0x80

#define PSR_OFFLINE 0
#define PSR_ONLINE 1

struct adapter {
  struct aml_node *node;
  struct sensordev sd;
  struct sensor online;
};

int acpi_acline = -1;
static int units, registered;

static void refresh(struct adapter *a) {
  int64_t psr;

  if (aml_eval_int(a->node, "_PSR", 0, 0, &psr) != 0) {
    a->online.flags |= SENSOR_FINVALID;
    return;
  }
  a->online.flags &= ~SENSOR_FINVALID;
  a->online.value = psr == PSR_ONLINE;
  acpi_acline = psr == PSR_ONLINE;
}

static void notify(struct aml_node *node __unused, int value, void *arg) {
  if (value == NOTIFY_STATUS)
    refresh(arg);
}

static void poll(void *arg) { refresh(arg); }

static void attach(struct aml_node *dev, const char *hid __unused) {
  struct adapter *a = malloc(sizeof(*a), M_ACPI, M_WAITOK | M_ZERO);

  a->node = dev;
  dev->cookie = a;
  snprintf(a->sd.xname, sizeof(a->sd.xname), "acpiac%d", units++);
  a->online.type = SENSOR_INDICATOR;
  strlcpy(a->online.desc, "power supply", sizeof(a->online.desc));
  refresh(a);
  sensor_attach(&a->sd, &a->online);
  sensordev_install(&a->sd);
  aml_notify_register(dev, notify, a);
  acpi_poll_register(poll, a);
  if (!registered) {
    registered = 1;
    sysctl_add_int("hw.acpi.acline", &acpi_acline, 0, -1, 1);
  }
  printf("acpi: adapter %s %s\n", aml_nodename(dev),
          a->online.value ? "online" : "offline");
}

static const char *const hids[] = {ACPI_DEV_AC, 0};

const struct acpi_driver acpi_ac_driver = {"acpiac", hids, 0, attach};
