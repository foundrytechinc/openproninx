// ACPI DEVICES

#include "acpi.h"
#include "aml.h"
#include "defs.h"

static const struct acpi_driver *const drivers[] = {
    &acpi_ec_driver,
    &acpi_button_driver,
    &acpi_ac_driver,
    &acpi_battery_driver,
    &acpi_ideapad_driver,
    &acpi_tpm_driver,
    &psm_acpi_driver,
};

static int id_text(struct aml_value *v, char *out, size_t n) {
  if (v->type == AML_STRING)
    strlcpy(out, v->str, (int)n);
  else if (v->type == AML_INTEGER)
    strlcpy(out, aml_eisaid((uint32_t)v->integer), (int)n);
  else
    return -1;
  return 0;
}

static int listed(const char *id, const char *const *list) {
  int i;

  for (i = 0; list[i] != 0; i++)
    if (strcmp(id, list[i]) == 0)
      return 1;
  return 0;
}

static const struct acpi_driver *driver_of(struct aml_node *dev, int early,
                                           char *id, size_t n) {
  struct aml_value hid, cid, *c;
  const struct acpi_driver *found = 0;
  uint i;
  int k, ncid;

  memset(&cid, 0, sizeof(cid));
  if (aml_eval_hid(dev, &hid) == 0) {
    if (id_text(&hid, id, n) == 0)
      for (i = 0; i < sizeof(drivers) / sizeof(drivers[0]) && !found; i++)
        if (drivers[i]->early == early && listed(id, drivers[i]->hids))
          found = drivers[i];
    aml_freevalue(&hid);
  }
  if (found == 0 && aml_eval_name(dev, "_CID", 0, 0, &cid) == 0) {
    ncid = cid.type == AML_PACKAGE ? cid.length : 1;
    for (k = 0; k < ncid && !found; k++) {
      c = cid.type == AML_PACKAGE ? cid.pkg[k] : &cid;
      if (id_text(c, id, n) != 0)
        continue;
      for (i = 0; i < sizeof(drivers) / sizeof(drivers[0]) && !found; i++)
        if (drivers[i]->early == early && listed(id, drivers[i]->hids))
          found = drivers[i];
    }
    aml_freevalue(&cid);
  }
  return found;
}

static int foundhid(struct aml_node *hid, void *arg) {
  struct aml_node *dev = hid->parent;
  int early = *(int *)arg;
  const struct acpi_driver *drv;
  char id[32];
  int64_t sta;

  sta = acpi_getsta(dev);
  if (!(sta & STA_PRESENT) || !(sta & STA_ENABLED) || dev->attached)
    return 0;
  if ((drv = driver_of(dev, early, id, sizeof(id))) != 0) {
    dev->attached = 1;
    drv->attach(dev, id);
  }
  return 0;
}

static int foundzone(struct aml_node *node, void *arg __unused) {
  if (node->value != 0 && node->value->type == AML_THERMZONE &&
      (acpi_getsta(node) & STA_PRESENT))
    acpi_thermal_attach(node);
  return 0;
}

// EC probe before any _INI touches it
void acpi_attach_early(void) {
  int early = 1;

  aml_find(aml_root(), "_HID", foundhid, &early);
}

void acpi_attach_devices(void) {
  int early = 0;

  aml_find(aml_root(), "_HID", foundhid, &early);
  aml_walk(aml_root(), 0, foundzone, 0);
}
