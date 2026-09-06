#ifndef PRONINX_STORAGE_AHCI_H
#define PRONINX_STORAGE_AHCI_H

#include "inc/types.h"
#include "buf.h"
#include "driver.h"

#define AHCI_MAX_PORTS 32
#define AHCI_SECTOR_SIZE 512
#define AHCI_NUM_SLOTS 32
#define AHCI_PRDT_PER_CMD 8

// FIS Types
#define FIS_TYPE_REG_H2D   0x27 // Register FIS - host to device
#define FIS_TYPE_REG_D2H   0x34 // Register FIS - device to host
#define FIS_TYPE_DMA_ACT   0x39 // DMA activate FIS - device to host
#define FIS_TYPE_DMA_SETUP 0x41 // DMA setup FIS - bidirectional
#define FIS_TYPE_DATA      0x46 // Data FIS - bidirectional
#define FIS_TYPE_BIST      0x58 // BIST activate FIS - bidirectional
#define FIS_TYPE_PIO_SETUP 0x5F // PIO setup FIS - device to host
#define FIS_TYPE_DEV_BITS  0xA1 // Set device bits FIS - device to host

// ATA Commands
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_IDENTIFY      0xEC

// SATA Device Signatures
#define SATA_SIG_ATA   0x00000101 // SATA drive
#define SATA_SIG_ATAPI 0xEB140101 // SATAPI drive
#define SATA_SIG_SEMB  0xC33C0101 // Enclosure management bridge
#define SATA_SIG_PM    0x96690101 // Port multiplier

#define HBA_PORT_IPM_ACTIVE 1
#define HBA_PORT_DET_PRESENT 3

#define HBA_PxCMD_ST  0x0001 // Start
#define HBA_PxCMD_FRE 0x0010 // FIS Receive Enable
#define HBA_PxCMD_FR  0x4000 // FIS Receive Running
#define HBA_PxCMD_CR  0x8000 // Command List Running

#define HBA_GHC_AE (1U << 31) // AHCI Enable
#define HBA_GHC_IE (1U << 1)  // Interrupt Enable
#define HBA_GHC_HR (1U << 0)  // HBA Reset

// Port Interrupt Status / Enable flags
#define HBA_PxIS_DHRS  (1U << 0)  // Device to Host Register FIS Interrupt
#define HBA_PxIS_PSS   (1U << 1)  // PIO Setup FIS Interrupt
#define HBA_PxIS_DSS   (1U << 2)  // DMA Setup FIS Interrupt
#define HBA_PxIS_SDBS  (1U << 3)  // Set Device Bits Interrupt
#define HBA_PxIS_UFS   (1U << 4)  // Unknown FIS Interrupt
#define HBA_PxIS_DPS   (1U << 5)  // Descriptor Processed Interrupt
#define HBA_PxIS_PCS   (1U << 6)  // Port Connect Change Status
#define HBA_PxIS_DMPS  (1U << 7)  // Device Mechanical Presence Status
#define HBA_PxIS_PRCS  (1U << 22) // PhyReady Change Status
#define HBA_PxIS_IPMS  (1U << 23) // Incorrect Port Multiplier Status
#define HBA_PxIS_OFS   (1U << 24) // Overflow Status
#define HBA_PxIS_INFS  (1U << 26) // Interface Non-fatal Error Status
#define HBA_PxIS_IFS   (1U << 27) // Interface Fatal Error Status
#define HBA_PxIS_HBDS  (1U << 28) // Host Bus Data Error Status
#define HBA_PxIS_HBFS  (1U << 29) // Host Bus Fatal Error Status
#define HBA_PxIS_TFES  (1U << 30) // Task File Error Status
#define HBA_PxIS_CPDS  (1U << 31) // Cold Port Detect Status

#define HBA_PxIS_ERRORS (HBA_PxIS_TFES | HBA_PxIS_HBFS | HBA_PxIS_HBDS | \
                         HBA_PxIS_IFS | HBA_PxIS_INFS | HBA_PxIS_OFS)

struct hba_port {
  uint32_t clb;       // 0x00: Command list base address, 1K-byte aligned
  uint32_t clbu;      // 0x04: Command list base address upper 32 bits
  uint32_t fb;        // 0x08: FIS base address, 256-byte aligned
  uint32_t fbu;       // 0x0C: FIS base address upper 32 bits
  uint32_t is;        // 0x10: Interrupt status
  uint32_t ie;        // 0x14: Interrupt enable
  uint32_t cmd;       // 0x18: Command and status
  uint32_t rsv0;      // 0x1C: Reserved
  uint32_t tfd;       // 0x20: Task file data
  uint32_t sig;       // 0x24: Signature
  uint32_t ssts;      // 0x28: SATA status (SCR0:SStatus)
  uint32_t sctl;      // 0x2C: SATA control (SCR2:SControl)
  uint32_t serr;      // 0x30: SATA error (SCR1:SError)
  uint32_t sact;      // 0x34: SATA active (SCR3:SActive)
  uint32_t ci;        // 0x38: Command issue
  uint32_t sntf;      // 0x3C: SATA notification (SCR4:SNotification)
  uint32_t fbs;       // 0x40: FIS-based switch control
  uint32_t rsv1[11];  // 0x44 - 0x6F: Reserved
  uint32_t vendor[4]; // 0x70 - 0x7F: Vendor specific
};

struct hba_mem {
  uint32_t cap;       // 0x00: Host capability
  uint32_t ghc;       // 0x04: Global host control
  uint32_t is;        // 0x08: Interrupt status
  uint32_t pi;        // 0x0C: Port implemented
  uint32_t vs;        // 0x10: Version
  uint32_t ccc_ctl;   // 0x14: Command completion coalescing control
  uint32_t ccc_pts;   // 0x18: Command completion coalescing ports
  uint32_t em_loc;    // 0x1C: Enclosure management location
  uint32_t em_ctl;    // 0x20: Enclosure management control
  uint32_t cap2;      // 0x24: Host capabilities extended
  uint32_t bohc;      // 0x28: BIOS/OS handoff control and status
  uint8_t  rsv[0xA0 - 0x2C]; // 0x2C - 0x9F: Reserved
  uint8_t  vendor[0x100 - 0xA0]; // 0xA0 - 0xFF: Vendor specific
  struct hba_port ports[32];     // 0x100 - 0x10FF: Port control registers
};

struct hba_prdt_entry {
  uint32_t dba;      // Data base address
  uint32_t dbau;     // Data base address upper 32 bits
  uint32_t rsv0;     // Reserved
  uint32_t dbc:22;   // Byte count, 4M max, 0-based
  uint32_t rsv1:9;   // Reserved
  uint32_t i:1;      // Interrupt on completion
};

struct hba_cmd_header {
  uint8_t  cfl:5;    // Command FIS length in DWORDS (2 .. 16)
  uint8_t  a:1;      // ATAPI
  uint8_t  w:1;      // Write: 1 = H2D, 0 = D2H
  uint8_t  p:1;      // Prefetchable
  uint8_t  r:1;      // Reset
  uint8_t  b:1;      // BIST
  uint8_t  c:1;      // Clear busy upon R_OK
  uint8_t  rsv0:1;   // Reserved
  uint8_t  pmp:4;    // Port multiplier port
  uint16_t prdtl;    // Physical region descriptor table length in entries
  volatile uint32_t prdbc; // Physical region descriptor byte count transferred
  uint32_t ctba;     // Command table descriptor base address
  uint32_t ctbau;    // Command table descriptor base address upper 32 bits
  uint32_t rsv1[4];  // Reserved
};

struct fis_reg_h2d {
  uint8_t  fis_type; // FIS_TYPE_REG_H2D (0x27)
  uint8_t  pmport:4; // Port multiplier
  uint8_t  rsv0:3;   // Reserved
  uint8_t  c:1;      // 1: Command, 0: Control
  uint8_t  command;  // Command register
  uint8_t  featurel; // Feature register (7:0)
  uint8_t  lba0;     // LBA low (7:0)
  uint8_t  lba1;     // LBA mid (15:8)
  uint8_t  lba2;     // LBA high (23:16)
  uint8_t  device;   // Device register (1<<6 for LBA)
  uint8_t  lba3;     // LBA (31:24)
  uint8_t  lba4;     // LBA (39:32)
  uint8_t  lba5;     // LBA (47:40)
  uint8_t  featureh; // Feature register (15:8)
  uint8_t  countl;   // Count (7:0)
  uint8_t  counth;   // Count (15:8)
  uint8_t  icc;      // Isochronous command completion
  uint8_t  control;  // Control register
  uint8_t  rsv1[4];  // Reserved
};

struct hba_cmd_tbl {
  uint8_t  cfis[64]; // Command FIS
  uint8_t  acmd[16]; // ATAPI command, 12 or 16 bytes
  uint8_t  rsv[48];  // Reserved (128 bytes total header)
  struct hba_prdt_entry prdt[AHCI_PRDT_PER_CMD]; // 8 * 16 = 128 bytes (256B total, 128B aligned)
};

void ahci_driver_init(void);
void ahci_init(void);
uint64_t ahci_size(uint port);
int ahci_read(uint port, uint64_t lba, uint count, void *dst);
int ahci_write(uint port, uint64_t lba, uint count, const void *src);
void ahcirw(struct buf *b);
void ahci_intr(struct device *dev);
void ahci_poll(struct device *dev);

#endif /* PRONINX_STORAGE_AHCI_H */
