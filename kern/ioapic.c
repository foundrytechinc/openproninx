// I/O APICS
#include "defs.h"
#include "malloc.h"
#include "mutex.h"
#include "proc.h"
#include "trap.h"
#include "acpi/acpi.h"

#define REG_ID 0x00
#define REG_VER 0x01
#define REG_TABLE 0x10

#define INT_DISABLED 0x00010000
#define INT_LEVEL 0x00008000
#define INT_ACTIVELOW 0x00002000

struct ioapic_regs {
  uint32_t reg;
  uint32_t pad[3];
  uint32_t data;
};

struct ioapic_unit {
  volatile struct ioapic_regs *regs;
  int id;
  int gsibase;
  int npins;
  uint32_t *saved;
};

static struct {
  struct mutex lock;
  struct ioapic_unit *unit;
  int n;
  int *vector;
  int ngsi;
} io;

static uint32_t rd(struct ioapic_unit *u, int reg) {
  u->regs->reg = (uint32_t)reg;
  return u->regs->data;
}

static void wr(struct ioapic_unit *u, int reg, uint32_t data) {
  u->regs->reg = (uint32_t)reg;
  u->regs->data = data;
}

static void found(int id, uint64_t addr, int gsibase) {
  struct ioapic_unit *grown, *u;
  int i;

  grown = malloc((size_t)(io.n + 1) * sizeof(*grown), M_DEVBUF, M_WAITOK);
  if (io.n > 0)
    memmove(grown, io.unit, (size_t)io.n * sizeof(*grown));
  if (io.unit != 0)
    free(io.unit, M_DEVBUF);
  io.unit = grown;
  u = &io.unit[io.n++];
  u->regs = (volatile struct ioapic_regs *)ioremap(addr, 4096);
  u->id = id;
  u->gsibase = gsibase >= 0 ? gsibase : io.ngsi;
  gsibase = u->gsibase;
  u->npins = (int)((rd(u, REG_VER) >> 16) & 0xff) + 1;
  u->saved = malloc((size_t)u->npins * 2 * sizeof(uint32_t), M_DEVBUF,
                    M_WAITOK | M_ZERO);
  if (id < 0)
    u->id = id = (int)(rd(u, REG_ID) >> 24);
  if ((int)(rd(u, REG_ID) >> 24) != id)
    printf("IOAPIC: id %d answers as %d\n", id, (int)(rd(u, REG_ID) >> 24));
  for (i = 0; i < u->npins; i++) {
    wr(u, REG_TABLE + 2 * i, INT_DISABLED);
    wr(u, REG_TABLE + 2 * i + 1, 0);
  }
  if (gsibase + u->npins > io.ngsi)
    io.ngsi = gsibase + u->npins;
  printf("IOAPIC: id %d, global interrupts %d to %d\n", id, gsibase,
          gsibase + u->npins - 1);
}

void ioapicinit(void) {
  mtx_init(&io.lock, "ioapic");
  if (acpi_ioapic_each(found) <= 0 && mp_ioapic_each(found) <= 0)
    panic("ioapicinit: neither the MADT nor the MP table names an I/O APIC");
  io.vector = malloc((size_t)io.ngsi * sizeof(*io.vector), M_DEVBUF,
                     M_WAITOK | M_ZERO);
}

static struct ioapic_unit *unit_of(int gsi) {
  int i;

  for (i = 0; i < io.n; i++)
    if (gsi >= io.unit[i].gsibase && gsi < io.unit[i].gsibase + io.unit[i].npins)
      return &io.unit[i];
  return 0;
}

int ioapic_gsi_vector(int gsi, int cpu, int level, int low) {
  struct ioapic_unit *u;
  int v, pin;

  if (gsi < 0 || gsi >= io.ngsi || (u = unit_of(gsi)) == 0)
    return -1;
  mtx_enter(&io.lock);
  if ((v = io.vector[gsi]) != 0) {
    mtx_leave(&io.lock);
    return v;
  }
  mtx_leave(&io.lock);
  if ((v = intr_vector_alloc()) < 0)
    return -1;
  mtx_enter(&io.lock);
  if (io.vector[gsi] != 0) {
    // another asked for it meanwhile
    mtx_leave(&io.lock);
    intr_vector_free(v);
    return io.vector[gsi];
  }
  io.vector[gsi] = v;
  pin = gsi - u->gsibase;
  wr(u, REG_TABLE + 2 * pin + 1, (uint32_t)cpus[cpu].apicid << 24);
  wr(u, REG_TABLE + 2 * pin,
     (uint32_t)v | (level ? INT_LEVEL : 0) | (low ? INT_ACTIVELOW : 0));
  mtx_leave(&io.lock);
  return v;
}

int ioapic_isa_vector(int irq, int cpu) {
  int gsi, level, low;

  acpi_isa_irq(irq, 0, &gsi, &level, &low);
  return ioapic_gsi_vector(gsi, cpu, level, low);
}

// SLEEP
void ioapic_save(void) {
  int i, k;

  for (k = 0; k < io.n; k++)
    for (i = 0; i < 2 * io.unit[k].npins; i++)
      io.unit[k].saved[i] = rd(&io.unit[k], REG_TABLE + i);
}

void ioapic_restore(void) {
  struct ioapic_unit *u;
  int i, k;

  for (k = 0; k < io.n; k++) {
    u = &io.unit[k];
    for (i = 0; i < u->npins; i++) {
      wr(u, REG_TABLE + 2 * i, INT_DISABLED);
      wr(u, REG_TABLE + 2 * i + 1, u->saved[2 * i + 1]);
      wr(u, REG_TABLE + 2 * i, u->saved[2 * i]);
    }
  }
}
