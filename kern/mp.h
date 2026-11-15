#ifndef PRONINX_X86_64_MP_H
#define PRONINX_X86_64_MP_H


#include "inc/types.h"

struct mp {
  char signature[4];
  uint32_t physaddr;
  uint8_t length;
  uint8_t specrev;
  uint8_t checksum;
  uint8_t type;
  uint8_t imcrp;
  uint8_t reserved[3];
} __packed;

struct mpconf {
  char signature[4];
  uint16_t length;
  uint8_t version;
  uint8_t checksum;
  uint8_t product[20];
  uint32_t oemtable;
  uint16_t oemlength;
  uint16_t entry;
  uint32_t lapicaddr;
  uint16_t xlength;
  uint8_t xchecksum;
  uint8_t reserved;
} __packed;

struct mpproc {
  uint8_t type;
  uint8_t apicid;
  uint8_t version;
  uint8_t flags;
#define MPBOOT 0x02
  char signature[4];
  uint32_t feature;
  uint8_t reserved[8];
} __packed;

struct mpioapic {
  uint8_t type;
  uint8_t apicno;
  uint8_t version;
  uint8_t flags;
  uint32_t addr;
} __packed;

#define MPPROC 0x00
#define MPBUS 0x01
#define MPIOAPIC 0x02
#define MPIOINTR 0x03
#define MPLINTR 0x04

// MPspec 4.3.3 and 4.3.4
struct mpbus {
  uint8_t type;
  uint8_t busid;
  char busname[6];
} __packed;

struct mpiointr {
  uint8_t type;
  uint8_t inttype;
  uint16_t flags;
  uint8_t srcbus;
  uint8_t srcirq;
  uint8_t dstapic;
  uint8_t dstintin;
} __packed;

#endif /* ifndef PRONINX_X86_64_MP_H */
