#ifndef PRONINX_USB_XHCIREG_H
#define PRONINX_USB_XHCIREG_H

#include "inc/types.h"

// capability registers
#define XHCI_CAPLENGTH 0x00
#define XHCI_HCIVERSION 0x02
#define XHCI_HCSPARAMS1 0x04
#define XHCI_HCS1_DEVSLOT_MAX(x) ((x) & 0xff)
#define XHCI_HCS1_N_PORTS(x) (((x) >> 24) & 0xff)
#define XHCI_HCSPARAMS2 0x08
#define XHCI_HCS2_SPB_MAX(x) ((((x) >> 16) & 0x3e0) | (((x) >> 27) & 0x1f))
#define XHCI_HCCPARAMS 0x10
#define XHCI_HCC_AC64(x) ((x) & 1)
#define XHCI_HCC_CSZ(x) (((x) >> 2) & 1)
#define XHCI_HCC_XECP(x) (((x) >> 16) & 0xffff)
#define XHCI_DBOFF 0x14
#define XHCI_RTSOFF 0x18

// operational registers, at CAPLENGTH
#define XHCI_USBCMD 0x00
#define XHCI_CMD_RS 0x00000001
#define XHCI_CMD_HCRST 0x00000002
#define XHCI_CMD_INTE 0x00000004
#define XHCI_CMD_HSEE 0x00000008
#define XHCI_CMD_CSS 0x00000100 // save state
#define XHCI_CMD_CRS 0x00000200 // restore state
#define XHCI_USBSTS 0x04
#define XHCI_DNCTRL 0x14
#define XHCI_STS_HCH 0x00000001
#define XHCI_STS_HSE 0x00000004
#define XHCI_STS_EINT 0x00000008
#define XHCI_STS_PCD 0x00000010
#define XHCI_STS_SSS 0x00000100
#define XHCI_STS_RSS 0x00000200
#define XHCI_STS_SRE 0x00000400
#define XHCI_STS_CNR 0x00000800
#define XHCI_STS_HCE 0x00001000
#define XHCI_PAGESIZE 0x08
#define XHCI_CRCR_LO 0x18
#define XHCI_CRCR_HI 0x1c
#define XHCI_CRCR_LO_RCS 0x00000001
#define XHCI_DCBAAP_LO 0x30
#define XHCI_DCBAAP_HI 0x34
#define XHCI_CONFIG 0x38

#define XHCI_PORTSC(n) (0x3f0 + (0x10 * (n)))
#define XHCI_PORTPMSC(n) (0x3f4 + (0x10 * (n)))
#define XHCI_PM3_U1TO_SET(x) (((x) & 0xff) << 0)
#define XHCI_PM3_U2TO_SET(x) (((x) & 0xff) << 8)
#define XHCI_PS_CCS 0x00000001
#define XHCI_PS_PLS_GET(x) (((x) >> 5) & 0xf)
#define XHCI_PS_PLS_SET(x) (((x) & 0xf) << 5)
#define XHCI_PS_PLS_U0 0
#define XHCI_PS_PLS_U3 3
#define XHCI_PS_PLS_DISABLED 4
#define XHCI_PS_PLS_RXDETECT 5
#define XHCI_PS_PLS_INACTIVE 6
#define XHCI_PS_PLS_POLLING 7
#define XHCI_PS_PLS_COMPLIANCE 0xa
#define XHCI_PS_PLS_LOOPBACK 0xb
#define XHCI_PS_PLS_RESUME 0xf
#define XHCI_PS_LWS 0x00010000
#define XHCI_PS_CAS 0x01000000
#define XHCI_PS_WPR 0x80000000u
#define XHCI_PS_PED 0x00000002
#define XHCI_PS_PR 0x00000010
#define XHCI_PS_PP 0x00000200
#define XHCI_PS_SPEED(x) (((x) >> 10) & 0xf)
#define XHCI_PS_CSC 0x00020000
#define XHCI_PS_PEC 0x00040000
#define XHCI_PS_WRC 0x00080000
#define XHCI_PS_OCC 0x00100000
#define XHCI_PS_PRC 0x00200000
#define XHCI_PS_PLC 0x00400000
#define XHCI_PS_CEC 0x00800000
#define XHCI_PS_CLEAR 0x80ff01ffu
#define XHCI_PS_WCE 0x02000000
#define XHCI_PS_WDE 0x04000000
#define XHCI_PS_WOE 0x08000000
#define XHCI_PS_WAKE (XHCI_PS_WCE | XHCI_PS_WDE | XHCI_PS_WOE)

#define XHCI_SPEED_FULL 1
#define XHCI_SPEED_LOW 2
#define XHCI_SPEED_HIGH 3
#define XHCI_SPEED_SUPER 4

// runtime registers, at RTSOFF
#define XHCI_IMAN(n) (0x0020 + (0x20 * (n)))
#define XHCI_IMAN_INTR_PEND 0x00000001
#define XHCI_IMAN_INTR_ENA 0x00000002
#define XHCI_IMOD(n) (0x0024 + (0x20 * (n)))
#define XHCI_IMOD_DEFAULT 0x000001f4u
#define XHCI_ERSTSZ(n) (0x0028 + (0x20 * (n)))
#define XHCI_ERSTBA_LO(n) (0x0030 + (0x20 * (n)))
#define XHCI_ERSTBA_HI(n) (0x0034 + (0x20 * (n)))
#define XHCI_ERDP_LO(n) (0x0038 + (0x20 * (n)))
#define XHCI_ERDP_LO_BUSY 0x00000008
#define XHCI_ERDP_HI(n) (0x003c + (0x20 * (n)))

// doorbells, at DBOFF
#define XHCI_DOORBELL(n) (4 * (n))

#define XHCI_XECP_ID(x) ((x) & 0xff)
#define XHCI_XECP_NEXT(x) (((x) >> 8) & 0xff)
#define XHCI_ID_USB_LEGACY 0x0001
#define XHCI_XECP_BIOS_SEM 0x0002
#define XHCI_XECP_OS_SEM 0x0003
#define XHCI_XECP_LEGCTLSTS 0x0004

#define XHCI_ID_PROTOCOLS 2
#define XHCI_XECP_USBID 0x20425355
#define XHCI_XECP_SP_MAJOR(x) (((x) >> 24) & 0xff)
#define XHCI_XECP_SP_CPO(x) ((x) & 0xff)
#define XHCI_XECP_SP_CPC(x) (((x) >> 8) & 0xff)

#define PCI_XHCI_INTEL_XUSB2PR 0xd0
#define PCI_XHCI_INTEL_XUSB2PRM 0xd4
#define PCI_XHCI_INTEL_USB3_PSSEN 0xd8
#define PCI_XHCI_INTEL_USB3PRM 0xdc

struct xhci_trb {
  uint64_t params;
  uint32_t status;
  uint32_t control;
} __packed;

#define XHCI_TRB_SIZE 16
#define XHCI_RING_TRBS 256

#define XHCI_TRB_CYCLE 0x00000001
#define XHCI_TRB_TC 0x00000002
#define XHCI_TRB_SP 0x00800000 // stop endpoint: for a suspend
#define XHCI_TRB_ENT 0x00000002
#define XHCI_TRB_ISP 0x00000004
#define XHCI_TRB_CHAIN 0x00000010
#define XHCI_TRB_IOC 0x00000020
#define XHCI_TRB_IDT 0x00000040
#define XHCI_TRB_TYPE(x) (((x) & 0x3f) << 10)
#define XHCI_TRB_GET_TYPE(x) (((x) >> 10) & 0x3f)
#define XHCI_TRB_DIR_IN 0x00010000
#define XHCI_TRB_TRT_IN 0x00030000
#define XHCI_TRB_TRT_OUT 0x00020000
#define XHCI_TRB_SLOT(x) ((uint32_t)(x) << 24)
#define XHCI_TRB_GET_SLOT(x) (((x) >> 24) & 0xff)
#define XHCI_TRB_EP(x) ((uint32_t)(x) << 16)
#define XHCI_TRB_GET_EP(x) (((x) >> 16) & 0x1f)

#define XHCI_TRB_LEN(x) ((x) & 0x1ffff)
#define XHCI_TRB_MAXSIZE (64 * 1024)
#define XHCI_TRB_TDREM(x) (((x) & 0x1f) << 17)
#define XHCI_TRB_GET_LEN(x) ((x) & 0x1ffff)
#define XHCI_TRB_TDSIZE(x) (((x) & 0x1f) << 17)
#define XHCI_TRB_INTR(x) (((x) & 0x3ff) << 22)
#define XHCI_TRB_GET_CODE(x) (((x) >> 24) & 0xff)

// trb types
#define XHCI_TRB_ISOCH_SIA 0x80000000
#define XHCI_TRB_ISOC_TBC(x) (((x) & 3) << 7)
#define XHCI_TRB_ISOC_TLBPC(x) (((x) & 0xf) << 16)

#define XHCI_TRB_NORMAL 1
#define XHCI_TRB_SETUP 2
#define XHCI_TRB_DATA 3
#define XHCI_TRB_STATUS 4
#define XHCI_TRB_LINK 6
#define XHCI_TRB_ISOCH 5
#define XHCI_CMD_ENABLE_SLOT 9
#define XHCI_CMD_DISABLE_SLOT 10
#define XHCI_CMD_ADDRESS_DEVICE 11
#define XHCI_CMD_CONFIGURE_EP 12
#define XHCI_CMD_EVALUATE_CTX 13
#define XHCI_CMD_RESET_EP 14
#define XHCI_CMD_STOP_EP 15
#define XHCI_CMD_SET_TR_DEQUEUE 16
#define XHCI_CMD_RESET_DEVICE 17
#define XHCI_CMD_NOOP 23
#define XHCI_EVT_TRANSFER 32
#define XHCI_EVT_CMD_COMPLETE 33
#define XHCI_EVT_PORT_CHANGE 34

// endpoint context state, 6.2.3
#define XHCI_EPSTATE_DISABLED 0
#define XHCI_EPSTATE_RUNNING 1
#define XHCI_EPSTATE_HALTED 2
#define XHCI_EPSTATE_STOPPED 3
#define XHCI_EPSTATE_ERROR 4

// completion codes
#define XHCI_CODE_SUCCESS 1
#define XHCI_CODE_SHORT_PKT 13
#define XHCI_CODE_STALL 6
#define XHCI_CODE_RING_OVERRUN 21
#define XHCI_CODE_RING_UNDERRUN 22
#define XHCI_CODE_MISSED_SERVICE 23
#define XHCI_CODE_NO_SLOTS 9
#define XHCI_CODE_SLOT_NOT_ENABLED 11
#define XHCI_CODE_CONTEXT_STATE 19
#define XHCI_CODE_STOPPED 26
#define XHCI_CODE_STOPPED_LEN 27
#define XHCI_CODE_STOPPED_SHORT 28

struct xhci_erseg {
  uint64_t addr;
  uint32_t size;
  uint32_t rsvd;
} __packed;

#define XHCI_EP_ISOCH_OUT 1
#define XHCI_EP_CONTROL 4
#define XHCI_EP_ISOCH_IN 5
#define XHCI_EP_INTERRUPT_IN 7

#endif
