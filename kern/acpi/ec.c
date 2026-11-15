// THE EMBEDDED CONTROLLER

#include "acpi.h"
#include "aml.h"
#include "defs.h"
#include "malloc.h"
#include "x86.h"

#define EC_STAT_OBF 0x01
#define EC_STAT_IBF 0x02
#define EC_STAT_CMD 0x08
#define EC_STAT_BURST 0x10
#define EC_STAT_SCI 0x20 // an event waits

#define EC_CMD_READ 0x80
#define EC_CMD_WRITE 0x81
#define EC_CMD_QUERY 0x84

#define EC_SPIN 300

struct ec {
  struct ec *link;
  struct aml_node *node;
  int cport, dport; // command/status, and data
  int gpe;
  int glk;
  int ecdt;
};

static struct ec *ecs;

static int ec_sts(struct ec *e) { return inb((uint16_t)e->cport); }

// a few timeout lines, not a flood
static void ec_timeout(struct ec *e, int sts) {
  static int said;

  if (said++ < 8)
    printf("acpi: %s timed out, status 0x%x\n", aml_nodename(e->node), sts);
}

// a transaction is given up after 500 ms
#define EC_DELAY_US 500000

static int ec_wait(struct ec *e, int mask, int val) {
  uint64_t hz = delay_tsc_hz(), end = rdtsc() + hz / 1000000 * EC_DELAY_US;
  int sts, tries = 0;

  while (((sts = ec_sts(e)) & mask) != val) {
    if (hz != 0 ? rdtsc() > end : tries > EC_DELAY_US) {
      ec_timeout(e, sts);
      return -1;
    }
    if ((sts & EC_STAT_BURST) || tries++ < EC_SPIN)
      delay(1);
    else
      aml_msleep(1);
  }
  return 0;
}

static int ec_put(struct ec *e, int port, int v) {
  if (ec_wait(e, EC_STAT_IBF, 0) < 0)
    return -1;
  outb((uint16_t)port, (uint8_t)v);
  return 0;
}

static int ec_get(struct ec *e) {
  if (ec_wait(e, EC_STAT_OBF, EC_STAT_OBF) < 0)
    return -1;
  return inb((uint16_t)e->dport);
}

static int ec_read(struct ec *e, int addr) {
  if (ec_put(e, e->cport, EC_CMD_READ) < 0 || ec_put(e, e->dport, addr) < 0)
    return -1;
  return ec_get(e);
}

static int ec_write(struct ec *e, int addr, int v) {
  if (ec_put(e, e->cport, EC_CMD_WRITE) < 0 || ec_put(e, e->dport, addr) < 0 ||
      ec_put(e, e->dport, v) < 0)
    return -1;
  return ec_wait(e, EC_STAT_IBF, 0);
}

static int ec_region(void *cookie, int dir, uint64_t addr, int size,
                  uint64_t *val) {
  struct ec *e = cookie;
  uint64_t v = 0;
  int i, r = 0;

  if (addr > 0xff || addr + (uint64_t)size > 0x100)
    return -1;
  if (e->glk)
    aml_glk_enter();
  for (i = 0; i < size && r >= 0; i++) {
    if (dir == ACPI_IOREAD) {
      if ((r = ec_read(e, (int)addr + i)) >= 0)
        v |= (uint64_t)r << (8 * i);
    } else
      r = ec_write(e, (int)addr + i, (int)(*val >> (8 * i)) & 0xff);
  }
  if (e->glk)
    aml_glk_leave();
  if (dir == ACPI_IOREAD)
    *val = v;
  return r < 0 ? -1 : 0;
}

static int ec_query(struct ec *e) {
  int q;

  if (e->glk)
    aml_glk_enter();
  q = ec_put(e, e->cport, EC_CMD_QUERY) < 0 ? -1 : ec_get(e);
  if (e->glk)
    aml_glk_leave();
  return q;
}

static void drain(struct ec *e) {
  char name[8];
  int q;

  aml_lock();
  while (ec_sts(e) & EC_STAT_SCI) {
    if ((q = ec_query(e)) <= 0)
      break;
    snprintf(name, sizeof(name), "_Q%02X", q);
    aml_eval_name(e->node, name, 0, 0, 0);
  }
  aml_unlock();
}

static int ec_event(int gpe, void *arg) {
  drain(arg);
  acpi_gpe_done(gpe);
  return 0;
}

// an event raised during sleep has no edge left
static void pending(void *arg0, int dummy) {
  struct ec *e;

  (void)arg0;
  (void)dummy;
  for (e = ecs; e != 0; e = e->link)
    if (ec_sts(e) & EC_STAT_SCI)
      drain(e);
}

void acpi_ec_resume(void) {
  if (ecs != 0)
    acpi_task_add(pending, 0, 0);
}

static void start(struct ec *e) {
  int64_t glk;

  if (e->cport == 0 || e->dport == 0 || e->node == 0) {
    printf("acpi: the embedded controller has no ports, not used\n");
    free(e, M_ACPI);
    return;
  }
  if (aml_eval_int(e->node, "_GLK", 0, 0, &glk) == 0)
    e->glk = glk != 0;
  e->node->attached = 1;
  e->node->cookie = e;
  printf("acpi: embedded controller %s at 0x%x,0x%x GPE 0x%x%s%s\n",
          aml_nodename(e->node), e->dport, e->cport, e->gpe,
          e->ecdt ? ", ECDT" : "", e->glk ? ", global lock" : "");
  if (ecs == 0)
    aml_region_default(ACPI_OPREG_EC, e, ec_region);
  e->link = ecs;
  ecs = e;
  aml_region_register(e->node, ACPI_OPREG_EC, e, ec_region);
  if (e->gpe >= 0 && acpi_gpe_handler(e->gpe, ec_event, e, GPE_LEVEL) != 0)
    printf("acpi: no GPE 0x%x for the embedded controller\n", e->gpe);
}

static int ports(int idx __unused, union acpi_resource *crs, void *arg) {
  struct ec *e = arg;

  if (AML_CRSTYPE(crs) == SR_IOPORT || AML_CRSTYPE(crs) == SR_FIXEDPORT) {
    int base = AML_CRSTYPE(crs) == SR_IOPORT ? crs->sr_ioport._min
                                             : crs->sr_fioport._bas;

    if (e->dport == 0)
      e->dport = base;
    else if (e->cport == 0)
      e->cport = base;
  }
  return 0;
}

static void attach(struct aml_node *dev, const char *hid __unused) {
  struct aml_value res;
  struct ec *e = malloc(sizeof(*e), M_ACPI, M_WAITOK | M_ZERO);

  e->node = dev;
  e->gpe = -1;
  if (aml_eval_name(dev, "_CRS", 0, 0, &res) == 0) {
    aml_parse_resource(&res, ports, e);
    aml_freevalue(&res);
  }
  if (aml_eval_name(dev, "_GPE", 0, 0, &res) == 0) {
    e->gpe = (int)(res.type == AML_PACKAGE && res.length >= 2
                       ? aml_val2int(res.pkg[1])
                       : aml_val2int(&res));
    aml_freevalue(&res);
  }
  start(e);
}

// ECDT
void acpi_ec_ecdt(void) {
  struct acpi_ecdt *t = acpi_find_table("ECDT");
  struct aml_node *dev;
  struct ec *e;

  if (t == 0 || ecs != 0)
    return;
  if (t->ec_control.address_space_id != GAS_SYSTEM_IOSPACE ||
      t->ec_data.address_space_id != GAS_SYSTEM_IOSPACE)
    return;
  if (t->header.length <= sizeof(*t) - 1 ||
      (dev = aml_search(aml_root(), t->ec_id)) == 0)
    return;
  e = malloc(sizeof(*e), M_ACPI, M_WAITOK | M_ZERO);
  e->node = dev;
  e->cport = (int)t->ec_control.address;
  e->dport = (int)t->ec_data.address;
  e->gpe = t->gpe_bit;
  e->ecdt = 1;
  start(e);
}

static const char *const hids[] = {ACPI_DEV_ECD, 0};

const struct acpi_driver acpi_ec_driver = {"acpiec", hids, 1, attach};
