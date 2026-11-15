// VT-D, AS THE FIRMWARE LEFT IT
#include "acpi.h"
#include "defs.h"

#define DMAR_HEADER 48 // the table header, host address width, flags
#define DMAR_DRHD 0

#define VTD_GSTS 0x1c
#define VTD_GSTS_TES 0x80000000u // translation
#define VTD_PMEN 0x64
#define VTD_PMEN_EPM 0x80000000u // protected memory
#define VTD_PMEN_PRS 0x00000001u
#define VTD_PLMBASE 0x68
#define VTD_PLMLIMIT 0x6c
#define VTD_PHMBASE 0x70
#define VTD_PHMLIMIT 0x78

static uint32_t rd32(volatile uint8_t *r, uint off) {
  return *(volatile uint32_t *)(r + off);
}

static uint64_t rd64(volatile uint8_t *r, uint off) {
  return *(volatile uint64_t *)(r + off);
}

// nothing remaps DMA here, guarded regions would just break stuff
static void unit(uint64_t base) {
  volatile uint8_t *r = ioremap(base, 4096);
  uint32_t gsts, pmen;
  int i;

  if (r == 0)
    return;
  gsts = rd32(r, VTD_GSTS);
  pmen = rd32(r, VTD_PMEN);
  if (gsts & VTD_GSTS_TES)
    printf("dmar: unit at 0x%lx translates DMA; the devices may not reach "
           "memory\n",
           (unsigned long)base);
  if (!(pmen & (VTD_PMEN_EPM | VTD_PMEN_PRS)))
    return;
  printf("dmar: unit at 0x%lx guards 0x%x..0x%x and 0x%lx..0x%lx from DMA; "
         "letting it go\n",
         (unsigned long)base, rd32(r, VTD_PLMBASE), rd32(r, VTD_PLMLIMIT),
         (unsigned long)rd64(r, VTD_PHMBASE), (unsigned long)rd64(r, VTD_PHMLIMIT));
  *(volatile uint32_t *)(r + VTD_PMEN) = pmen & ~VTD_PMEN_EPM;
  for (i = 0; i < 1000 && (rd32(r, VTD_PMEN) & VTD_PMEN_PRS); i++)
    delay(100);
  if (rd32(r, VTD_PMEN) & VTD_PMEN_PRS)
    printf("dmar: unit at 0x%lx keeps guarding\n", (unsigned long)base);
}

void dmar_init(void) {
  const struct acpi_sdt_header *t = acpi_find_table("DMAR");
  const uint8_t *p, *end;
  uint16_t type, len;

  if (t == 0 || t->length < DMAR_HEADER)
    return;
  p = (const uint8_t *)t + DMAR_HEADER;
  end = (const uint8_t *)t + t->length;
  while (p + 4 <= end) {
    type = (uint16_t)(p[0] | p[1] << 8);
    len = (uint16_t)(p[2] | p[3] << 8);
    if (len < 4 || p + len > end)
      break;
    if (type == DMAR_DRHD && len >= 16)
      unit(*(const uint64_t *)(p + 8));
    p += len;
  }
}
