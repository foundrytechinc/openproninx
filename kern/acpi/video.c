// THE BACKLIGHT: _BCL, _BCM, _BQC

#include "acpi.h"
#include "aml.h"
#include "defs.h"

static struct aml_node *panel;
static int looked;
static int64_t was = -1;

static int pick(struct aml_node *bcm, void *arg __unused) {
  if (bcm->parent == 0 || aml_search(bcm->parent, "_BCL") == 0)
    return 0;
  panel = bcm->parent;
  return 1;
}

// _BCL: on AC, on battery, then the levels
static int levels(int64_t *low, int64_t *full) {
  struct aml_value res;
  int i, ok = -1;

  if (aml_eval_name(panel, "_BCL", 0, 0, &res) != 0)
    return -1;
  if (res.type == AML_PACKAGE && res.length >= 3) {
    *full = aml_val2int(res.pkg[0]);
    *low = aml_val2int(res.pkg[2]);
    for (i = 3; i < res.length; i++)
      if (aml_val2int(res.pkg[i]) < *low)
        *low = aml_val2int(res.pkg[i]);
    ok = 0;
  }
  aml_freevalue(&res);
  return ok;
}

static void set(int64_t level) {
  struct aml_value arg;

  memset(&arg, 0, sizeof(arg));
  arg.type = AML_INTEGER;
  arg.integer = level;
  aml_eval_name(panel, "_BCM", 1, &arg, 0);
}

// dark, the level kept for later
void acpi_backlight_off(void) {
  int64_t low, full;

  if (!looked) {
    looked = 1;
    aml_find(aml_root(), "_BCM", pick, 0);
  }
  if (panel == 0 || levels(&low, &full) < 0)
    return;
  if (aml_eval_int(panel, "_BQC", 0, 0, &was) != 0)
    was = full;
  set(low);
}

void acpi_backlight_on(void) {
  if (panel != 0 && was >= 0)
    set(was);
  was = -1;
}
