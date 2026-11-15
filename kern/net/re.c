// RE: Realtek 8139C+, 8169 and the 8168 family, after OpenBSD sys/dev/ic/re.c
/*
 * Copyright (c) 1997, 1998-2003
 *	Bill Paul <wpaul@windriver.com>.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Bill Paul.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR THE VOICES IN HIS HEAD
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "callout.h"
#include "if.h"
#include "malloc.h"
#include "defs.h"
#include "driver.h"
#include "memlayout.h"
#include "mmu.h"
#include "mutex.h"
#include "param.h"
#include "task.h"
#include "x86.h"

#define PCI_VENDOR_REALTEK 0x10ec
#define RT8139 0x8139
#define RT8101E 0x8136
#define RT8168 0x8168
#define RT8168_2 0x8161
#define RT8169 0x8169
#define RT8169SC 0x8167

#define RL_IDR0 0x0000
#define RL_IDR4 0x0004
#define RL_MAR0 0x0008
#define RL_MAR4 0x000c
#define RL_TXLIST_ADDR_LO 0x0020
#define RL_TXLIST_ADDR_HI 0x0024
#define RL_COMMAND 0x0037
#define RL_GTXSTART 0x0038
#define RL_IMR 0x003c
#define RL_ISR 0x003e
#define RL_TXCFG 0x0040
#define RL_RXCFG 0x0044
#define RL_TIMERCNT 0x0048
#define RL_MISSEDPKT 0x004c
#define RL_EECMD 0x0050
#define RL_CFG1 0x0052
#define RL_CFG2 0x0053
#define RL_CFG3 0x0054
#define RL_CFG4 0x0055
#define RL_TIMERINT 0x0054
#define RL_TIMERINT_8169 0x0058
#define RL_MEDIASTAT 0x0058
#define RL_PHYAR 0x0060
#define RL_BMCR 0x0062
#define RL_BMSR 0x0064
#define RL_ANAR 0x0066
#define RL_GMEDIASTAT 0x006c
#define RL_MACDBG 0x006d
#define RL_GPIO 0x006e
#define RL_PMCH 0x006f
#define RL_LDPS 0x0082
#define RL_TXSTART 0x00d9
#define RL_MAXRXPKTLEN 0x00da
#define RL_CPLUS_CMD 0x00e0
#define RL_RXLIST_ADDR_LO 0x00e4
#define RL_RXLIST_ADDR_HI 0x00e8
#define RL_EARLY_TX_THRESH 0x00ec
#define RL_MISC 0x00f0

#define RL_TXCFG_HWREV 0x7c800000
#define RL_TXCFG_QUEUE_EMPTY 0x00000800
#define RL_TXCFG_IFG 0x03000000
#define RL_TXDMA_2048BYTES 0x00000700
#define RL_TXCFG_CONFIG (RL_TXCFG_IFG | RL_TXDMA_2048BYTES)

#define RL_RXCFG_RX_ALLPHYS 0x00000001
#define RL_RXCFG_RX_INDIV 0x00000002
#define RL_RXCFG_RX_MULTI 0x00000004
#define RL_RXCFG_RX_BROAD 0x00000008
#define RL_RXCFG_EARLYOFFV2 0x00000800
#define RL_RXCFG_EARLYOFF 0x00003800
#define RL_RXDMA_UNLIMITED 0x00000700
#define RL_RXBUF_64 0x00001800
#define RL_RXFIFO_NOTHRESH 0x0000e000
#define RL_RXCFG_CONFIG (RL_RXFIFO_NOTHRESH | RL_RXDMA_UNLIMITED | RL_RXBUF_64)

#define RL_CMD_TX_ENB 0x0004
#define RL_CMD_RX_ENB 0x0008
#define RL_CMD_RESET 0x0010
#define RL_CMD_STOPREQ 0x0080

#define RL_ISR_RX_OK 0x0001
#define RL_ISR_RX_ERR 0x0002
#define RL_ISR_TX_OK 0x0004
#define RL_ISR_TX_ERR 0x0008
#define RL_ISR_RX_OVERRUN 0x0010
#define RL_ISR_LINKCHG 0x0020
#define RL_ISR_FIFO_OFLOW 0x0040
#define RL_ISR_SYSTEM_ERR 0x8000
#define RL_INTRS_CPLUS                                                         \
  (RL_ISR_RX_OK | RL_ISR_RX_ERR | RL_ISR_TX_ERR | RL_ISR_RX_OVERRUN |          \
   RL_ISR_FIFO_OFLOW | RL_ISR_SYSTEM_ERR | RL_ISR_TX_OK)

#define RL_EE_DATAOUT 0x01
#define RL_EE_DATAIN 0x02
#define RL_EE_CLK 0x04
#define RL_EE_SEL 0x08
#define RL_EE_MODE 0xc0
#define RL_EEMODE_OFF 0x00
#define RL_EEMODE_PROGRAM 0x80
#define RL_EEMODE_WRITECFG 0xc0
#define RL_9346_READ 0x6
#define RL_9346_ADDR_LEN 6
#define RL_9356_ADDR_LEN 8
#define RL_EE_EADDR 0x07

#define RL_CFG1_DRVLOAD 0x20
#define RL_CFG2_MSI 0x20
#define RL_CFG3_JUMBO_EN0 0x04
#define RL_CFG4_JUMBO_EN1 0x02
#define RL_CFG4_8168E_JUMBO_EN1 0x01

#define RL_TXSTART_START 0x40

#define RL_CPLUSCMD_TXENB 0x0001
#define RL_CPLUSCMD_RXENB 0x0002
#define RL_CPLUSCMD_PCI_MRW 0x0008
#define RL_CPLUSCMD_RXCSUM_ENB 0x0020
#define RL_CPLUSCMD_MACSTAT_DIS 0x0080

#define RL_PHYAR_PHYDATA 0x0000ffff
#define RL_PHYAR_BUSY 0x80000000

#define RL_MEDIASTAT_SPEED10 0x08
#define RL_GMEDIASTAT_LINK 0x02
#define RL_GMEDIASTAT_10MBPS 0x04
#define RL_GMEDIASTAT_100MBPS 0x08
#define RL_GMEDIASTAT_1000MBPS 0x10

#define MII_BMCR 0x00
#define MII_BMSR 0x01
#define MII_ANAR 0x04
#define MII_100T2CR 0x09
#define BMCR_RESET 0x8000
#define BMCR_AUTOEN 0x1000
#define BMCR_STARTNEG 0x0200
#define BMSR_LINK 0x0004
#define ANAR_CSMA 0x0001
#define ANAR_10 0x0020
#define ANAR_10_FD 0x0040
#define ANAR_TX 0x0080
#define ANAR_TX_FD 0x0100
#define ANAR_FC 0x0400
#define GTCR_ADV_1000THDX 0x0100
#define GTCR_ADV_1000TFDX 0x0200

#define RL_TDESC_CMD_EOF 0x10000000
#define RL_TDESC_CMD_SOF 0x20000000
#define RL_TDESC_CMD_EOR 0x40000000
#define RL_TDESC_CMD_OWN 0x80000000
#define RL_TDESC_STAT_TXERRSUM 0x00800000

#define RL_RDESC_CMD_EOR 0x40000000
#define RL_RDESC_CMD_OWN 0x80000000
#define RL_RDESC_STAT_SOF 0x20000000
#define RL_RDESC_STAT_EOF 0x10000000
#define RL_RDESC_STAT_GIANT 0x00200000
#define RL_RDESC_STAT_RXERRSUM 0x00100000
#define RL_RDESC_STAT_RUNT 0x00080000
#define RL_RDESC_STAT_CRCERR 0x00040000
#define RL_RDESC_STAT_FRAGLEN 0x00001fff
#define RL_RDESC_STAT_GFRAGLEN 0x00003fff
#define RL_RDESC_STAT_ERRS                                                     \
  (RL_RDESC_STAT_GIANT | RL_RDESC_STAT_RUNT | RL_RDESC_STAT_CRCERR)

#define RL_HWREV_8169 0x00000000
#define RL_HWREV_8169S 0x00800000
#define RL_HWREV_8110S 0x04000000
#define RL_HWREV_8169_8110SB 0x10000000
#define RL_HWREV_8169_8110SCd 0x18000000
#define RL_HWREV_8401E 0x24000000
#define RL_HWREV_8102EL 0x24800000
#define RL_HWREV_8102EL_SPIN1 0x24c00000
#define RL_HWREV_8168D 0x28000000
#define RL_HWREV_8168DP 0x28800000
#define RL_HWREV_8168E 0x2c000000
#define RL_HWREV_8168E_VL 0x2c800000
#define RL_HWREV_8168B_SPIN1 0x30000000
#define RL_HWREV_8100E 0x30800000
#define RL_HWREV_8101E 0x34000000
#define RL_HWREV_8102E 0x34800000
#define RL_HWREV_8103E 0x34c00000
#define RL_HWREV_8168B_SPIN2 0x38000000
#define RL_HWREV_8168B_SPIN3 0x38400000
#define RL_HWREV_8100E_SPIN2 0x38800000
#define RL_HWREV_8168C 0x3c000000
#define RL_HWREV_8168C_SPIN2 0x3c400000
#define RL_HWREV_8168CP 0x3c800000
#define RL_HWREV_8105E 0x40800000
#define RL_HWREV_8105E_SPIN1 0x40c00000
#define RL_HWREV_8402 0x44000000
#define RL_HWREV_8106E 0x44800000
#define RL_HWREV_8168F 0x48000000
#define RL_HWREV_8411 0x48800000
#define RL_HWREV_8168G 0x4c000000
#define RL_HWREV_8168EP 0x50000000
#define RL_HWREV_8168GU 0x50800000
#define RL_HWREV_8168H 0x54000000
#define RL_HWREV_8168FP 0x54800000
#define RL_HWREV_8411B 0x5c800000
#define RL_HWREV_8139CPLUS 0x74800000
#define RL_HWREV_8101 0x74c00000
#define RL_HWREV_8169_8110SBL 0x7cc00000
#define RL_HWREV_8100 0x78800000
#define RL_HWREV_8169_8110SCe 0x98000000

#define RL_FLAG_MSI 0x00000001
#define RL_FLAG_PCIE 0x00000004
#define RL_FLAG_PHYWAKE 0x00000008
#define RL_FLAG_PAR 0x00000010
#define RL_FLAG_DESCV2 0x00000020
#define RL_FLAG_MACSTAT 0x00000040
#define RL_FLAG_MACRESET 0x00000200
#define RL_FLAG_CMDSTOP 0x00000400
#define RL_FLAG_MACSLEEP 0x00000800
#define RL_FLAG_AUTOPAD 0x00001000
#define RL_FLAG_PHYWAKE_PM 0x00004000
#define RL_FLAG_EARLYOFF 0x00008000
#define RL_FLAG_EARLYOFFV2 0x00010000
#define RL_FLAG_RXDV_GATED 0x00020000
#define RL_FLAG_FASTETHER 0x00040000
#define RL_FLAG_CMDSTOP_WAIT_TXQ 0x00080000
#define RL_FLAG_JUMBOV2 0x00100000
#define RL_FLAG_WAIT_TXPOLL 0x00400000

#define RL_TIMEOUT 1000
#define RL_PHY_TIMEOUT 20

#define PCI_CAP_ID_PM 0x01
#define PCI_CAP_ID_PCIE 0x10
#define PCI_PMCSR 0x04
#define PCI_PMCSR_STATE 0x0003
#define PCI_PCIE_LCSR 0x10
#define PCI_PCIE_LCSR_ASPM 0x00000003
#define PCI_PCIE_LCSR_ECPM 0x00000100

#define RE_NDESC 256      // the 8169 family takes up to 1024
#define RE_NDESC_8139 64  // the 8139C+ no more than 64
#define RE_BUFLEN 2048

struct rl_desc {
  volatile uint32_t cmdstat;
  volatile uint32_t vlanctl;
  volatile uint32_t bufaddr_lo;
  volatile uint32_t bufaddr_hi;
};

static const struct {
  uint32_t chipid;
  const char *name;
} re_revisions[] = {
    {RL_HWREV_8100, "RTL8100"},
    {RL_HWREV_8100E, "RTL8100E"},
    {RL_HWREV_8100E_SPIN2, "RTL8100E 2"},
    {RL_HWREV_8101, "RTL8101"},
    {RL_HWREV_8101E, "RTL8101E"},
    {RL_HWREV_8102E, "RTL8102E"},
    {RL_HWREV_8106E, "RTL8106E"},
    {RL_HWREV_8401E, "RTL8401E"},
    {RL_HWREV_8402, "RTL8402"},
    {RL_HWREV_8411, "RTL8411"},
    {RL_HWREV_8411B, "RTL8411B"},
    {RL_HWREV_8102EL, "RTL8102EL"},
    {RL_HWREV_8102EL_SPIN1, "RTL8102EL 1"},
    {RL_HWREV_8103E, "RTL8103E"},
    {RL_HWREV_8110S, "RTL8110S"},
    {RL_HWREV_8139CPLUS, "RTL8139C+"},
    {RL_HWREV_8168B_SPIN1, "RTL8168 1"},
    {RL_HWREV_8168B_SPIN2, "RTL8168 2"},
    {RL_HWREV_8168B_SPIN3, "RTL8168 3"},
    {RL_HWREV_8168C, "RTL8168C/8111C"},
    {RL_HWREV_8168C_SPIN2, "RTL8168C/8111C"},
    {RL_HWREV_8168CP, "RTL8168CP/8111CP"},
    {RL_HWREV_8168F, "RTL8168F/8111F"},
    {RL_HWREV_8168G, "RTL8168G/8111G"},
    {RL_HWREV_8168GU, "RTL8168GU/8111GU"},
    {RL_HWREV_8168H, "RTL8168H/8111H"},
    {RL_HWREV_8105E, "RTL8105E"},
    {RL_HWREV_8105E_SPIN1, "RTL8105E"},
    {RL_HWREV_8168D, "RTL8168D/8111D"},
    {RL_HWREV_8168DP, "RTL8168DP/8111DP"},
    {RL_HWREV_8168E, "RTL8168E/8111E"},
    {RL_HWREV_8168E_VL, "RTL8168E/8111E-VL"},
    {RL_HWREV_8168EP, "RTL8168EP/8111EP"},
    {RL_HWREV_8168FP, "RTL8168FP/8111FP"},
    {RL_HWREV_8169, "RTL8169"},
    {RL_HWREV_8169_8110SB, "RTL8169/8110SB"},
    {RL_HWREV_8169_8110SBL, "RTL8169SBL"},
    {RL_HWREV_8169_8110SCd, "RTL8169/8110SCd"},
    {RL_HWREV_8169_8110SCe, "RTL8169/8110SCe"},
    {RL_HWREV_8169S, "RTL8169S"},
    {0, 0},
};

struct re_softc {
  struct mutex lock;
  struct device *dev;
  struct ifnet ifnet;
  struct callout timer; // re_tick: the link, once a second
  volatile uchar *mmio;
  ushort io;
  uint32_t hwrev;
  uint32_t flags;
  uint16_t product;
  int eewidth;
  uint32_t rxlenmask;
  uint txstart;
  uint cfg1, cfg2, cfg3, cfg4;

  struct rl_desc *tx_list;
  struct rl_desc *rx_list;
  char *tx_buf[RE_NDESC];
  char *rx_buf[RE_NDESC];
  uint ndesc; // descriptors in each ring
  uint tx_prod, tx_cons, tx_free;
  uint rx_cons;

  int link;
  int running;
  int tx_timer;          // re_watchdog: seconds left for the tx ring
  const char *why;       // what the reinit task is for
  struct task reinit;
};

#define RE_TX_TIMEOUT 5

// REGISTERS: memory or i/o space, whichever bar was taken

static uchar csr_read_1(struct re_softc *sc, uint off) {
  return sc->mmio ? *(volatile uchar *)(sc->mmio + off) : inb(sc->io + off);
}

static uint16_t csr_read_2(struct re_softc *sc, uint off) {
  return sc->mmio ? *(volatile uint16_t *)(sc->mmio + off) : inw(sc->io + off);
}

static uint32_t csr_read_4(struct re_softc *sc, uint off) {
  return sc->mmio ? *(volatile uint32_t *)(sc->mmio + off) : inl(sc->io + off);
}

static void csr_write_1(struct re_softc *sc, uint off, uchar v) {
  if (sc->mmio)
    *(volatile uchar *)(sc->mmio + off) = v;
  else
    outb(sc->io + off, v);
}

static void csr_write_2(struct re_softc *sc, uint off, uint16_t v) {
  if (sc->mmio)
    *(volatile uint16_t *)(sc->mmio + off) = v;
  else
    outw(sc->io + off, v);
}

static void csr_write_4(struct re_softc *sc, uint off, uint32_t v) {
  if (sc->mmio)
    *(volatile uint32_t *)(sc->mmio + off) = v;
  else
    outl(sc->io + off, v);
}

static void csr_setbit_1(struct re_softc *sc, uint off, uchar v) { csr_write_1(sc, off, csr_read_1(sc, off) | v); }
static void csr_clrbit_1(struct re_softc *sc, uint off, uchar v) { csr_write_1(sc, off, csr_read_1(sc, off) & ~v); }

// THE EEPROM, bit by bit, for the chips that keep the station address there

static void eeprom_putbyte(struct re_softc *sc, int addr) {
  int d, i;

  d = addr | (RL_9346_READ << sc->eewidth);
  for (i = 1 << (sc->eewidth + 3); i; i >>= 1) {
    if (d & i)
      csr_setbit_1(sc, RL_EECMD, RL_EE_DATAIN);
    else
      csr_clrbit_1(sc, RL_EECMD, RL_EE_DATAIN);
    delay(100);
    csr_setbit_1(sc, RL_EECMD, RL_EE_CLK);
    delay(150);
    csr_clrbit_1(sc, RL_EECMD, RL_EE_CLK);
    delay(100);
  }
}

static uint16_t eeprom_getword(struct re_softc *sc, int addr) {
  uint16_t word = 0;
  int i;

  eeprom_putbyte(sc, addr);
  for (i = 0x8000; i; i >>= 1) {
    csr_setbit_1(sc, RL_EECMD, RL_EE_CLK);
    delay(100);
    if (csr_read_1(sc, RL_EECMD) & RL_EE_DATAOUT)
      word |= i;
    csr_clrbit_1(sc, RL_EECMD, RL_EE_CLK);
    delay(100);
  }
  return word;
}

static void read_eeprom(struct re_softc *sc, uint16_t *dest, int off, int cnt) {
  int i;

  csr_setbit_1(sc, RL_EECMD, RL_EEMODE_PROGRAM);
  delay(100);
  for (i = 0; i < cnt; i++) {
    csr_setbit_1(sc, RL_EECMD, RL_EE_SEL);
    dest[i] = eeprom_getword(sc, off + i);
    csr_clrbit_1(sc, RL_EECMD, RL_EE_SEL);
  }
  csr_clrbit_1(sc, RL_EECMD, RL_EEMODE_PROGRAM);
}

// THE PHY: through PHYAR on the gigabit chips, through fixed registers on the 8139C+

static int gmii_read(struct re_softc *sc, int reg) {
  uint32_t rval;
  int i;

  csr_write_4(sc, RL_PHYAR, (uint32_t)reg << 16);
  for (i = 0; i < RL_PHY_TIMEOUT; i++) {
    rval = csr_read_4(sc, RL_PHYAR);
    if (rval & RL_PHYAR_BUSY)
      break;
    delay(25);
  }
  if (i == RL_PHY_TIMEOUT) {
    printf("%s: PHY read failed\n", sc->dev->name);
    return 0;
  }
  delay(20);
  return (int)(rval & RL_PHYAR_PHYDATA);
}

static void gmii_write(struct re_softc *sc, int reg, int data) {
  uint32_t rval;
  int i;

  csr_write_4(sc, RL_PHYAR, ((uint32_t)reg << 16) | (data & RL_PHYAR_PHYDATA) |
                            RL_PHYAR_BUSY);
  for (i = 0; i < RL_PHY_TIMEOUT; i++) {
    rval = csr_read_4(sc, RL_PHYAR);
    if (!(rval & RL_PHYAR_BUSY))
      break;
    delay(25);
  }
  if (i == RL_PHY_TIMEOUT)
    printf("%s: PHY write failed\n", sc->dev->name);
  delay(20);
}

static int mii_8139_reg(struct re_softc *sc __unused, int reg) {
  switch (reg) {
  case MII_BMCR: return RL_BMCR;
  case MII_BMSR: return RL_BMSR;
  case MII_ANAR: return RL_ANAR;
  default: return -1;
  }
}

static int phy_read(struct re_softc *sc, int reg) {
  int r;

  if (sc->hwrev != RL_HWREV_8139CPLUS)
    return gmii_read(sc, reg);
  if ((r = mii_8139_reg(sc, reg)) < 0)
    return 0;
  return csr_read_2(sc, (uint)r);
}

static void phy_write(struct re_softc *sc, int reg, int data) {
  int r;

  if (sc->hwrev != RL_HWREV_8139CPLUS) {
    gmii_write(sc, reg, data);
    return;
  }
  if ((r = mii_8139_reg(sc, reg)) < 0)
    return;
  csr_write_2(sc, (uint)r, (uint16_t)data);
}

// mii_phy_reset, then rgephy_mii_phy_auto
static void phy_auto(struct re_softc *sc) {
  int i, anar;

  for (i = 0; i < 15000; i++) {
    if (!(phy_read(sc, MII_BMSR) & BMSR_LINK))
      break;
    delay(10);
  }
  phy_write(sc, MII_BMCR, BMCR_RESET);
  delay(500);
  for (i = 0; i < 100; i++) {
    if ((phy_read(sc, MII_BMCR) & BMCR_RESET) == 0)
      break;
    delay(1000);
  }
  delay(1000);

  anar = ANAR_CSMA | ANAR_10 | ANAR_10_FD | ANAR_TX | ANAR_TX_FD | ANAR_FC;
  phy_write(sc, MII_ANAR, anar);
  delay(1000);
  if (sc->hwrev != RL_HWREV_8139CPLUS) {
    phy_write(sc, MII_100T2CR, GTCR_ADV_1000THDX | GTCR_ADV_1000TFDX);
    delay(1000);
  }
  phy_write(sc, MII_BMCR, BMCR_AUTOEN | BMCR_STARTNEG);
  delay(100);
}

// rgephy_status, reduced to the link
static int link_up(struct re_softc *sc, int *mbps) {
  uchar st;

  if (sc->hwrev == RL_HWREV_8139CPLUS) {
    // BMSR latches a loss: read it twice, as mii_phy_status does
    phy_read(sc, MII_BMSR);
    *mbps = (csr_read_1(sc, RL_MEDIASTAT) & RL_MEDIASTAT_SPEED10) ? 10 : 100;
    return (phy_read(sc, MII_BMSR) & BMSR_LINK) != 0;
  }
  st = csr_read_1(sc, RL_GMEDIASTAT);
  if (st & RL_GMEDIASTAT_1000MBPS)
    *mbps = 1000;
  else if (st & RL_GMEDIASTAT_100MBPS)
    *mbps = 100;
  else
    *mbps = 10;
  return (st & RL_GMEDIASTAT_LINK) != 0;
}

static void re_reset(struct re_softc *sc) {
  int i;

  csr_write_1(sc, RL_COMMAND, RL_CMD_RESET);
  for (i = 0; i < RL_TIMEOUT; i++) {
    delay(10);
    if (!(csr_read_1(sc, RL_COMMAND) & RL_CMD_RESET))
      break;
  }
  if (i == RL_TIMEOUT)
    printf("%s: reset never completed\n", sc->dev->name);
  if (sc->flags & RL_FLAG_MACRESET)
    csr_write_1(sc, RL_LDPS, 1);
}

// THE RINGS

static void set_bufaddr(struct re_softc *sc __unused, struct rl_desc *d, uint64_t pa) {
  d->bufaddr_lo = (uint32_t)pa;
  d->bufaddr_hi = (uint32_t)(pa >> 32);
}

static void rx_give(struct re_softc *sc, uint idx) {
  struct rl_desc *d = &sc->rx_list[idx];
  uint32_t cmdstat = RE_BUFLEN;

  if (idx == sc->ndesc - 1)
    cmdstat |= RL_RDESC_CMD_EOR;
  d->vlanctl = 0;
  set_bufaddr(sc, d, V2P(sc->rx_buf[idx]));
  d->cmdstat = cmdstat;
  mb();
  d->cmdstat = cmdstat | RL_RDESC_CMD_OWN;
  mb();
}

static void rings_free(struct re_softc *sc) {
  uint i;

  for (i = 0; i < sc->ndesc; i++) {
    if (sc->tx_buf[i] != 0)
      page_free(sc->tx_buf[i]);
    if (sc->rx_buf[i] != 0)
      page_free(sc->rx_buf[i]);
  }
  if (sc->tx_list != 0)
    page_free((char *)sc->tx_list);
  if (sc->rx_list != 0)
    page_free((char *)sc->rx_list);
}

static int rings_alloc(struct re_softc *sc) {
  uint i;

  if (sc->tx_list == 0 && (sc->tx_list = (struct rl_desc *)page_alloc()) == 0)
    return -1;
  if (sc->rx_list == 0 && (sc->rx_list = (struct rl_desc *)page_alloc()) == 0)
    return -1;
  for (i = 0; i < sc->ndesc; i++) {
    if (sc->tx_buf[i] == 0 && (sc->tx_buf[i] = page_alloc()) == 0)
      return -1;
    if (sc->rx_buf[i] == 0 && (sc->rx_buf[i] = page_alloc()) == 0)
      return -1;
  }
  return 0;
}

static void rings_init(struct re_softc *sc) {
  uint i;

  memset(sc->tx_list, 0, PGSIZE);
  memset(sc->rx_list, 0, PGSIZE);
  sc->tx_prod = sc->tx_cons = 0;
  sc->tx_free = sc->ndesc;
  sc->rx_cons = 0;
  for (i = 0; i < sc->ndesc; i++)
    rx_give(sc, i);
}

// re_rxeof: one buffer is one frame here, so a fragment is only an error
static int re_rxeof(struct re_softc *sc) {
  uint i = sc->rx_cons;
  int rx = 0;

  for (;;) {
    struct rl_desc *d = &sc->rx_list[i];
    uint32_t rxstat = d->cmdstat;
    uint total;

    if (rxstat & RL_RDESC_CMD_OWN)
      break;
    mb();
    total = rxstat & sc->rxlenmask;
    rx = 1;

    if ((rxstat & (RL_RDESC_STAT_SOF | RL_RDESC_STAT_EOF)) !=
        (RL_RDESC_STAT_SOF | RL_RDESC_STAT_EOF)) {
      rx_give(sc, i);
      i = (i + 1) % sc->ndesc;
      continue;
    }
    // the 8169 status bits sit one place to the left of the 8139C+ ones
    if (sc->hwrev != RL_HWREV_8139CPLUS)
      rxstat >>= 1;
    if ((rxstat & RL_RDESC_STAT_RXERRSUM) != 0 &&
        !(total > 8191 && (rxstat & RL_RDESC_STAT_ERRS) == RL_RDESC_STAT_GIANT)) {
      rx_give(sc, i);
      i = (i + 1) % sc->ndesc;
      continue;
    }
    if (total > 4 && total - 4 <= RE_BUFLEN)
      if_input(&sc->ifnet, sc->rx_buf[i], total - 4);
    rx_give(sc, i);
    i = (i + 1) % sc->ndesc;
  }
  sc->rx_cons = i;
  return rx;
}

static int re_txeof(struct re_softc *sc) {
  int freed = 0;

  while (sc->tx_cons != sc->tx_prod) {
    uint32_t st = sc->tx_list[sc->tx_cons].cmdstat;

    if (st & RL_TDESC_CMD_OWN)
      break;
    sc->tx_cons = (sc->tx_cons + 1) % sc->ndesc;
    sc->tx_free++;
    freed = 1;
  }
  if (sc->tx_cons == sc->tx_prod)
    sc->tx_timer = 0;
  return freed;
}

static int re_transmit(struct ifnet *ifp, const void *frame, uint len) {
  struct re_softc *sc = ifp->if_softc;
  struct rl_desc *d;
  uint idx;
  uint32_t cmdstat;

  if (len == 0 || len > RE_BUFLEN)
    return -1;
  mtx_enter(&sc->lock);
  if (!sc->running || !sc->link) {
    mtx_leave(&sc->lock);
    return -1;
  }
  re_txeof(sc);
  if (sc->tx_free < 2) {
    mtx_leave(&sc->lock);
    return -1;
  }
  idx = sc->tx_prod;
  memmove(sc->tx_buf[idx], frame, len);
  d = &sc->tx_list[idx];
  d->vlanctl = 0;
  set_bufaddr(sc, d, V2P(sc->tx_buf[idx]));
  cmdstat = RL_TDESC_CMD_SOF | RL_TDESC_CMD_EOF | len;
  if (idx == sc->ndesc - 1)
    cmdstat |= RL_TDESC_CMD_EOR;
  d->cmdstat = cmdstat;
  mb();
  d->cmdstat = cmdstat | RL_TDESC_CMD_OWN;
  mb();
  sc->tx_prod = (idx + 1) % sc->ndesc;
  sc->tx_free--;
  sc->tx_timer = RE_TX_TIMEOUT;
  csr_write_1(sc, sc->txstart, RL_TXSTART_START);
  mtx_leave(&sc->lock);
  return 0;
}

// re_tick: the link, once a second, and at once when it changes
static void re_link(struct re_softc *sc) {
  int mbps, up = link_up(sc, &mbps);

  if (up == sc->link)
    return;
  sc->link = up;
  if_link_state_change(&sc->ifnet, up ? LINK_STATE_UP : LINK_STATE_DOWN,
                       up ? (uint64_t)mbps * 1000000 : 0);
}

static void re_tick(void *arg) {
  struct re_softc *sc = arg;

  mtx_enter(&sc->lock);
  if (sc->running) {
    re_link(sc);
    if (sc->tx_timer > 0 && --sc->tx_timer == 0) {
      re_txeof(sc);
      if (sc->tx_cons != sc->tx_prod) {
        sc->why = "watchdog timeout";
        task_add(systq, &sc->reinit);
      }
    }
    callout_reset(&sc->timer, HZ, re_tick, sc);
  }
  mtx_leave(&sc->lock);
}

// re_intr: a line shared with others is not always this one's
static int re_intr(struct device *dev) {
  struct re_softc *sc = dev->driver_data;
  uint16_t status;

  if (sc == 0)
    return 0;
  mtx_enter(&sc->lock);
  if (!sc->running) {
    mtx_leave(&sc->lock);
    return 0;
  }
  // masked while it runs: the unmask at the end sends a new MSI for anything
  // that came meanwhile, as an edge would never come again
  csr_write_2(sc, RL_IMR, 0);
  if ((status = csr_read_2(sc, RL_ISR)) == 0xffff) {
    mtx_leave(&sc->lock);
    return 0;
  }
  if (status)
    csr_write_2(sc, RL_ISR, status);
  if ((status & RL_INTRS_CPLUS) == 0) {
    csr_write_2(sc, RL_IMR, RL_INTRS_CPLUS);
    mtx_leave(&sc->lock);
    return 0;
  }
  re_rxeof(sc);
  re_txeof(sc);
  if (status & RL_ISR_SYSTEM_ERR) {
    sc->why = "system error";
    task_add(systq, &sc->reinit);
  }
  if (status & RL_ISR_LINKCHG)
    re_link(sc);
  csr_write_2(sc, RL_IMR, RL_INTRS_CPLUS);
  mtx_leave(&sc->lock);
  return 1;
}

static void re_iff(struct re_softc *sc) {
  uint32_t rxfilt;

  rxfilt = csr_read_4(sc, RL_RXCFG);
  rxfilt &= ~(RL_RXCFG_RX_ALLPHYS | RL_RXCFG_RX_BROAD | RL_RXCFG_RX_INDIV |
              RL_RXCFG_RX_MULTI);
  rxfilt |= RL_RXCFG_RX_INDIV | RL_RXCFG_RX_BROAD | RL_RXCFG_RX_MULTI;
  csr_write_4(sc, RL_MAR0, 0xffffffff);
  csr_write_4(sc, RL_MAR4, 0xffffffff);
  csr_write_4(sc, RL_RXCFG, rxfilt);
}

static void re_set_jumbo(struct re_softc *sc) {
  csr_write_1(sc, RL_EECMD, RL_EEMODE_WRITECFG);
  csr_write_1(sc, RL_CFG3, csr_read_1(sc, RL_CFG3) | RL_CFG3_JUMBO_EN0);
  switch (sc->hwrev) {
  case RL_HWREV_8168DP: break;
  case RL_HWREV_8168E:
    csr_write_1(sc, RL_CFG4, csr_read_1(sc, RL_CFG4) | RL_CFG4_8168E_JUMBO_EN1);
    break;
  default:
    csr_write_1(sc, RL_CFG4, csr_read_1(sc, RL_CFG4) | RL_CFG4_JUMBO_EN1);
    break;
  }
  csr_write_1(sc, RL_EECMD, RL_EEMODE_OFF);
}

// the interrupt and the timer see it stopped first
static void re_stop(struct re_softc *sc) {
  int i;

  mtx_enter(&sc->lock);
  sc->running = 0;
  sc->link = 0;
  callout_stop_async(&sc->timer);
  mtx_leave(&sc->lock);
  csr_write_4(sc, RL_RXCFG, csr_read_4(sc, RL_RXCFG) &
                            ~(RL_RXCFG_RX_ALLPHYS | RL_RXCFG_RX_BROAD |
                              RL_RXCFG_RX_INDIV | RL_RXCFG_RX_MULTI));
  if (sc->flags & RL_FLAG_WAIT_TXPOLL) {
    for (i = RL_TIMEOUT; i > 0; i--) {
      if ((csr_read_1(sc, sc->txstart) & RL_TXSTART_START) == 0)
        break;
      delay(20);
    }
    csr_write_1(sc, RL_COMMAND, 0);
  } else if (sc->flags & RL_FLAG_CMDSTOP) {
    csr_write_1(sc, RL_COMMAND, RL_CMD_STOPREQ | RL_CMD_TX_ENB | RL_CMD_RX_ENB);
    if (sc->flags & RL_FLAG_CMDSTOP_WAIT_TXQ)
      for (i = RL_TIMEOUT; i > 0; i--) {
        if (csr_read_4(sc, RL_TXCFG) & RL_TXCFG_QUEUE_EMPTY)
          break;
        delay(100);
      }
  } else
    csr_write_1(sc, RL_COMMAND, 0);
  delay(1000);
  csr_write_2(sc, RL_IMR, 0);
  csr_write_2(sc, RL_ISR, 0xffff);
}

// re_init: the C+ command register first, then everything else
static int re_init(struct re_softc *sc) {
  const uchar *ea = sc->ifnet.if_lladdr;
  uint16_t cfg;
  uint32_t rxcfg;
  uint64_t pa;

  re_stop(sc);
  re_reset(sc);

  cfg = RL_CPLUSCMD_TXENB | RL_CPLUSCMD_PCI_MRW | RL_CPLUSCMD_RXCSUM_ENB;
  if (sc->flags & RL_FLAG_MACSTAT)
    cfg |= RL_CPLUSCMD_MACSTAT_DIS;
  else
    cfg |= RL_CPLUSCMD_RXENB;
  csr_write_2(sc, RL_CPLUS_CMD, cfg);

  csr_write_1(sc, RL_EECMD, RL_EEMODE_WRITECFG);
  csr_write_4(sc, RL_IDR4, (uint32_t)ea[4] | ((uint32_t)ea[5] << 8));
  csr_write_4(sc, RL_IDR0, (uint32_t)ea[0] | ((uint32_t)ea[1] << 8) |
                           ((uint32_t)ea[2] << 16) | ((uint32_t)ea[3] << 24));
  csr_write_1(sc, RL_EECMD, RL_EEMODE_OFF);

  if (sc->flags & RL_FLAG_JUMBOV2)
    re_set_jumbo(sc);

  rings_init(sc);

  pa = V2P(sc->rx_list);
  csr_write_4(sc, RL_RXLIST_ADDR_HI, (uint32_t)(pa >> 32));
  csr_write_4(sc, RL_RXLIST_ADDR_LO, (uint32_t)pa);
  pa = V2P(sc->tx_list);
  csr_write_4(sc, RL_TXLIST_ADDR_HI, (uint32_t)(pa >> 32));
  csr_write_4(sc, RL_TXLIST_ADDR_LO, (uint32_t)pa);

  if (sc->flags & RL_FLAG_RXDV_GATED)
    csr_write_4(sc, RL_MISC, csr_read_4(sc, RL_MISC) & ~0x00080000);

  csr_write_4(sc, RL_TXCFG, RL_TXCFG_CONFIG);
  csr_write_1(sc, RL_EARLY_TX_THRESH, 16);
  rxcfg = RL_RXCFG_CONFIG;
  if (sc->flags & RL_FLAG_EARLYOFF)
    rxcfg |= RL_RXCFG_EARLYOFF;
  else if (sc->flags & RL_FLAG_EARLYOFFV2)
    rxcfg |= RL_RXCFG_EARLYOFFV2;
  csr_write_4(sc, RL_RXCFG, rxcfg);

  csr_write_1(sc, RL_COMMAND, RL_CMD_TX_ENB | RL_CMD_RX_ENB);
  re_iff(sc);

  csr_write_2(sc, RL_IMR, RL_INTRS_CPLUS);
  csr_write_2(sc, RL_ISR, RL_INTRS_CPLUS);
  if (sc->hwrev == RL_HWREV_8139CPLUS)
    csr_write_4(sc, RL_TIMERINT, 0);
  else
    csr_write_4(sc, RL_TIMERINT_8169, 0);
  csr_write_4(sc, RL_MISSEDPKT, 0);

  if (sc->hwrev != RL_HWREV_8139CPLUS) {
    if ((sc->flags & RL_FLAG_PCIE) && (sc->flags & RL_FLAG_JUMBOV2) == 0)
      csr_write_2(sc, RL_MAXRXPKTLEN, RE_BUFLEN);
    else
      csr_write_2(sc, RL_MAXRXPKTLEN, 16383);
  }
  csr_write_1(sc, sc->cfg1, csr_read_1(sc, sc->cfg1) | RL_CFG1_DRVLOAD);

  phy_auto(sc);
  mtx_enter(&sc->lock);
  sc->running = 1;
  sc->link = 0;
  sc->tx_timer = 0;
  callout_reset(&sc->timer, HZ, re_tick, sc);
  mtx_leave(&sc->lock);
  return 0;
}

// re_watchdog, and re_intr on a system error: the chip from its reset
static void re_reinit(void *arg) {
  struct re_softc *sc = arg;

  if (!sc->running)
    return;
  printf("%s: %s\n", sc->dev->name, sc->why);
  re_init(sc);
}

// ASPM and clock power management off: the chips stop on a link in L1
static void aspm_off(struct device *dev) {
  int off = pci_find_capability(&dev->pci, PCI_CAP_ID_PCIE);
  uint32_t lcsr;

  if (off <= 0)
    return;
  lcsr = pci_read32(dev->pci.bus, dev->pci.device, dev->pci.function,
                    (uint)off + PCI_PCIE_LCSR);
  lcsr &= ~(PCI_PCIE_LCSR_ASPM | PCI_PCIE_LCSR_ECPM);
  pci_write32(dev->pci.bus, dev->pci.device, dev->pci.function,
              (uint)off + PCI_PCIE_LCSR, lcsr);
}

// ATTACH

static int re_probe(struct device *dev) {
  if (!dev || dev->bus != BUS_TYPE_PCI || dev->pci.vendor_id != PCI_VENDOR_REALTEK)
    return 0;
  switch (dev->pci.device_id) {
  case RT8139:
    // only the C+ revision has the descriptor rings
    return pci_read8(dev->pci.bus, dev->pci.device, dev->pci.function,
                     PCI_REG_REVISION_ID) == 0x20
               ? 100
               : 0;
  case RT8101E:
  case RT8168:
  case RT8168_2:
  case RT8169:
  case RT8169SC:
    return 100;
  default:
    return 0;
  }
}

static void re_flags_by_rev(struct re_softc *sc) {
  switch (sc->hwrev) {
  case RL_HWREV_8139CPLUS:
    sc->flags |= RL_FLAG_FASTETHER | RL_FLAG_AUTOPAD;
    break;
  case RL_HWREV_8100E:
  case RL_HWREV_8100E_SPIN2:
  case RL_HWREV_8101E:
    sc->flags |= RL_FLAG_PHYWAKE | RL_FLAG_FASTETHER;
    break;
  case RL_HWREV_8103E:
    sc->flags |= RL_FLAG_MACSLEEP;
    /* FALLTHROUGH */
  case RL_HWREV_8102E:
  case RL_HWREV_8102EL:
  case RL_HWREV_8102EL_SPIN1:
    sc->flags |= RL_FLAG_PHYWAKE | RL_FLAG_PAR | RL_FLAG_DESCV2 |
                RL_FLAG_MACSTAT | RL_FLAG_FASTETHER | RL_FLAG_CMDSTOP |
                RL_FLAG_AUTOPAD;
    break;
  case RL_HWREV_8401E:
  case RL_HWREV_8105E:
  case RL_HWREV_8105E_SPIN1:
  case RL_HWREV_8106E:
    sc->flags |= RL_FLAG_PHYWAKE | RL_FLAG_PHYWAKE_PM | RL_FLAG_PAR |
                RL_FLAG_DESCV2 | RL_FLAG_MACSTAT | RL_FLAG_FASTETHER |
                RL_FLAG_CMDSTOP | RL_FLAG_AUTOPAD;
    break;
  case RL_HWREV_8402:
    sc->flags |= RL_FLAG_PHYWAKE | RL_FLAG_PHYWAKE_PM | RL_FLAG_PAR |
                RL_FLAG_DESCV2 | RL_FLAG_MACSTAT | RL_FLAG_FASTETHER |
                RL_FLAG_CMDSTOP | RL_FLAG_AUTOPAD | RL_FLAG_CMDSTOP_WAIT_TXQ;
    break;
  case RL_HWREV_8168B_SPIN1:
  case RL_HWREV_8168B_SPIN2:
  case RL_HWREV_8168B_SPIN3:
    sc->flags |= RL_FLAG_PHYWAKE | RL_FLAG_MACSTAT;
    break;
  case RL_HWREV_8168C_SPIN2:
    sc->flags |= RL_FLAG_MACSLEEP;
    /* FALLTHROUGH */
  case RL_HWREV_8168C:
  case RL_HWREV_8168CP:
    sc->flags |= RL_FLAG_PHYWAKE | RL_FLAG_PAR | RL_FLAG_DESCV2 |
                RL_FLAG_MACSTAT | RL_FLAG_CMDSTOP | RL_FLAG_AUTOPAD |
                RL_FLAG_JUMBOV2;
    break;
  case RL_HWREV_8168D:
    sc->flags |= RL_FLAG_PHYWAKE | RL_FLAG_PHYWAKE_PM | RL_FLAG_PAR |
                RL_FLAG_DESCV2 | RL_FLAG_MACSTAT | RL_FLAG_CMDSTOP |
                RL_FLAG_AUTOPAD | RL_FLAG_JUMBOV2;
    break;
  case RL_HWREV_8168DP:
    sc->flags |= RL_FLAG_PHYWAKE | RL_FLAG_PAR | RL_FLAG_DESCV2 |
                RL_FLAG_MACSTAT | RL_FLAG_AUTOPAD | RL_FLAG_JUMBOV2 |
                RL_FLAG_WAIT_TXPOLL;
    break;
  case RL_HWREV_8168E:
    sc->flags |= RL_FLAG_PHYWAKE | RL_FLAG_PHYWAKE_PM | RL_FLAG_PAR |
                RL_FLAG_DESCV2 | RL_FLAG_MACSTAT | RL_FLAG_CMDSTOP |
                RL_FLAG_AUTOPAD | RL_FLAG_JUMBOV2;
    break;
  case RL_HWREV_8168E_VL:
    sc->flags |= RL_FLAG_EARLYOFF | RL_FLAG_PHYWAKE | RL_FLAG_PAR |
                RL_FLAG_DESCV2 | RL_FLAG_MACSTAT | RL_FLAG_CMDSTOP |
                RL_FLAG_AUTOPAD | RL_FLAG_JUMBOV2 | RL_FLAG_CMDSTOP_WAIT_TXQ;
    break;
  case RL_HWREV_8168F:
    sc->flags |= RL_FLAG_EARLYOFF;
    /* FALLTHROUGH */
  case RL_HWREV_8411:
    sc->flags |= RL_FLAG_PHYWAKE | RL_FLAG_PAR | RL_FLAG_DESCV2 |
                RL_FLAG_MACSTAT | RL_FLAG_CMDSTOP | RL_FLAG_AUTOPAD |
                RL_FLAG_JUMBOV2 | RL_FLAG_CMDSTOP_WAIT_TXQ;
    break;
  case RL_HWREV_8168EP:
  case RL_HWREV_8168FP:
  case RL_HWREV_8168G:
  case RL_HWREV_8168GU:
  case RL_HWREV_8168H:
  case RL_HWREV_8411B:
    if (sc->product == RT8101E)
      sc->flags |= RL_FLAG_FASTETHER;
    else
      sc->flags |= RL_FLAG_JUMBOV2;
    sc->flags |= RL_FLAG_PHYWAKE | RL_FLAG_PAR | RL_FLAG_DESCV2 |
                RL_FLAG_MACSTAT | RL_FLAG_CMDSTOP | RL_FLAG_AUTOPAD |
                RL_FLAG_CMDSTOP_WAIT_TXQ | RL_FLAG_EARLYOFFV2 |
                RL_FLAG_RXDV_GATED;
    break;
  case RL_HWREV_8169_8110SB:
  case RL_HWREV_8169_8110SBL:
  case RL_HWREV_8169_8110SCd:
  case RL_HWREV_8169_8110SCe:
    sc->flags |= RL_FLAG_PHYWAKE;
    /* FALLTHROUGH */
  case RL_HWREV_8169:
  case RL_HWREV_8169S:
  case RL_HWREV_8110S:
    sc->flags |= RL_FLAG_MACRESET;
    break;
  default:
    break;
  }
}

// the bars: memory below i/o, the 64-bit one first, as re_pci_attach takes them
static int map_registers(struct re_softc *sc, struct device *dev) {
  int b;

  for (b = 2; b >= 0; b--) {
    if (dev->pci.bar[b] == 0 || dev->pci.bar_is_io[b])
      continue;
    if (b == 0 && dev->mmio_vaddr != 0) {
      sc->mmio = (volatile uchar *)dev->mmio_vaddr;
      return 0;
    }
    sc->mmio = (volatile uchar *)ioremap(dev->pci.bar[b], dev->pci.bar_size[b]);
    if (sc->mmio != 0)
      return 0;
  }
  if (dev->pci.bar_is_io[0] && dev->pci.bar[0] != 0) {
    sc->io = (ushort)dev->pci.bar[0];
    return 0;
  }
  return -1;
}

// FreeBSD's re_attach and re_resume: the MAC out of its sleep, the PHY
// powered up again (re_clrwol), as a sleep or the firmware may leave them
static void re_power_up(struct re_softc *sc) {
  if ((sc->flags & RL_FLAG_MACSLEEP) && (csr_read_1(sc, RL_MACDBG) & 0x80))
    csr_write_1(sc, RL_GPIO, csr_read_1(sc, RL_GPIO) | 0x01);
  if (sc->flags & RL_FLAG_PHYWAKE_PM) {
    csr_write_1(sc, RL_PMCH, csr_read_1(sc, RL_PMCH) | 0x80);
    if (sc->hwrev == RL_HWREV_8401E)
      csr_write_1(sc, 0xd1, csr_read_1(sc, 0xd1) & ~0x08);
  }
  if (sc->flags & RL_FLAG_PHYWAKE) {
    gmii_write(sc, 0x1f, 0);
    gmii_write(sc, 0x0e, 0);
  }
}

static int re_attach(struct device *dev) {
  struct re_softc *sc;
  uint16_t as[3], did = 0;
  const char *name = 0;
  uchar *ea, cfg;
  int off, i;

  sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
  ea = sc->ifnet.if_lladdr;
  mtx_init(&sc->lock, "re");
  callout_init(&sc->timer);
  task_set(&sc->reinit, re_reinit, sc);
  sc->dev = dev;
  sc->product = dev->pci.device_id;
  if (map_registers(sc, dev) < 0) {
    printf("%s: no usable bar\n", dev->name);
    free(sc, M_DEVBUF);
    return -1;
  }

  // out of D3 and, on PCIe, out of ASPM
  if ((off = pci_find_capability(&dev->pci, PCI_CAP_ID_PM)) > 0) {
    uint16_t pmcsr = pci_read16(dev->pci.bus, dev->pci.device,
                                dev->pci.function, (uint)off + PCI_PMCSR);
    if (pmcsr & PCI_PMCSR_STATE) {
      pci_write16(dev->pci.bus, dev->pci.device, dev->pci.function,
                  (uint)off + PCI_PMCSR, pmcsr & ~PCI_PMCSR_STATE);
      delay(10000);
    }
  }
  if (pci_find_capability(&dev->pci, PCI_CAP_ID_PCIE) > 0) {
    aspm_off(dev);
    sc->flags |= RL_FLAG_PCIE;
  }

  if (driver_setup_intr_best(dev, PCI_INTR_MSI) < 0) {
    printf("%s: no interrupt\n", dev->name);
    free(sc, M_DEVBUF);
    return -1;
  }
  if (dev->intr.kind == PCI_INTR_MSI)
    sc->flags |= RL_FLAG_MSI;
  if (sc->product != RT8139) {
    csr_write_1(sc, RL_EECMD, RL_EE_MODE);
    cfg = csr_read_1(sc, RL_CFG2);
    if (sc->flags & RL_FLAG_MSI)
      csr_write_1(sc, RL_CFG2, cfg | RL_CFG2_MSI);
    else if (cfg & RL_CFG2_MSI)
      csr_write_1(sc, RL_CFG2, cfg & ~RL_CFG2_MSI);
    csr_write_1(sc, RL_EECMD, RL_EEMODE_OFF);
  }

  sc->hwrev = csr_read_4(sc, RL_TXCFG) & RL_TXCFG_HWREV;
  re_flags_by_rev(sc);
  sc->ndesc = sc->hwrev == RL_HWREV_8139CPLUS ? RE_NDESC_8139 : RE_NDESC;
  sc->cfg1 = RL_CFG1;
  if (sc->hwrev == RL_HWREV_8139CPLUS) {
    sc->rxlenmask = RL_RDESC_STAT_FRAGLEN;
    sc->txstart = RL_TXSTART;
  } else {
    sc->rxlenmask = RL_RDESC_STAT_GFRAGLEN;
    sc->txstart = RL_GTXSTART;
  }

  re_reset(sc);

  if (sc->flags & RL_FLAG_PAR) {
    for (i = 0; i < 6; i++)
      ea[i] = csr_read_1(sc, RL_IDR0 + (uint)i);
  } else {
    sc->eewidth = RL_9356_ADDR_LEN;
    read_eeprom(sc, &did, 0, 1);
    if (did != 0x8129)
      sc->eewidth = RL_9346_ADDR_LEN;
    read_eeprom(sc, as, RL_EE_EADDR, 3);
    for (i = 0; i < 3; i++) {
      ea[2 * i] = (uchar)as[i];
      ea[2 * i + 1] = (uchar)(as[i] >> 8);
    }
  }

  for (i = 0; re_revisions[i].name != 0; i++)
    if (re_revisions[i].chipid == sc->hwrev)
      name = re_revisions[i].name;
  printf("%s: %s (0x%x)\n", dev->name, name ? name : "unknown ASIC",
          sc->hwrev >> 16);

  if (rings_alloc(sc) < 0) {
    printf("%s: no memory for the rings\n", dev->name);
    rings_free(sc);
    free(sc, M_DEVBUF);
    return -1;
  }

  re_power_up(sc);

  dev->driver_data = sc;
  re_init(sc);
  strlcpy(sc->ifnet.if_xname, dev->name, sizeof(sc->ifnet.if_xname));
  sc->ifnet.if_mtu = ETHERMTU;
  sc->ifnet.if_transmit = re_transmit;
  sc->ifnet.if_softc = sc;
  if_attach(&sc->ifnet);
  return 0;
}

static int re_detach(struct device *dev) {
  re_stop(dev->driver_data);
  return 0;
}

static void re_shutdown(struct device *dev) {
  struct re_softc *sc = dev->driver_data;

  if (sc->running)
    re_stop(sc);
}

// SLEEP: stopped, and brought up from its reset as re_resume does
static int re_suspend(struct device *dev) {
  struct re_softc *sc = dev->driver_data;

  if (sc->running)
    re_stop(sc);
  return 0;
}

static int re_resume(struct device *dev) {
  aspm_off(dev);
  re_power_up(dev->driver_data);
  return re_init(dev->driver_data);
}

static struct driver re_driver = {
    .name = "re",
    .bus_type = BUS_TYPE_PCI,
    .dev_type = DEVICE_TYPE_NET,
    .probe = re_probe,
    .attach = re_attach,
    .detach = re_detach,
    .intr = re_intr,
    .shutdown = re_shutdown,
    .suspend = re_suspend,
    .resume = re_resume,
};

void re_driver_init(void) { driver_register(&re_driver); }
