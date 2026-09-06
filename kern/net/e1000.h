#ifndef PRONINX_NET_E1000_H
#define PRONINX_NET_E1000_H

#include "inc/types.h"
#include "driver.h"

// Intel E1000 Registers
#define E1000_CTRL     0x00000  // Device Control
#define E1000_STATUS   0x00008  // Device Status
#define E1000_EECD     0x00010  // EEPROM/Flash Control
#define E1000_EERD     0x00014  // EEPROM Read
#define E1000_CTRL_EXT 0x00018  // Extended Device Control
#define E1000_ICR      0x000C0  // Interrupt Cause Read
#define E1000_ITR      0x000C4  // Interrupt Throttling Rate
#define E1000_ICS      0x000C8  // Interrupt Cause Set
#define E1000_IMS      0x000D0  // Interrupt Mask Set
#define E1000_IMC      0x000D8  // Interrupt Mask Clear
#define E1000_RCTL     0x00100  // Receive Control
#define E1000_TCTL     0x00400  // Transmit Control
#define E1000_TIPG     0x00410  // Transmit Inter Packet Gap
#define E1000_RDBAL    0x02800  // RX Descriptor Base Address Low
#define E1000_RDBAH    0x02804  // RX Descriptor Base Address High
#define E1000_RDLEN    0x02808  // RX Descriptor Length
#define E1000_RDH      0x02810  // RX Descriptor Head
#define E1000_RDT      0x02818  // RX Descriptor Tail
#define E1000_RDTR     0x02820  // RX Delay Timer
#define E1000_TDBAL    0x03800  // TX Descriptor Base Address Low
#define E1000_TDBAH    0x03804  // TX Descriptor Base Address High
#define E1000_TDLEN    0x03808  // TX Descriptor Length
#define E1000_TDH      0x03810  // TX Descriptor Head
#define E1000_TDT      0x03818  // TX Descriptor Tail
#define E1000_TIDV     0x03820  // TX Interrupt Delay Value
#define E1000_MTA      0x05200  // Multicast Table Array (128 entries)
#define E1000_RAL      0x05400  // Receive Address Low (MAC)
#define E1000_RAH      0x05404  // Receive Address High (MAC)

// E1000 Control Register Flags
#define E1000_CTRL_FD       0x00000001  // Full Duplex
#define E1000_CTRL_ASDE     0x00000020  // Auto-Speed Detection Enable
#define E1000_CTRL_SLU      0x00000040  // Set Link Up
#define E1000_CTRL_RST      0x04000000  // Device Reset

// E1000 Receive Control Register Flags
#define E1000_RCTL_EN         0x00000002  // Receiver Enable
#define E1000_RCTL_SBP        0x00000004  // Store Bad Packets
#define E1000_RCTL_UPE        0x00000008  // Unicast Promiscuous Enable
#define E1000_RCTL_MPE        0x00000010  // Multicast Promiscuous Enable
#define E1000_RCTL_LPE        0x00000020  // Long Packet Enable
#define E1000_RCTL_LBM_NONE   0x00000000  // No Loopback
#define E1000_RCTL_BAM        0x00008000  // Broadcast Accept Mode
#define E1000_RCTL_BSIZE_2048 0x00000000  // Buffer Size 2048 bytes
#define E1000_RCTL_SECRC      0x04000000  // Strip Ethernet CRC

// E1000 Transmit Control Register Flags
#define E1000_TCTL_EN   0x00000002  // Transmit Enable
#define E1000_TCTL_PSP  0x00000008  // Pad Short Packets
#define E1000_TCTL_CT   0x000000f0  // Collision Threshold (15)
#define E1000_TCTL_COLD 0x00040000  // Collision Distance (64 for FD)

// E1000 Transmit Descriptor Command Flags
#define E1000_TXD_CMD_EOP  0x01  // End of Packet
#define E1000_TXD_CMD_IFCS 0x02  // Insert FCS (CRC)
#define E1000_TXD_CMD_IC   0x04  // Insert Checksum
#define E1000_TXD_CMD_RS   0x08  // Report Status
#define E1000_TXD_CMD_RPS  0x10  // Report Packet Sent
#define E1000_TXD_CMD_DEXT 0x20  // Descriptor Extension
#define E1000_TXD_CMD_VLE  0x40  // VLAN Packet Enable
#define E1000_TXD_CMD_IDE  0x80  // Interrupt Delay Enable

// E1000 Transmit Descriptor Status Flags
#define E1000_TXD_STAT_DD 0x01  // Descriptor Done

// E1000 Receive Descriptor Status Flags
#define E1000_RXD_STAT_DD  0x01  // Descriptor Done
#define E1000_RXD_STAT_EOP 0x02  // End of Packet
#define E1000_RXD_STAT_IXSM 0x04 // Ignore Checksum
#define E1000_RXD_STAT_VP   0x08 // Packet is 802.1Q (VLAN)

// E1000 Interrupt Cause / Mask Flags
#define E1000_ICR_TXDW    0x00000001  // TX Descriptor Written Back
#define E1000_ICR_TXQE    0x00000002  // TX Queue Empty
#define E1000_ICR_LSC     0x00000004  // Link Status Change
#define E1000_ICR_RXSEQ   0x00000008  // RX Sequence Error
#define E1000_ICR_RXDMT0  0x00000010  // RX Descriptor Minimum Threshold Reached
#define E1000_ICR_RXO     0x00000040  // Receiver Overrun
#define E1000_ICR_RXT0    0x00000080  // Receiver Timer Interrupt

#define E1000_RING_ALIGN 16
#define E1000_NUM_TX_DESC 64
#define E1000_NUM_RX_DESC 64
#define E1000_RX_BUF_SIZE 2048
#define E1000_TX_BUF_SIZE 2048

struct e1000_tx_desc {
  uint64_t addr;
  uint16_t length;
  uint8_t  cso;
  uint8_t  cmd;
  uint8_t  status;
  uint8_t  css;
  uint16_t special;
} __attribute__((packed));

struct e1000_rx_desc {
  uint64_t addr;
  uint16_t length;
  uint16_t checksum;
  uint8_t  status;
  uint8_t  errors;
  uint16_t special;
} __attribute__((packed));

void e1000_driver_init(void);

#endif /* PRONINX_NET_E1000_H */
