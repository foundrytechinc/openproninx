#ifndef PRONINX_E1000_OSDEP_H
#define PRONINX_E1000_OSDEP_H
#include "defs.h"
#include "inc/stdbool.h"
#include "pci.h"
#include "x86.h"

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int64_t s64;
typedef int32_t s32;
typedef int16_t s16;
typedef int8_t s8;
#define __le16 u16
#define __le32 u32
#define __le64 u64

#ifndef ETHER_ADDR_LEN
#define ETHER_ADDR_LEN 6
#endif

#define usec_delay(x) delay(x)
#define usec_delay_irq(x) delay(x)
#define msec_delay(x) delay(1000 * (x))
#define msec_delay_irq(x) delay(1000 * (x))

extern int e1000_debug;
#define DEBUGOUT(...) do { if (e1000_debug) printf(__VA_ARGS__); } while (0)
#define DEBUGOUT1(...) DEBUGOUT(__VA_ARGS__)
#define DEBUGOUT2(...) DEBUGOUT(__VA_ARGS__)
#define DEBUGOUT3(...) DEBUGOUT(__VA_ARGS__)
#define DEBUGOUT7(...) DEBUGOUT(__VA_ARGS__)
#define DEBUGFUNC(F) DEBUGOUT(F "\n")
#define STATIC static
#define ASSERT_CTX_LOCK_HELD(hw)
#define ASSERT_NO_LOCKS()

#define CMD_MEM_WRT_INVALIDATE 0x0010
#define PCI_COMMAND_REGISTER PCI_REG_COMMAND

struct e1000_osdep {
  volatile uchar *mem;
  volatile uchar *flash;
  ushort io;
  struct pci_device *pci;
};

static inline u32 e1000_rd4(volatile uchar *base, u32 off) {
  return *(volatile u32 *)(base + off);
}
static inline void e1000_wr4(volatile uchar *base, u32 off, u32 v) {
  *(volatile u32 *)(base + off) = v;
}

#define E1000_OSDEP(hw) ((struct e1000_osdep *)(hw)->back)
#define E1000_REGISTER(hw, reg) \
  (((hw)->mac.type >= e1000_82543) ? (reg) : e1000_translate_register_82542(reg))
#define E1000_WRITE_FLUSH(a) E1000_READ_REG(a, E1000_STATUS)
#define E1000_READ_OFFSET(hw, offset) e1000_rd4(E1000_OSDEP(hw)->mem, (offset))
#define E1000_WRITE_OFFSET(hw, offset, value) \
  e1000_wr4(E1000_OSDEP(hw)->mem, (offset), (value))
#define E1000_READ_REG(hw, reg) e1000_rd4(E1000_OSDEP(hw)->mem, E1000_REGISTER(hw, reg))
#define E1000_WRITE_REG(hw, reg, value) \
  e1000_wr4(E1000_OSDEP(hw)->mem, E1000_REGISTER(hw, reg), (value))
#define E1000_READ_REG_ARRAY(hw, reg, index) \
  e1000_rd4(E1000_OSDEP(hw)->mem, E1000_REGISTER(hw, reg) + ((index) << 2))
#define E1000_WRITE_REG_ARRAY(hw, reg, index, value) \
  e1000_wr4(E1000_OSDEP(hw)->mem, E1000_REGISTER(hw, reg) + ((index) << 2), (value))
#define E1000_READ_REG_ARRAY_DWORD E1000_READ_REG_ARRAY
#define E1000_WRITE_REG_ARRAY_DWORD E1000_WRITE_REG_ARRAY
#define E1000_READ_REG_ARRAY_BYTE(hw, reg, index) \
  (*(volatile u8 *)(E1000_OSDEP(hw)->mem + E1000_REGISTER(hw, reg) + (index)))
#define E1000_WRITE_REG_ARRAY_BYTE(hw, reg, index, value) \
  (*(volatile u8 *)(E1000_OSDEP(hw)->mem + E1000_REGISTER(hw, reg) + (index)) = (value))
#define E1000_WRITE_REG_ARRAY_WORD(hw, reg, index, value) \
  (*(volatile u16 *)(E1000_OSDEP(hw)->mem + E1000_REGISTER(hw, reg) + ((index) << 1)) = (value))
#define E1000_WRITE_REG_IO(hw, reg, value) \
  do { \
    outl(E1000_OSDEP(hw)->io, (reg)); \
    outl(E1000_OSDEP(hw)->io + 4, (value)); \
  } while (0)
#define E1000_READ_FLASH_REG(hw, reg) e1000_rd4(E1000_OSDEP(hw)->flash, (reg))
#define E1000_READ_FLASH_REG16(hw, reg) (*(volatile u16 *)(E1000_OSDEP(hw)->flash + (reg)))
#define E1000_WRITE_FLASH_REG(hw, reg, value) e1000_wr4(E1000_OSDEP(hw)->flash, (reg), (value))
#define E1000_WRITE_FLASH_REG16(hw, reg, value) \
  (*(volatile u16 *)(E1000_OSDEP(hw)->flash + (reg)) = (value))

#endif
