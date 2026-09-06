#ifndef PRONINX_STORAGE_NVME_H
#define PRONINX_STORAGE_NVME_H

#include "inc/types.h"
#include "buf.h"
#include "driver.h"

#define NVME_MAX_NAMESPACES 8
#define NVME_SECTOR_SIZE 512
#define NVME_ADMIN_Q_SIZE 64
#define NVME_IO_Q_SIZE 64

// NVMe Admin Opcodes
#define NVME_ADMIN_DELETE_IO_SQ 0x00
#define NVME_ADMIN_CREATE_IO_SQ 0x01
#define NVME_ADMIN_DELETE_IO_CQ 0x04
#define NVME_ADMIN_CREATE_IO_CQ 0x05
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_SET_FEATURES 0x09

// NVMe NVM I/O Opcodes
#define NVME_NVM_FLUSH 0x00
#define NVME_NVM_WRITE 0x01
#define NVME_NVM_READ  0x02

// NVMe Registers (BAR0)
#define NVME_REG_CAP    0x00 // Controller Capabilities (64-bit)
#define NVME_REG_VS     0x08 // Version (32-bit)
#define NVME_REG_INTMS  0x0C // Interrupt Mask Set (32-bit)
#define NVME_REG_INTMC  0x10 // Interrupt Mask Clear (32-bit)
#define NVME_REG_CC     0x14 // Controller Configuration (32-bit)
#define NVME_REG_CSTS   0x1C // Controller Status (32-bit)
#define NVME_REG_NSSR   0x20 // NVM Subsystem Reset (32-bit)
#define NVME_REG_AQA    0x24 // Admin Queue Attributes (32-bit)
#define NVME_REG_ASQ    0x28 // Admin SQ Base Address (64-bit)
#define NVME_REG_ACQ    0x30 // Admin CQ Base Address (64-bit)
#define NVME_REG_DBS    0x1000 // Doorbell registers start

// CC register bits
#define NVME_CC_EN      (1U << 0)
#define NVME_CC_CSS_NVM (0U << 4)
#define NVME_CC_MPS_4K  (0U << 7)
#define NVME_CC_IOCQES  (4U << 20) // 16 bytes (2^4)
#define NVME_CC_IOSQES  (6U << 16) // 64 bytes (2^6)

// CSTS register bits
#define NVME_CSTS_RDY   (1U << 0)
#define NVME_CSTS_CFS   (1U << 1)

// 64-byte Submission Queue Entry
struct nvme_sqe {
  uint8_t  opcode;
  uint8_t  flags;
  uint16_t cid;
  uint32_t nsid;
  uint64_t rsv0;
  uint64_t mptr;
  uint64_t prp1;
  uint64_t prp2;
  uint32_t cdw10;
  uint32_t cdw11;
  uint32_t cdw12;
  uint32_t cdw13;
  uint32_t cdw14;
  uint32_t cdw15;
};

// 16-byte Completion Queue Entry
struct nvme_cqe {
  uint32_t result;
  uint32_t rsv0;
  uint16_t sq_head;
  uint16_t sq_id;
  uint16_t cid;
  uint16_t status;
};

void nvme_driver_init(void);
void nvme_init(void);
uint64_t nvme_size(uint ns_id);
int nvme_read(uint ns_id, uint64_t lba, uint count, void *dst);
int nvme_write(uint ns_id, uint64_t lba, uint count, const void *src);
void nvmerw(struct buf *b);

#endif /* PRONINX_STORAGE_NVME_H */
