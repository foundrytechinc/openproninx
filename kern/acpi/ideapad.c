// LENOVO IDEAPAD AND LEGION
// VPC2004, mailbox VPCR/VPCW. lenovo, ну вы и пидоры

#include "acpi.h"
#include "aml.h"
#include "defs.h"
#include "kbdlight.h"
#include "malloc.h"

#define NOTIFY_EVENTS 0x80

#define HALS_KBD_LIGHT 0x10 // it has one
#define HALS_KBD_LIGHT_ON 0x20
#define SALS_KBD_LIGHT_ON 0x08
#define SALS_KBD_LIGHT_OFF 0x09

#define VPCCMD_R_VPC1 0x10
#define VPCCMD_R_VPC2 0x1a

#define EVENT_KBD_LIGHT 1
#define EVENT_KBLC 12

#define EC_TIMEOUT_MS 200

struct ideapad {
  struct aml_node *node;
  struct kbdlight light;
  char name[32];
};

static int vpcr(struct ideapad *p, int64_t which, int64_t *v) {
  struct aml_value arg;

  memset(&arg, 0, sizeof(arg));
  arg.type = AML_INTEGER;
  arg.integer = which;
  return aml_eval_int(p->node, "VPCR", 1, &arg, v);
}

static int vpcw(struct ideapad *p, int64_t which, int64_t v) {
  struct aml_value arg[2];

  memset(arg, 0, sizeof(arg));
  arg[0].type = AML_INTEGER;
  arg[0].integer = which;
  arg[1].type = AML_INTEGER;
  arg[1].integer = v;
  return aml_eval_name(p->node, "VPCW", 2, arg, 0);
}

// VPCR(1) reads 0: the EC took it. maybe
static int ec_data(struct ideapad *p, int cmd, int64_t *v) {
  int64_t busy;
  int ms;

  if (vpcw(p, 1, cmd) != 0)
    return -1;
  for (ms = 0; ms <= EC_TIMEOUT_MS; ms++) {
    if (vpcr(p, 1, &busy) != 0)
      return -1;
    if (busy == 0)
      return vpcr(p, 0, v);
    aml_msleep(1);
  }
  return -1;
}

static int hals(struct ideapad *p, int64_t *v) {
  return aml_eval_int(p->node, "HALS", 0, 0, v);
}

static int light_get(void *arg) {
  int64_t v;

  if (hals(arg, &v) != 0)
    return -1;
  return (v & HALS_KBD_LIGHT_ON) != 0;
}

static void light_task(void *arg, int on) {
  struct ideapad *p = arg;
  struct aml_value v;

  memset(&v, 0, sizeof(v));
  v.type = AML_INTEGER;
  v.integer = on ? SALS_KBD_LIGHT_ON : SALS_KBD_LIGHT_OFF;
  if (aml_eval_name(p->node, "SALS", 1, &v, 0) != 0)
    printf("acpi: %s would not set the keyboard light\n", aml_nodename(p->node));
}

static void light_set(void *arg, int level) { acpi_task_add(light_task, arg, level != 0); }

static void notify(struct aml_node *node, int value, void *arg) {
  struct ideapad *p = arg;
  int64_t vpc1, vpc2, events;

  (void)node;
  if (value != NOTIFY_EVENTS)
    return;
  if (ec_data(p, VPCCMD_R_VPC1, &vpc1) != 0 || ec_data(p, VPCCMD_R_VPC2, &vpc2) != 0)
    return;
  events = (vpc2 << 8) | vpc1;
  if (events & ((1 << EVENT_KBD_LIGHT) | (1 << EVENT_KBLC)))
    kbdlight_changed(light_get(p));
}

static void attach(struct aml_node *dev, const char *hid) {
  struct ideapad *p;
  int64_t v;

  (void)hid;
  p = malloc(sizeof(*p), M_ACPI, M_WAITOK | M_ZERO);
  p->node = dev;
  dev->cookie = p;
  aml_notify_register(dev, notify, p);
  if (hals(p, &v) != 0)
    v = 0;
  printf("acpi: ideapad %s%s\n", aml_nodename(dev),
         (v & HALS_KBD_LIGHT) ? ", keyboard light" : "");
  if (!(v & HALS_KBD_LIGHT))
    return;
  strlcpy(p->name, aml_nodename(dev), sizeof(p->name));
  p->light.name = p->name;
  p->light.max = 1;
  p->light.get = light_get;
  p->light.set = light_set;
  p->light.arg = p;
  kbdlight_attach(&p->light);
}

static const char *const hids[] = {"VPC2004", 0};

const struct acpi_driver acpi_ideapad_driver = {"ideapad", hids, 0, attach};
