#ifndef PRONINX_STORAGE_AHCI_H
#define PRONINX_STORAGE_AHCI_H

#include "buf.h"
#include "driver.h"
#include "inc/types.h"

#define AHCI_MAX_PORTS 32
#define AHCI_SECTOR_SIZE 512
#define AHCI_NUM_SLOTS 32
#define AHCI_PRDT_PER_CMD 8
#define AHCI_PRDT_MAX (4u << 20)

// FIS types
#define FIS_TYPE_REG_H2D   0x27
#define FIS_TYPE_REG_D2H   0x34
#define FIS_TYPE_DMA_ACT   0x39
#define FIS_TYPE_DMA_SETUP 0x41
#define FIS_TYPE_DATA      0x46
#define FIS_TYPE_BIST      0x58
#define FIS_TYPE_PIO_SETUP 0x5F
#define FIS_TYPE_DEV_BITS  0xA1

// ATA commands
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_FLUSH_CACHE   0xE7
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA

// SATA signatures
#define SATA_SIG_ATA   0x00000101
#define SATA_SIG_ATAPI 0xEB140101
#define SATA_SIG_SEMB  0xC33C0101
#define SATA_SIG_PM    0x96690101

#define HBA_PORT_IPM_ACTIVE 1
#define HBA_PORT_DET_PRESENT 3

#define HBA_SCTL_DET_NONE 0x0
#define HBA_SCTL_DET_INIT 0x1
#define HBA_SCTL_IPM_DISABLED 0x300

#define HBA_PxCMD_ST  0x0001
#define HBA_PxCMD_SUD 0x0002
#define HBA_PxCMD_POD 0x0004
#define HBA_PxCMD_ICC_MASK 0xf0000000
#define HBA_PxCMD_ICC_ACTIVE 0x10000000
#define HBA_PxCMD_FRE 0x0010
#define HBA_PxCMD_FR  0x4000
#define HBA_PxCMD_CR  0x8000

#define HBA_CAP_S64A (1U << 31)

#define HBA_GHC_AE (1U << 31)
#define HBA_CAP2_BOH (1U << 0)  // BIOS/OS handoff
#define HBA_BOHC_BOS (1U << 0)
#define HBA_BOHC_OOS (1U << 1)
#define HBA_BOHC_BB (1U << 4)   // the BIOS is busy
#define HBA_GHC_IE (1U << 1)
#define HBA_GHC_HR (1U << 0)

// PxIS / PxIE
#define HBA_PxIS_DHRS  (1U << 0)
#define HBA_PxIS_PSS   (1U << 1)
#define HBA_PxIS_DSS   (1U << 2)
#define HBA_PxIS_SDBS  (1U << 3)
#define HBA_PxIS_UFS   (1U << 4)
#define HBA_PxIS_DPS   (1U << 5)
#define HBA_PxIS_PCS   (1U << 6)
#define HBA_PxIS_DMPS  (1U << 7)
#define HBA_PxIS_PRCS  (1U << 22)
#define HBA_PxIS_IPMS  (1U << 23)
#define HBA_PxIS_OFS   (1U << 24)
#define HBA_PxIS_INFS  (1U << 26)
#define HBA_PxIS_IFS   (1U << 27)
#define HBA_PxIS_HBDS  (1U << 28)
#define HBA_PxIS_HBFS  (1U << 29)
#define HBA_PxIS_TFES  (1U << 30)
#define HBA_PxIS_CPDS  (1U << 31)

#define HBA_PxIS_ERRORS                                                        \
  (HBA_PxIS_TFES | HBA_PxIS_HBFS | HBA_PxIS_HBDS | HBA_PxIS_IFS |              \
   HBA_PxIS_INFS | HBA_PxIS_OFS)

#define AHCI_SYNC_SLOT 31 // identify and cache flushes

#define HBA_PxIE_WANTED                                                        \
  (HBA_PxIS_DHRS | HBA_PxIS_SDBS | HBA_PxIS_ERRORS | HBA_PxIS_DPS |            \
   HBA_PxIS_UFS | HBA_PxIS_PCS | HBA_PxIS_PRCS)

struct hba_port {
  volatile uint32_t clb;
  volatile uint32_t clbu;
  volatile uint32_t fb;
  volatile uint32_t fbu;
  volatile uint32_t is;
  volatile uint32_t ie;
  volatile uint32_t cmd;
  volatile uint32_t rsv0;
  volatile uint32_t tfd;
  volatile uint32_t sig;
  volatile uint32_t ssts;
  volatile uint32_t sctl;
  volatile uint32_t serr;
  volatile uint32_t sact;
  volatile uint32_t ci;
  volatile uint32_t sntf;
  volatile uint32_t fbs;
  volatile uint32_t rsv1[11];
  volatile uint32_t vendor[4];
};

struct hba_mem {
  volatile uint32_t cap;
  volatile uint32_t ghc;
  volatile uint32_t is;
  volatile uint32_t pi;
  volatile uint32_t vs;
  volatile uint32_t ccc_ctl;
  volatile uint32_t ccc_pts;
  volatile uint32_t em_loc;
  volatile uint32_t em_ctl;
  volatile uint32_t cap2;
  volatile uint32_t bohc;
  volatile uint8_t rsv[0xA0 - 0x2C];
  volatile uint8_t vendor[0x100 - 0xA0];
  struct hba_port ports[32];
};

struct hba_prdt_entry {
  uint32_t dba;
  uint32_t dbau;
  uint32_t rsv0;
  uint32_t dbc : 22;
  uint32_t rsv1 : 9;
  uint32_t i : 1;
};

struct hba_cmd_header {
  uint8_t cfl : 5;
  uint8_t a : 1;
  uint8_t w : 1;
  uint8_t p : 1;
  uint8_t r : 1;
  uint8_t b : 1;
  uint8_t c : 1;
  uint8_t rsv0 : 1;
  uint8_t pmp : 4;
  uint16_t prdtl;
  volatile uint32_t prdbc;
  uint32_t ctba;
  uint32_t ctbau;
  uint32_t rsv1[4];
};

struct fis_reg_h2d {
  uint8_t  fis_type;
  uint8_t pmport : 4;
  uint8_t rsv0 : 3;
  uint8_t c : 1;
  uint8_t  command;
  uint8_t  featurel;
  uint8_t  lba0;
  uint8_t  lba1;
  uint8_t  lba2;
  uint8_t  device;
  uint8_t  lba3;
  uint8_t  lba4;
  uint8_t  lba5;
  uint8_t  featureh;
  uint8_t  countl;
  uint8_t  counth;
  uint8_t  icc;
  uint8_t  control;
  uint8_t  rsv1[4];
};

struct hba_cmd_tbl {
  uint8_t  cfis[64];
  uint8_t  acmd[16];
  uint8_t  rsv[48];
  struct hba_prdt_entry
      prdt[AHCI_PRDT_PER_CMD];
};

void ahci_driver_init(void);

uint64_t ahci_size(uint unit);
void ahcirw(struct buf *b);

#endif
