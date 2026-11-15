#ifndef PRONINX_USB_EHCIREG_H
#define PRONINX_USB_EHCIREG_H

// EHCI 1

#include "inc/types.h"

#define PCI_EHCI_USBLEGSUP 0x00
#define EHCI_LEG_HC_OS_OWNED 0x01000000
#define EHCI_LEG_HC_BIOS_OWNED 0x00010000
#define PCI_EHCI_USBLEGCTLSTS 0x04
#define EHCI_CAP_ID_LEGACY 1

#define EHCI_CAPLENGTH 0x00
#define EHCI_HCIVERSION 0x02
#define EHCI_HCSPARAMS 0x04
#define EHCI_HCS_N_PORTS(x) ((x) & 0xf)
#define EHCI_HCS_PPC(x) ((x) & 0x10)
#define EHCI_HCS_N_PCC(x) (((x) >> 8) & 0xf)
#define EHCI_HCS_N_CC(x) (((x) >> 12) & 0xf)
#define EHCI_HCCPARAMS 0x08
#define EHCI_HCC_64BIT(x) ((x) & 0x1)
#define EHCI_HCC_PFLF(x) ((x) & 0x2)
#define EHCI_HCC_EECP(x) (((x) >> 8) & 0xff)

#define EHCI_USBCMD 0x00
#define EHCI_CMD_ITC_M 0x00ff0000
#define EHCI_CMD_ITC_1 0x00010000
#define EHCI_CMD_ITC_2 0x00020000
#define EHCI_CMD_IAAD 0x00000040
#define EHCI_CMD_ASE 0x00000020
#define EHCI_CMD_PSE 0x00000010
#define EHCI_CMD_FLS_M 0x0000000c
#define EHCI_CMD_FLS(x) (((x) >> 2) & 3)
#define EHCI_CMD_HCRESET 0x00000002
#define EHCI_CMD_RS 0x00000001
#define EHCI_USBSTS 0x04
#define EHCI_STS_ASS 0x00008000
#define EHCI_STS_PSS 0x00004000
#define EHCI_STS_HCH 0x00001000
#define EHCI_STS_IAA 0x00000020
#define EHCI_STS_HSE 0x00000010
#define EHCI_STS_FLR 0x00000008
#define EHCI_STS_PCD 0x00000004
#define EHCI_STS_ERRINT 0x00000002
#define EHCI_STS_INT 0x00000001
#define EHCI_STS_INTRS(x) ((x) & 0x3f)
#define EHCI_NORMAL_INTRS \
  (EHCI_STS_IAA | EHCI_STS_HSE | EHCI_STS_PCD | EHCI_STS_ERRINT | EHCI_STS_INT)
#define EHCI_USBINTR 0x08
#define EHCI_FRINDEX 0x0c
#define EHCI_CTRLDSSEGMENT 0x10
#define EHCI_PERIODICLISTBASE 0x14
#define EHCI_ASYNCLISTADDR 0x18
#define EHCI_CONFIGFLAG 0x40
#define EHCI_CONF_CF 0x00000001
#define EHCI_PORTSC(n) (0x44 + 4 * (n))
#define EHCI_PS_PSPD 0x0C000000
#define EHCI_PS_PSPD_FS 0x00000000
#define EHCI_PS_PSPD_LS 0x04000000
#define EHCI_PS_PSPD_HS 0x08000000
#define EHCI_PS_PO 0x00002000
#define EHCI_PS_PP 0x00001000
#define EHCI_PS_LS 0x00000c00
#define EHCI_PS_IS_LOWSPEED(x) (((x) & EHCI_PS_LS) == 0x00000400)
#define EHCI_PS_PR 0x00000100
#define EHCI_PS_SUSP 0x00000080
#define EHCI_PS_FPR 0x00000040
#define EHCI_PS_OCC 0x00000020
#define EHCI_PS_OCA 0x00000010
#define EHCI_PS_PEC 0x00000008
#define EHCI_PS_PE 0x00000004
#define EHCI_PS_CSC 0x00000002
#define EHCI_PS_CS 0x00000001
#define EHCI_PS_CLEAR (EHCI_PS_OCC | EHCI_PS_PEC | EHCI_PS_CSC)

#define EHCI_LINK_TERMINATE 0x1
#define EHCI_LINK_ITD 0x0
#define EHCI_LINK_QH 0x2
#define EHCI_LINK_SITD 0x4
#define EHCI_LINK_ADDR(x) ((x) & ~0x1fu)

#define EHCI_QTD_NBUFFERS 5
struct ehci_qtd {
  volatile uint32_t next;
  volatile uint32_t altnext;
  volatile uint32_t status;
  volatile uint32_t buffer[EHCI_QTD_NBUFFERS];
  volatile uint32_t buffer_hi[EHCI_QTD_NBUFFERS];
  uint32_t pad[3];
};
#define EHCI_QTD_ACTIVE 0x80
#define EHCI_QTD_HALTED 0x40
#define EHCI_QTD_BUFERR 0x20
#define EHCI_QTD_BABBLE 0x10
#define EHCI_QTD_XACTERR 0x08
#define EHCI_QTD_MISSEDMICRO 0x04
#define EHCI_QTD_STATERRS 0x3c
#define EHCI_QTD_PID_OUT 0x0
#define EHCI_QTD_PID_IN 0x1
#define EHCI_QTD_PID_SETUP 0x2
#define EHCI_QTD_SET_PID(x) ((uint32_t)(x) << 8)
#define EHCI_QTD_SET_CERR(x) ((uint32_t)(x) << 10)
#define EHCI_QTD_IOC 0x8000
#define EHCI_QTD_GET_BYTES(x) (((x) >> 16) & 0x7fff)
#define EHCI_QTD_SET_BYTES(x) ((uint32_t)(x) << 16)
#define EHCI_QTD_TOGGLE 0x80000000u

struct ehci_qh {
  volatile uint32_t link;
  volatile uint32_t endp;
  volatile uint32_t endphub;
  volatile uint32_t curqtd;
  struct ehci_qtd ovl;
} __aligned(32);
#define EHCI_QH_SET_ADDR(x) ((uint32_t)(x) & 0x7f)
#define EHCI_QH_SET_ENDPT(x) (((uint32_t)(x) & 0xf) << 8)
#define EHCI_QH_SET_EPS(x) (((uint32_t)(x) & 3) << 12)
#define EHCI_QH_SPEED_FULL 0x0
#define EHCI_QH_SPEED_LOW 0x1
#define EHCI_QH_SPEED_HIGH 0x2
#define EHCI_QH_DTC 0x4000
#define EHCI_QH_HRECL 0x8000
#define EHCI_QH_SET_MPL(x) (((uint32_t)(x) & 0x7ff) << 16)
#define EHCI_QH_MPL_MASK (0x7ffu << 16)
#define EHCI_QH_CTL 0x08000000
#define EHCI_QH_SET_NRL(x) (((uint32_t)(x) & 0xf) << 28)
#define EHCI_QH_SET_SMASK(x) ((uint32_t)(x) & 0xff)
#define EHCI_QH_SET_CMASK(x) (((uint32_t)(x) & 0xff) << 8)
#define EHCI_QH_SET_HUBA(x) (((uint32_t)(x) & 0x7f) << 16)
#define EHCI_QH_SET_PORT(x) (((uint32_t)(x) & 0x7f) << 23)
#define EHCI_QH_SET_MULT(x) (((uint32_t)(x) & 3) << 30)

#define EHCI_ITD_NUFRAMES 8
#define EHCI_ITD_NBUFFERS 7
struct ehci_itd {
  volatile uint32_t next;
  volatile uint32_t ctl[EHCI_ITD_NUFRAMES];
  volatile uint32_t bufr[EHCI_ITD_NBUFFERS];
  volatile uint32_t bufr_hi[EHCI_ITD_NBUFFERS];
} __aligned(32);
#define EHCI_ITD_ACTIVE 0x80000000u
#define EHCI_ITD_STATUS(x) (((x) >> 28) & 0xf)
#define EHCI_ITD_GET_LEN(x) (((x) >> 16) & 0xfff)
#define EHCI_ITD_SET_LEN(x) (((uint32_t)(x) & 0xfff) << 16)
#define EHCI_ITD_IOC 0x8000
#define EHCI_ITD_SET_PG(x) (((uint32_t)(x) & 7) << 12)
#define EHCI_ITD_SET_OFFS(x) ((uint32_t)(x) & 0xfff)
#define EHCI_ITD_SET_EP(x) (((uint32_t)(x) & 0xf) << 8)
#define EHCI_ITD_SET_DADDR(x) ((uint32_t)(x) & 0x7f)
#define EHCI_ITD_DIR_IN 0x800
#define EHCI_ITD_SET_MAXPKT(x) ((uint32_t)(x) & 0x7ff)
#define EHCI_ITD_SET_MULTI(x) ((uint32_t)(x) & 3)

struct ehci_sitd {
  volatile uint32_t next;
  volatile uint32_t endp;
  volatile uint32_t sched;
  volatile uint32_t trans;
  volatile uint32_t buffer[2];
  volatile uint32_t back;
  volatile uint32_t buffer_hi[2];
} __aligned(32);
#define EHCI_SITD_DIR_IN 0x80000000u
#define EHCI_SITD_SET_PORT(x) (((uint32_t)(x) & 0x7f) << 24)
#define EHCI_SITD_SET_HUBA(x) (((uint32_t)(x) & 0x7f) << 16)
#define EHCI_SITD_SET_ENDPT(x) (((uint32_t)(x) & 0xf) << 8)
#define EHCI_SITD_SET_DADDR(x) ((uint32_t)(x) & 0x7f)
#define EHCI_SITD_SET_SMASK(x) ((uint32_t)(x) & 0xff)
#define EHCI_SITD_SET_CMASK(x) (((uint32_t)(x) & 0xff) << 8)
#define EHCI_SITD_IOC 0x80000000u
#define EHCI_SITD_SET_LEN(x) (((uint32_t)(x) & 0x3ff) << 16)
#define EHCI_SITD_GET_LEN(x) (((x) >> 16) & 0x3ff)
#define EHCI_SITD_ACTIVE 0x80
#define EHCI_SITD_ERRS 0x7c

#endif
