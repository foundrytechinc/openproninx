#include "e1000_api.h"
#include "callout.h"
#include "if.h"
#include "malloc.h"
#include "driver.h"
#include "memlayout.h"
#include "mmu.h"
#include "mutex.h"
#include "param.h"

#define EM_NTXD 256
#define EM_NRXD 256
#define EM_BUFSIZE 2048
#define EM_FC_PAUSE_TIME 1000
#define EM_EIAC 0x000DC
#define EM_MSIX_MASK 0x01F00000
#define ETHERTYPE_VLAN 0x8100
#define AUTO_ALL_MODES 0
#define TARC_SPEED_MODE_BIT (1 << 21)
#define TARC_ERRATA_BIT (1 << 26)

static const struct {
  uint16_t id;
  const char *name;
} em_devices[] = {
    {E1000_DEV_ID_82540EM, "Legacy PRO/1000 MT 82540EM"},
    {E1000_DEV_ID_82540EM_LOM, "Legacy PRO/1000 MT 82540EM (LOM)"},
    {E1000_DEV_ID_82540EP, "Legacy PRO/1000 MT 82540EP"},
    {E1000_DEV_ID_82540EP_LOM, "Legacy PRO/1000 MT 82540EP (LOM)"},
    {E1000_DEV_ID_82540EP_LP, "Legacy PRO/1000 MT 82540EP (Mobile)"},
    {E1000_DEV_ID_82541EI, "Legacy PRO/1000 MT 82541EI (Copper)"},
    {E1000_DEV_ID_82541ER, "Legacy PRO/1000 82541ER"},
    {E1000_DEV_ID_82541ER_LOM, "Legacy PRO/1000 MT 82541ER"},
    {E1000_DEV_ID_82541EI_MOBILE, "Legacy PRO/1000 MT 82541EI (Mobile)"},
    {E1000_DEV_ID_82541GI, "Legacy PRO/1000 MT 82541GI"},
    {E1000_DEV_ID_82541GI_LF, "Legacy PRO/1000 GT 82541PI"},
    {E1000_DEV_ID_82541GI_MOBILE, "Legacy PRO/1000 MT 82541GI (Mobile)"},
    {E1000_DEV_ID_82542, "Legacy PRO/1000 82542 (Fiber)"},
    {E1000_DEV_ID_82543GC_FIBER, "Legacy PRO/1000 F 82543GC (Fiber)"},
    {E1000_DEV_ID_82543GC_COPPER, "Legacy PRO/1000 T 82543GC (Copper)"},
    {E1000_DEV_ID_82544EI_COPPER, "Legacy PRO/1000 XT 82544EI (Copper)"},
    {E1000_DEV_ID_82544EI_FIBER, "Legacy PRO/1000 XF 82544EI (Fiber)"},
    {E1000_DEV_ID_82544GC_COPPER, "Legacy PRO/1000 T 82544GC (Copper)"},
    {E1000_DEV_ID_82544GC_LOM, "Legacy PRO/1000 XT 82544GC (LOM)"},
    {E1000_DEV_ID_82545EM_COPPER, "Legacy PRO/1000 MT 82545EM (Copper)"},
    {E1000_DEV_ID_82545EM_FIBER, "Legacy PRO/1000 MF 82545EM (Fiber)"},
    {E1000_DEV_ID_82545GM_COPPER, "Legacy PRO/1000 MT 82545GM (Copper)"},
    {E1000_DEV_ID_82545GM_FIBER, "Legacy PRO/1000 MF 82545GM (Fiber)"},
    {E1000_DEV_ID_82545GM_SERDES, "Legacy PRO/1000 MB 82545GM (SERDES)"},
    {E1000_DEV_ID_82546EB_COPPER, "Legacy PRO/1000 MT 82546EB (Copper)"},
    {E1000_DEV_ID_82546EB_FIBER, "Legacy PRO/1000 MF 82546EB (Fiber)"},
    {E1000_DEV_ID_82546EB_QUAD_COPPER, "Legacy PRO/1000 MT 82546EB (Quad Copper"},
    {E1000_DEV_ID_82546GB_COPPER, "Legacy PRO/1000 MT 82546GB (Copper)"},
    {E1000_DEV_ID_82546GB_FIBER, "Legacy PRO/1000 MF 82546GB (Fiber)"},
    {E1000_DEV_ID_82546GB_SERDES, "Legacy PRO/1000 MB 82546GB (SERDES)"},
    {E1000_DEV_ID_82546GB_PCIE, "Legacy PRO/1000 P 82546GB (PCIe)"},
    {E1000_DEV_ID_82546GB_QUAD_COPPER, "Legacy PRO/1000 GT 82546GB (Quad Copper)"},
    {E1000_DEV_ID_82546GB_QUAD_COPPER_KSP3, "Legacy PRO/1000 GT 82546GB (Quad Copper)"},
    {E1000_DEV_ID_82547EI, "Legacy PRO/1000 CT 82547EI"},
    {E1000_DEV_ID_82547EI_MOBILE, "Legacy PRO/1000 CT 82547EI (Mobile)"},
    {E1000_DEV_ID_82547GI, "Legacy PRO/1000 CT 82547GI"},
    {E1000_DEV_ID_82571EB_COPPER, "PRO/1000 PT 82571EB/82571GB (Copper)"},
    {E1000_DEV_ID_82571EB_FIBER, "PRO/1000 PF 82571EB/82571GB (Fiber)"},
    {E1000_DEV_ID_82571EB_SERDES, "PRO/1000 PB 82571EB (SERDES)"},
    {E1000_DEV_ID_82571EB_SERDES_DUAL, "PRO/1000 82571EB (Dual Mezzanine)"},
    {E1000_DEV_ID_82571EB_SERDES_QUAD, "PRO/1000 82571EB (Quad Mezzanine)"},
    {E1000_DEV_ID_82571EB_QUAD_COPPER, "PRO/1000 PT 82571EB/82571GB (Quad Copper)"},
    {E1000_DEV_ID_82571EB_QUAD_COPPER_LP, "PRO/1000 PT 82571EB/82571GB (Quad Copper)"},
    {E1000_DEV_ID_82571EB_QUAD_FIBER, "PRO/1000 PF 82571EB (Quad Fiber)"},
    {E1000_DEV_ID_82571PT_QUAD_COPPER, "PRO/1000 PT 82571PT (Quad Copper)"},
    {E1000_DEV_ID_82572EI, "PRO/1000 PT 82572EI (Copper)"},
    {E1000_DEV_ID_82572EI_COPPER, "PRO/1000 PT 82572EI (Copper)"},
    {E1000_DEV_ID_82572EI_FIBER, "PRO/1000 PF 82572EI (Fiber)"},
    {E1000_DEV_ID_82572EI_SERDES, "PRO/1000 82572EI (SERDES)"},
    {E1000_DEV_ID_82573E, "PRO/1000 82573E (Copper)"},
    {E1000_DEV_ID_82573E_IAMT, "PRO/1000 82573E AMT (Copper)"},
    {E1000_DEV_ID_82573L, "PRO/1000 82573L"},
    {E1000_DEV_ID_82583V, "82583V"},
    {E1000_DEV_ID_80003ES2LAN_COPPER_SPT, "80003ES2LAN (Copper)"},
    {E1000_DEV_ID_80003ES2LAN_SERDES_SPT, "80003ES2LAN (SERDES)"},
    {E1000_DEV_ID_80003ES2LAN_COPPER_DPT, "80003ES2LAN (Dual Copper)"},
    {E1000_DEV_ID_80003ES2LAN_SERDES_DPT, "80003ES2LAN (Dual SERDES)"},
    {E1000_DEV_ID_ICH8_IGP_M_AMT, "82566MM ICH8 AMT (Mobile)"},
    {E1000_DEV_ID_ICH8_IGP_AMT, "82566DM ICH8 AMT"},
    {E1000_DEV_ID_ICH8_IGP_C, "82566DC ICH8"},
    {E1000_DEV_ID_ICH8_IFE, "82562V ICH8"},
    {E1000_DEV_ID_ICH8_IFE_GT, "82562GT ICH8"},
    {E1000_DEV_ID_ICH8_IFE_G, "82562G ICH8"},
    {E1000_DEV_ID_ICH8_IGP_M, "82566MC ICH8"},
    {E1000_DEV_ID_ICH8_82567V_3, "82567V-3 ICH8"},
    {E1000_DEV_ID_ICH9_IGP_M_AMT, "82567LM ICH9 AMT"},
    {E1000_DEV_ID_ICH9_IGP_AMT, "82566DM-2 ICH9 AMT"},
    {E1000_DEV_ID_ICH9_IGP_C, "82566DC-2 ICH9"},
    {E1000_DEV_ID_ICH9_IGP_M, "82567LF ICH9"},
    {E1000_DEV_ID_ICH9_IGP_M_V, "82567V ICH9"},
    {E1000_DEV_ID_ICH9_IFE, "82562V-2 ICH9"},
    {E1000_DEV_ID_ICH9_IFE_GT, "82562GT-2 ICH9"},
    {E1000_DEV_ID_ICH9_IFE_G, "82562G-2 ICH9"},
    {E1000_DEV_ID_ICH9_BM, "82567LM-4 ICH9"},
    {E1000_DEV_ID_82574L, "Gigabit CT 82574L"},
    {E1000_DEV_ID_82574LA, "82574L-Apple"},
    {E1000_DEV_ID_ICH10_R_BM_LM, "82567LM-2 ICH10"},
    {E1000_DEV_ID_ICH10_R_BM_LF, "82567LF-2 ICH10"},
    {E1000_DEV_ID_ICH10_R_BM_V, "82567V-2 ICH10"},
    {E1000_DEV_ID_ICH10_D_BM_LM, "82567LM-3 ICH10"},
    {E1000_DEV_ID_ICH10_D_BM_LF, "82567LF-3 ICH10"},
    {E1000_DEV_ID_ICH10_D_BM_V, "82567V-4 ICH10"},
    {E1000_DEV_ID_PCH_M_HV_LM, "82577LM"},
    {E1000_DEV_ID_PCH_M_HV_LC, "82577LC"},
    {E1000_DEV_ID_PCH_D_HV_DM, "82578DM"},
    {E1000_DEV_ID_PCH_D_HV_DC, "82578DC"},
    {E1000_DEV_ID_PCH2_LV_LM, "82579LM"},
    {E1000_DEV_ID_PCH2_LV_V, "82579V"},
    {E1000_DEV_ID_PCH_LPT_I217_LM, "I217-LM LPT"},
    {E1000_DEV_ID_PCH_LPT_I217_V, "I217-V LPT"},
    {E1000_DEV_ID_PCH_LPTLP_I218_LM, "I218-LM LPTLP"},
    {E1000_DEV_ID_PCH_LPTLP_I218_V, "I218-V LPTLP"},
    {E1000_DEV_ID_PCH_I218_LM2, "I218-LM (2)"},
    {E1000_DEV_ID_PCH_I218_V2, "I218-V (2)"},
    {E1000_DEV_ID_PCH_I218_LM3, "I218-LM (3)"},
    {E1000_DEV_ID_PCH_I218_V3, "I218-V (3)"},
    {E1000_DEV_ID_PCH_SPT_I219_LM, "I219-LM SPT"},
    {E1000_DEV_ID_PCH_SPT_I219_V, "I219-V SPT"},
    {E1000_DEV_ID_PCH_SPT_I219_LM2, "I219-LM SPT-H(2)"},
    {E1000_DEV_ID_PCH_SPT_I219_V2, "I219-V SPT-H(2)"},
    {E1000_DEV_ID_PCH_LBG_I219_LM3, "I219-LM LBG(3)"},
    {E1000_DEV_ID_PCH_SPT_I219_LM4, "I219-LM SPT(4)"},
    {E1000_DEV_ID_PCH_SPT_I219_V4, "I219-V SPT(4)"},
    {E1000_DEV_ID_PCH_SPT_I219_LM5, "I219-LM SPT(5)"},
    {E1000_DEV_ID_PCH_SPT_I219_V5, "I219-V SPT(5)"},
    {E1000_DEV_ID_PCH_CNP_I219_LM6, "I219-LM CNP(6)"},
    {E1000_DEV_ID_PCH_CNP_I219_V6, "I219-V CNP(6)"},
    {E1000_DEV_ID_PCH_CNP_I219_LM7, "I219-LM CNP(7)"},
    {E1000_DEV_ID_PCH_CNP_I219_V7, "I219-V CNP(7)"},
    {E1000_DEV_ID_PCH_ICP_I219_LM8, "I219-LM ICP(8)"},
    {E1000_DEV_ID_PCH_ICP_I219_V8, "I219-V ICP(8)"},
    {E1000_DEV_ID_PCH_ICP_I219_LM9, "I219-LM ICP(9)"},
    {E1000_DEV_ID_PCH_ICP_I219_V9, "I219-V ICP(9)"},
    {E1000_DEV_ID_PCH_CMP_I219_LM10, "I219-LM CMP(10)"},
    {E1000_DEV_ID_PCH_CMP_I219_V10, "I219-V CMP(10)"},
    {E1000_DEV_ID_PCH_CMP_I219_LM11, "I219-LM CMP(11)"},
    {E1000_DEV_ID_PCH_CMP_I219_V11, "I219-V CMP(11)"},
    {E1000_DEV_ID_PCH_CMP_I219_LM12, "I219-LM CMP(12)"},
    {E1000_DEV_ID_PCH_CMP_I219_V12, "I219-V CMP(12)"},
    {E1000_DEV_ID_PCH_TGP_I219_LM13, "I219-LM TGP(13)"},
    {E1000_DEV_ID_PCH_TGP_I219_V13, "I219-V TGP(13)"},
    {E1000_DEV_ID_PCH_TGP_I219_LM14, "I219-LM TGP(14)"},
    {E1000_DEV_ID_PCH_TGP_I219_V14, "I219-V GTP(14)"},
    {E1000_DEV_ID_PCH_TGP_I219_LM15, "I219-LM TGP(15)"},
    {E1000_DEV_ID_PCH_TGP_I219_V15, "I219-V TGP(15)"},
    {E1000_DEV_ID_PCH_ADL_I219_LM16, "I219-LM ADL(16)"},
    {E1000_DEV_ID_PCH_ADL_I219_V16, "I219-V ADL(16)"},
    {E1000_DEV_ID_PCH_ADL_I219_LM17, "I219-LM ADL(17)"},
    {E1000_DEV_ID_PCH_ADL_I219_V17, "I219-V ADL(17)"},
    {E1000_DEV_ID_PCH_MTP_I219_LM18, "I219-LM MTP(18)"},
    {E1000_DEV_ID_PCH_MTP_I219_V18, "I219-V MTP(18)"},
    {E1000_DEV_ID_PCH_ADL_I219_LM19, "I219-LM ADL(19)"},
    {E1000_DEV_ID_PCH_ADL_I219_V19, "I219-V ADL(19)"},
    {E1000_DEV_ID_PCH_LNL_I219_LM20, "I219-LM LNL(20)"},
    {E1000_DEV_ID_PCH_LNL_I219_V20, "I219-V LNL(20)"},
    {E1000_DEV_ID_PCH_LNL_I219_LM21, "I219-LM LNL(21)"},
    {E1000_DEV_ID_PCH_LNL_I219_V21, "I219-V LNL(21)"},
    {E1000_DEV_ID_PCH_RPL_I219_LM22, "I219-LM RPL(22)"},
    {E1000_DEV_ID_PCH_RPL_I219_V22, "I219-V RPL(22)"},
    {E1000_DEV_ID_PCH_RPL_I219_LM23, "I219-LM RPL(23)"},
    {E1000_DEV_ID_PCH_RPL_I219_V23, "I219-V RPL(23)"},
    {E1000_DEV_ID_PCH_ARL_I219_LM24, "I219-LM ARL(24)"},
    {E1000_DEV_ID_PCH_ARL_I219_V24, "I219-V ARL(24)"},
    {E1000_DEV_ID_PCH_PTP_I219_LM25, "I219-LM PTP(25)"},
    {E1000_DEV_ID_PCH_PTP_I219_V25, "I219-V PTP(25)"},
    {E1000_DEV_ID_PCH_PTP_I219_LM26, "I219-LM PTP(26)"},
    {E1000_DEV_ID_PCH_PTP_I219_V26, "I219-V PTP(26)"},
    {E1000_DEV_ID_PCH_PTP_I219_LM27, "I219-LM PTP(27)"},
    {E1000_DEV_ID_PCH_PTP_I219_V27, "I219-V PTP(27)"},
    {E1000_DEV_ID_PCH_NVL_I219_LM29, "I219-LM NVL(29)"},
    {E1000_DEV_ID_PCH_NVL_I219_V29, "I219-V NVL(29)"},
    {E1000_DEV_ID_82575EB_COPPER, "PRO/1000 82575EB (Copper)"},
    {E1000_DEV_ID_82575EB_FIBER_SERDES, "PRO/1000 82575EB (SERDES)"},
    {E1000_DEV_ID_82575GB_QUAD_COPPER, "PRO/1000 VT 82575GB (Quad Copper)"},
    {E1000_DEV_ID_82576, "PRO/1000 82576"},
    {E1000_DEV_ID_82576_NS, "PRO/1000 82576NS"},
    {E1000_DEV_ID_82576_NS_SERDES, "PRO/1000 82576NS (SERDES)"},
    {E1000_DEV_ID_82576_FIBER, "PRO/1000 EF 82576 (Dual Fiber)"},
    {E1000_DEV_ID_82576_SERDES, "PRO/1000 82576 (Dual SERDES)"},
    {E1000_DEV_ID_82576_SERDES_QUAD, "PRO/1000 ET 82576 (Quad SERDES)"},
    {E1000_DEV_ID_82576_QUAD_COPPER, "PRO/1000 ET 82576 (Quad Copper)"},
    {E1000_DEV_ID_82576_QUAD_COPPER_ET2, "PRO/1000 ET(2) 82576 (Quad Copper)"},
    {E1000_DEV_ID_82580_COPPER, "I340 82580 (Copper)"},
    {E1000_DEV_ID_82580_FIBER, "I340 82580 (Fiber)"},
    {E1000_DEV_ID_82580_SERDES, "I340 82580 (SERDES)"},
    {E1000_DEV_ID_82580_SGMII, "I340 82580 (SGMII)"},
    {E1000_DEV_ID_82580_COPPER_DUAL, "I340-T2 82580 (Dual Copper)"},
    {E1000_DEV_ID_82580_QUAD_FIBER, "I340-F4 82580 (Quad Fiber)"},
    {E1000_DEV_ID_DH89XXCC_SERDES, "DH89XXCC (SERDES)"},
    {E1000_DEV_ID_DH89XXCC_SGMII, "I347-AT4 DH89XXCC"},
    {E1000_DEV_ID_DH89XXCC_SFP, "DH89XXCC (SFP)"},
    {E1000_DEV_ID_DH89XXCC_BACKPLANE, "DH89XXCC (Backplane)"},
    {E1000_DEV_ID_I350_COPPER, "I350 (Copper)"},
    {E1000_DEV_ID_I350_FIBER, "I350 (Fiber)"},
    {E1000_DEV_ID_I350_SERDES, "I350 (SERDES)"},
    {E1000_DEV_ID_I350_SGMII, "I350 (SGMII)"},
    {E1000_DEV_ID_I210_COPPER, "I210 (Copper)"},
    {E1000_DEV_ID_I210_COPPER_IT, "I210 IT (Copper)"},
    {E1000_DEV_ID_I210_COPPER_OEM1, "I210 (OEM)"},
    {E1000_DEV_ID_I210_COPPER_FLASHLESS, "I210 Flashless (Copper)"},
    {E1000_DEV_ID_I210_SERDES_FLASHLESS, "I210 Flashless (SERDES)"},
    {E1000_DEV_ID_I210_SGMII_FLASHLESS, "I210 Flashless (SGMII)"},
    {E1000_DEV_ID_I210_FIBER, "I210 (Fiber)"},
    {E1000_DEV_ID_I210_SERDES, "I210 (SERDES)"},
    {E1000_DEV_ID_I210_SGMII, "I210 (SGMII)"},
    {E1000_DEV_ID_I211_COPPER, "I211 (Copper)"},
    {E1000_DEV_ID_I354_BACKPLANE_1GBPS, "I354 (1.0 GbE Backplane)"},
    {E1000_DEV_ID_I354_BACKPLANE_2_5GBPS, "I354 (2.5 GbE Backplane)"},
    {E1000_DEV_ID_I354_SGMII, "I354 (SGMII)"},
};

struct em_softc {
  struct device *dev;
  struct ifnet ifnet;
  struct mutex lock;
  struct callout timer;
  struct e1000_hw hw;
  struct e1000_osdep osdep;
  struct e1000_tx_desc *txd;
  struct e1000_rx_desc *rxd;
  int igb;
  char *txbuf[EM_NTXD];
  char *rxbuf[EM_NRXD];
  uint tx_prod, tx_cons, tx_free, rx_cons;
  int running, link_active, has_mgmt, has_amt, hw_control;
  uint16_t link_speed, link_duplex;
};

static const char *em_name(uint16_t id) {
  uint i;

  for (i = 0; i < nitems(em_devices); i++)
    if (em_devices[i].id == id)
      return em_devices[i].name;
  return 0;
}

static int em_probe(struct device *dev) {
  if (dev == 0 || dev->bus != BUS_TYPE_PCI || dev->pci.vendor_id != 0x8086)
    return 0;
  return em_name(dev->pci.device_id) != 0 ? 100 : 0;
}

static int em_transmit(struct ifnet *ifp, const void *frame, uint length) {
  struct em_softc *sc = ifp->if_softc;
  struct e1000_tx_desc *d;
  uint i;

  if (length > EM_BUFSIZE)
    return -1;
  mtx_enter(&sc->lock);
  if (!sc->running) {
    mtx_leave(&sc->lock);
    return -1;
  }
  while (sc->tx_free < EM_NTXD &&
         (sc->txd[sc->tx_cons].upper.fields.status & E1000_TXD_STAT_DD)) {
    sc->txd[sc->tx_cons].upper.data = 0;
    sc->tx_cons = (sc->tx_cons + 1) % EM_NTXD;
    sc->tx_free++;
  }
  if (sc->tx_free == 0) {
    mtx_leave(&sc->lock);
    return -1;
  }
  i = sc->tx_prod;
  d = &sc->txd[i];
  memmove(sc->txbuf[i], frame, length);
  d->buffer_addr = V2P(sc->txbuf[i]);
  if (sc->igb) {
    union e1000_adv_tx_desc *a = (union e1000_adv_tx_desc *)d;
    a->read.cmd_type_len = length | E1000_ADVTXD_DTYP_DATA | E1000_ADVTXD_DCMD_DEXT |
                           E1000_ADVTXD_DCMD_IFCS | E1000_ADVTXD_DCMD_EOP | E1000_ADVTXD_DCMD_RS;
    a->read.olinfo_status = length << E1000_ADVTXD_PAYLEN_SHIFT;
  } else {
    d->lower.data = length | E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    d->upper.data = 0;
  }
  mb();
  sc->tx_prod = (i + 1) % EM_NTXD;
  sc->tx_free--;
  E1000_WRITE_REG(&sc->hw, E1000_TDT(0), sc->tx_prod);
  mtx_leave(&sc->lock);
  return 0;
}

static int em_rx_done(struct em_softc *sc, uint i, uint *len, int *good) {
  if (sc->igb) {
    union e1000_adv_rx_desc *a = (union e1000_adv_rx_desc *)&sc->rxd[i];
    u32 staterr = a->wb.upper.status_error;

    if (!(staterr & E1000_RXD_STAT_DD))
      return 0;
    *len = a->wb.upper.length;
    *good = (staterr & E1000_RXD_STAT_EOP) && !(staterr & E1000_RXDEXT_ERR_FRAME_ERR_MASK);
    a->read.pkt_addr = V2P(sc->rxbuf[i]);
    a->read.hdr_addr = 0;
    return 1;
  }
  if (!(sc->rxd[i].status & E1000_RXD_STAT_DD))
    return 0;
  *len = sc->rxd[i].length;
  *good = (sc->rxd[i].status & E1000_RXD_STAT_EOP) &&
          (sc->rxd[i].errors & E1000_RXD_ERR_FRAME_ERR_MASK) == 0;
  sc->rxd[i].status = 0;
  return 1;
}

static void em_rxeof(struct em_softc *sc) {
  uint len;
  int good;

  while (em_rx_done(sc, sc->rx_cons, &len, &good)) {
    uint i = sc->rx_cons;

    if (good && len > 0)
      if_input(&sc->ifnet, sc->rxbuf[i], len);
    mb();
    E1000_WRITE_REG(&sc->hw, E1000_RDT(0), i);
    sc->rx_cons = (i + 1) % EM_NRXD;
  }
}

static void em_update_link_status(struct em_softc *sc) {
  struct e1000_hw *hw = &sc->hw;
  int link = 0;

  switch (hw->phy.media_type) {
  case e1000_media_type_copper:
    if (hw->mac.get_link_status) {
      if (hw->mac.type >= e1000_pch_spt)
        msec_delay(50);
      e1000_check_for_link(hw);
      link = !hw->mac.get_link_status;
      if (link)
        e1000_cfg_on_link_up(hw);
    } else
      link = 1;
    break;
  case e1000_media_type_fiber:
    e1000_check_for_link(hw);
    link = (E1000_READ_REG(hw, E1000_STATUS) & E1000_STATUS_LU) != 0;
    break;
  case e1000_media_type_internal_serdes:
    e1000_check_for_link(hw);
    link = hw->mac.serdes_has_link;
    break;
  default:
    break;
  }
  if (link && !sc->link_active) {
    e1000_get_speed_and_duplex(hw, &sc->link_speed, &sc->link_duplex);
    // 82571/82572: SPEED_MODE only at gigabit
    if (sc->link_speed != SPEED_1000 &&
        (hw->mac.type == e1000_82571 || hw->mac.type == e1000_82572)) {
      u32 tarc0 = E1000_READ_REG(hw, E1000_TARC(0));
      E1000_WRITE_REG(hw, E1000_TARC(0), tarc0 & ~TARC_SPEED_MODE_BIT);
    }
    sc->link_active = 1;
    if_link_state_change(&sc->ifnet,
                         sc->link_duplex == FULL_DUPLEX ? LINK_STATE_FULL_DUPLEX : LINK_STATE_UP,
                         (uint64_t)sc->link_speed * 1000000);
  } else if (!link && sc->link_active) {
    sc->link_speed = sc->link_duplex = 0;
    sc->link_active = 0;
    if_link_state_change(&sc->ifnet, LINK_STATE_DOWN, 0);
  }
}

static int em_intr(struct device *dev) {
  struct em_softc *sc = dev->driver_data;
  u32 icr;

  if (sc == 0)
    return 0;
  mtx_enter(&sc->lock);
  icr = sc->running ? E1000_READ_REG(&sc->hw, E1000_ICR) : 0;
  if (icr == 0xffffffff || icr == 0 ||
      (sc->hw.mac.type >= e1000_82571 && (icr & E1000_ICR_INT_ASSERTED) == 0)) {
    mtx_leave(&sc->lock);
    return 0;
  }
  em_rxeof(sc);
  if (icr & (E1000_ICR_RXSEQ | E1000_ICR_LSC)) {
    sc->hw.mac.get_link_status = 1;
    em_update_link_status(sc);
  }
  mtx_leave(&sc->lock);
  return 1;
}

static void em_local_timer(void *arg) {
  struct em_softc *sc = arg;

  mtx_enter(&sc->lock);
  if (sc->running) {
    em_update_link_status(sc);
    callout_reset(&sc->timer, HZ, em_local_timer, sc);
  }
  mtx_leave(&sc->lock);
}

static void em_get_hw_control(struct em_softc *sc) {
  struct e1000_hw *hw = &sc->hw;

  if (hw->mac.type == e1000_82573)
    E1000_WRITE_REG(hw, E1000_SWSM, E1000_READ_REG(hw, E1000_SWSM) | E1000_SWSM_DRV_LOAD);
  else
    E1000_WRITE_REG(hw, E1000_CTRL_EXT,
                    E1000_READ_REG(hw, E1000_CTRL_EXT) | E1000_CTRL_EXT_DRV_LOAD);
  sc->hw_control = 1;
}

static void em_rel_hw_control(struct em_softc *sc) {
  struct e1000_hw *hw = &sc->hw;

  if (!sc->hw_control)
    return;
  sc->hw_control = 0;
  if (hw->mac.type == e1000_82573)
    E1000_WRITE_REG(hw, E1000_SWSM, E1000_READ_REG(hw, E1000_SWSM) & ~E1000_SWSM_DRV_LOAD);
  else
    E1000_WRITE_REG(hw, E1000_CTRL_EXT,
                    E1000_READ_REG(hw, E1000_CTRL_EXT) & ~E1000_CTRL_EXT_DRV_LOAD);
}

static void em_get_mgmt(struct em_softc *sc) {
  struct e1000_hw *hw = &sc->hw;
  u32 manc, manc2h;

  if (!sc->has_mgmt)
    return;
  manc = E1000_READ_REG(hw, E1000_MANC) & ~E1000_MANC_ARP_EN;
  if (hw->mac.type >= e1000_82571) {
    manc |= E1000_MANC_EN_MNG2HOST;
    manc2h = E1000_READ_REG(hw, E1000_MANC2H) | (1 << 5) | (1 << 6);
    E1000_WRITE_REG(hw, E1000_MANC2H, manc2h);
  }
  E1000_WRITE_REG(hw, E1000_MANC, manc);
}

static void em_rel_mgmt(struct em_softc *sc) {
  struct e1000_hw *hw = &sc->hw;
  u32 manc;

  if (!sc->has_mgmt)
    return;
  manc = E1000_READ_REG(hw, E1000_MANC) | E1000_MANC_ARP_EN;
  if (hw->mac.type >= e1000_82571)
    manc &= ~E1000_MANC_EN_MNG2HOST;
  E1000_WRITE_REG(hw, E1000_MANC, manc);
}

static void em_disable_aspm(struct em_softc *sc) {
  const struct pci_device *p = sc->osdep.pci;
  uint16_t disable, lcap, lctl;
  int off;

  switch (sc->hw.mac.type) {
  case e1000_82571:
  case e1000_82572:
  case e1000_82573:
    disable = 3;
    break;
  case e1000_82574:
  case e1000_82583:
    disable = 1;
    break;
  default:
    return;
  }
  if ((off = pci_find_capability(p, 0x10)) < 0)
    return;
  lcap = pci_read16(p->bus, p->device, p->function, (uint)off + 0x0c);
  if ((lcap & (3 << 10)) == 0)
    return;
  lctl = pci_read16(p->bus, p->device, p->function, (uint)off + 0x10);
  pci_write16(p->bus, p->device, p->function, (uint)off + 0x10, lctl & ~disable);
}

static int em_reset(struct em_softc *sc) {
  struct e1000_hw *hw = &sc->hw;
  u32 pba;
  u16 rx_buffer_size;

  if (hw->mac.type == e1000_82571 || hw->mac.type == e1000_82572) {
    u16 phy_tmp = 0;

    e1000_read_phy_reg(hw, IGP02E1000_PHY_POWER_MGMT, &phy_tmp);
    e1000_write_phy_reg(hw, IGP02E1000_PHY_POWER_MGMT, phy_tmp & ~IGP02E1000_PM_SPD);
  }

  switch (hw->mac.type) {
  case e1000_82547:
  case e1000_82547_rev_2:
    pba = hw->mac.max_frame_size > 8192 ? E1000_PBA_22K : E1000_PBA_30K;
    break;
  case e1000_82571:
  case e1000_82572:
  case e1000_80003es2lan:
    pba = E1000_PBA_32K;
    break;
  case e1000_82573:
    pba = E1000_PBA_12K;
    break;
  case e1000_82574:
  case e1000_82583:
    pba = E1000_PBA_20K;
    break;
  case e1000_ich8lan:
    pba = E1000_PBA_8K;
    break;
  case e1000_ich9lan:
  case e1000_ich10lan:
    pba = E1000_PBA_10K;
    break;
  case e1000_pchlan:
  case e1000_pch2lan:
  case e1000_pch_lpt:
  case e1000_pch_spt:
  case e1000_pch_cnp:
  case e1000_pch_tgp:
  case e1000_pch_adp:
  case e1000_pch_mtp:
  case e1000_pch_ptp:
  case e1000_pch_nvp:
    pba = E1000_PBA_26K;
    break;
  case e1000_82575:
    pba = E1000_PBA_32K;
    break;
  case e1000_82576:
  case e1000_vfadapt:
    pba = E1000_READ_REG(hw, E1000_RXPBS) & E1000_RXPBS_SIZE_MASK_82576;
    break;
  case e1000_82580:
  case e1000_i350:
  case e1000_i354:
  case e1000_vfadapt_i350:
    pba = E1000_READ_REG(hw, E1000_RXPBS);
    pba = e1000_rxpbs_adjust_82580(pba);
    break;
  case e1000_i210:
  case e1000_i211:
    pba = E1000_PBA_34K;
    break;
  default:
    pba = hw->mac.max_frame_size > 8192 ? E1000_PBA_40K : E1000_PBA_48K;
  }
  if (hw->mac.type < e1000_82575)
    E1000_WRITE_REG(hw, E1000_PBA, pba);

  rx_buffer_size = (E1000_READ_REG(hw, E1000_PBA) & 0xffff) << 10;
  hw->fc.high_water = rx_buffer_size - ((hw->mac.max_frame_size + 1023) & ~1023u);
  hw->fc.low_water = hw->fc.high_water - 1500;
  hw->fc.pause_time = hw->mac.type == e1000_80003es2lan ? 0xFFFF : EM_FC_PAUSE_TIME;
  hw->fc.send_xon = true;
  hw->fc.requested_mode = e1000_fc_full;
  switch (hw->mac.type) {
  case e1000_pchlan:
    // only pause reception on pch
    hw->fc.requested_mode = e1000_fc_rx_pause;
    hw->fc.pause_time = 0xFFFF;
    hw->fc.high_water = 0x5000;
    hw->fc.low_water = 0x3000;
    hw->fc.refresh_time = 0x1000;
    break;
  case e1000_pch2lan:
  case e1000_pch_lpt:
  case e1000_pch_spt:
  case e1000_pch_cnp:
  case e1000_pch_tgp:
  case e1000_pch_adp:
  case e1000_pch_mtp:
  case e1000_pch_ptp:
  case e1000_pch_nvp:
    hw->fc.high_water = 0x5C20;
    hw->fc.low_water = 0x5048;
    hw->fc.pause_time = 0x0650;
    hw->fc.refresh_time = 0x0400;
    E1000_WRITE_REG(hw, E1000_PBA, 26);
    break;
  case e1000_82575:
  case e1000_82576:
  case e1000_82580:
  case e1000_i350:
  case e1000_i354:
  case e1000_i210:
  case e1000_i211:
    hw->fc.high_water = ((pba << 10) * 9 / 10) & 0xFFF0;
    hw->fc.low_water = hw->fc.high_water - 16;
    hw->fc.pause_time = 0x0680;
    break;
  default:
    break;
  }

  e1000_reset_hw(hw);
  if (hw->mac.type >= e1000_82544)
    E1000_WRITE_REG(hw, E1000_WUC, 0);
  em_disable_aspm(sc);
  if (e1000_init_hw(hw) < 0) {
    printf("%s: hardware initialization failed\n", sc->dev->name);
    return -1;
  }
  E1000_WRITE_REG(hw, E1000_VET, ETHERTYPE_VLAN);
  e1000_get_phy_info(hw);
  e1000_check_for_link(hw);
  return 0;
}

static void em_init_tx_unit(struct em_softc *sc) {
  struct e1000_hw *hw = &sc->hw;
  u32 tctl, tarc, tipg = 0;
  u64 bus_addr = V2P(sc->txd);

  E1000_WRITE_REG(hw, E1000_TDLEN(0), EM_NTXD * sizeof(struct e1000_tx_desc));
  E1000_WRITE_REG(hw, E1000_TDBAH(0), (u32)(bus_addr >> 32));
  E1000_WRITE_REG(hw, E1000_TDBAL(0), (u32)bus_addr);
  E1000_WRITE_REG(hw, E1000_TDT(0), 0);
  E1000_WRITE_REG(hw, E1000_TDH(0), 0);
  if (hw->mac.type >= e1000_82571 && hw->mac.type < e1000_82575) {
    u32 txdctl = 0x1f | (1 << 8) | (1 << 16) | (1 << 22) | E1000_TXDCTL_GRAN | (1 << 25);
    E1000_WRITE_REG(hw, E1000_TXDCTL(0), txdctl);
  }

  switch (hw->mac.type) {
  case e1000_82542:
    tipg = DEFAULT_82542_TIPG_IPGT;
    tipg |= DEFAULT_82542_TIPG_IPGR1 << E1000_TIPG_IPGR1_SHIFT;
    tipg |= DEFAULT_82542_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT;
    break;
  case e1000_80003es2lan:
    tipg = DEFAULT_82543_TIPG_IPGR1;
    tipg |= DEFAULT_80003ES2LAN_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT;
    break;
  default:
    if (hw->phy.media_type == e1000_media_type_fiber ||
        hw->phy.media_type == e1000_media_type_internal_serdes)
      tipg = DEFAULT_82543_TIPG_IPGT_FIBER;
    else
      tipg = DEFAULT_82543_TIPG_IPGT_COPPER;
    tipg |= DEFAULT_82543_TIPG_IPGR1 << E1000_TIPG_IPGR1_SHIFT;
    tipg |= DEFAULT_82543_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT;
    break;
  }
  E1000_WRITE_REG(hw, E1000_TIPG, tipg);
  E1000_WRITE_REG(hw, E1000_TIDV, 1);
  if (hw->mac.type >= e1000_82540)
    E1000_WRITE_REG(hw, E1000_TADV, 0);

  if (hw->mac.type == e1000_82571 || hw->mac.type == e1000_82572) {
    tarc = E1000_READ_REG(hw, E1000_TARC(0));
    E1000_WRITE_REG(hw, E1000_TARC(0), tarc | TARC_SPEED_MODE_BIT);
  } else if (hw->mac.type == e1000_80003es2lan) {
    tarc = E1000_READ_REG(hw, E1000_TARC(0));
    E1000_WRITE_REG(hw, E1000_TARC(0), tarc | 1);
    tarc = E1000_READ_REG(hw, E1000_TARC(1));
    E1000_WRITE_REG(hw, E1000_TARC(1), tarc | 1);
  } else if (hw->mac.type == e1000_82574) {
    tarc = E1000_READ_REG(hw, E1000_TARC(0));
    E1000_WRITE_REG(hw, E1000_TARC(0), tarc | TARC_ERRATA_BIT);
  }

  tctl = E1000_READ_REG(hw, E1000_TCTL) & ~E1000_TCTL_CT;
  tctl |= E1000_TCTL_PSP | E1000_TCTL_RTLC | E1000_TCTL_EN |
          (E1000_COLLISION_THRESHOLD << E1000_CT_SHIFT);
  if (hw->mac.type >= e1000_82571)
    tctl |= E1000_TCTL_MULR;
  E1000_WRITE_REG(hw, E1000_TCTL, tctl);

  if (hw->mac.type == e1000_82571 || hw->mac.type == e1000_82572 ||
      hw->mac.type == e1000_80003es2lan) {
    tarc = E1000_READ_REG(hw, E1000_TARC(1));
    E1000_WRITE_REG(hw, E1000_TARC(1), tarc & ~(1 << 28));
  } else if (hw->mac.type >= e1000_pch_spt && hw->mac.type < e1000_82575) {
    u32 reg = E1000_READ_REG(hw, E1000_IOSFPC);
    E1000_WRITE_REG(hw, E1000_IOSFPC, reg | E1000_RCTL_RDMTS_HEX);
    reg = E1000_READ_REG(hw, E1000_TARC(0));
    E1000_WRITE_REG(hw, E1000_TARC(0), reg | E1000_TARC0_CB_MULTIQ_3_REQ);
  }
  if (hw->mac.type >= e1000_82575) {
    u32 txdctl = E1000_READ_REG(hw, E1000_TXDCTL(0));
    txdctl |= E1000_TXDCTL_QUEUE_ENABLE;
    E1000_WRITE_REG(hw, E1000_TXDCTL(0), txdctl);
  }
}

static void em_init_rx_unit(struct em_softc *sc) {
  struct e1000_hw *hw = &sc->hw;
  u64 bus_addr = V2P(sc->rxd);
  u32 rctl, rxcsum;
  uint i;

  rctl = E1000_READ_REG(hw, E1000_RCTL);
  if (hw->mac.type != e1000_82574 && hw->mac.type != e1000_82583)
    E1000_WRITE_REG(hw, E1000_RCTL, rctl & ~E1000_RCTL_EN);
  if (hw->mac.type >= e1000_82540 && hw->mac.type < e1000_82575)
    E1000_WRITE_REG(hw, E1000_ITR, 0);
  if (hw->mac.type == e1000_82574) {
    u32 rfctl = E1000_READ_REG(hw, E1000_RFCTL);
    E1000_WRITE_REG(hw, E1000_RFCTL, rfctl | E1000_RFCTL_ACK_DIS);
  }
  rxcsum = E1000_READ_REG(hw, E1000_RXCSUM) & ~(E1000_RXCSUM_IPOFL | E1000_RXCSUM_TUOFL);
  E1000_WRITE_REG(hw, E1000_RXCSUM, rxcsum);

  for (i = 0; i < EM_NRXD; i++) {
    memset(&sc->rxd[i], 0, sizeof(sc->rxd[i]));
    sc->rxd[i].buffer_addr = V2P(sc->rxbuf[i]);
  }
  sc->rx_cons = 0;
  if (hw->mac.type >= e1000_82575) {
    u32 rxdctl = E1000_READ_REG(hw, E1000_RXDCTL(0));
    E1000_WRITE_REG(hw, E1000_RXDCTL(0), rxdctl & ~E1000_RXDCTL_QUEUE_ENABLE);
    E1000_WRITE_FLUSH(hw);
  }
  E1000_WRITE_REG(hw, E1000_RDLEN(0), EM_NRXD * sizeof(struct e1000_rx_desc));
  E1000_WRITE_REG(hw, E1000_RDBAH(0), (u32)(bus_addr >> 32));
  E1000_WRITE_REG(hw, E1000_RDBAL(0), (u32)bus_addr);
  E1000_WRITE_REG(hw, E1000_RDH(0), 0);
  E1000_WRITE_REG(hw, E1000_RDT(0), EM_NRXD - 1);

  if (hw->mac.type >= e1000_82575) {
    u32 srrctl = (EM_BUFSIZE >> E1000_SRRCTL_BSIZEPKT_SHIFT) | E1000_SRRCTL_DESCTYPE_ADV_ONEBUF;
    u32 rxdctl;

    E1000_WRITE_REG(hw, E1000_SRRCTL(0), srrctl);
    rxdctl = E1000_READ_REG(hw, E1000_RXDCTL(0)) & 0xFFF00000;
    rxdctl |= 8 | (8 << 8) | (1 << 16) | E1000_RXDCTL_QUEUE_ENABLE;
    E1000_WRITE_REG(hw, E1000_RXDCTL(0), rxdctl);
    for (i = 0; i < 1000 && !(E1000_READ_REG(hw, E1000_RXDCTL(0)) & E1000_RXDCTL_QUEUE_ENABLE); i++)
      delay(10);
    E1000_WRITE_REG(hw, E1000_RDT(0), EM_NRXD - 1);
  } else if (hw->mac.type >= e1000_pch2lan)
    e1000_lv_jumbo_workaround_ich8lan(hw, false);

  rctl &= ~(3 << E1000_RCTL_MO_SHIFT);
  rctl |= E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_LBM_NO | E1000_RCTL_RDMTS_HALF |
          (hw->mac.mc_filter_type << E1000_RCTL_MO_SHIFT);
  rctl &= ~(E1000_RCTL_VFE | E1000_RCTL_LPE | E1000_RCTL_BSEX);
  if (e1000_tbi_sbp_enabled_82543(hw))
    rctl |= E1000_RCTL_SBP;
  else
    rctl &= ~E1000_RCTL_SBP;
  rctl |= E1000_RCTL_SZ_2048 | E1000_RCTL_SECRC;
  E1000_WRITE_REG(hw, E1000_RCTL, rctl);
}

static void em_stop(struct em_softc *sc) {
  struct e1000_hw *hw = &sc->hw;
  uint i;

  mtx_enter(&sc->lock);
  sc->running = 0;
  callout_stop_async(&sc->timer);
  mtx_leave(&sc->lock);
  if (hw->mac.type == e1000_82574)
    E1000_WRITE_REG(hw, EM_EIAC, 0);
  E1000_WRITE_REG(hw, E1000_IMC, 0xffffffff);
  e1000_reset_hw(hw);
  if (hw->mac.type >= e1000_82544)
    E1000_WRITE_REG(hw, E1000_WUC, 0);
  for (i = 0; i < EM_NTXD; i++)
    sc->txd[i].upper.data = 0;
  sc->tx_prod = sc->tx_cons = 0;
  sc->tx_free = EM_NTXD;
}

static int em_init(struct em_softc *sc) {
  struct e1000_hw *hw = &sc->hw;
  u32 ims = IMS_ENABLE_MASK;

  em_stop(sc);
  e1000_rar_set(hw, hw->mac.addr, 0);
  if (hw->mac.type == e1000_82571) {
    e1000_set_laa_state_82571(hw, true);
    e1000_rar_set(hw, hw->mac.addr, E1000_RAR_ENTRIES - 1);
  }
  if (em_reset(sc) < 0)
    return -1;
  em_update_link_status(sc);
  E1000_WRITE_REG(hw, E1000_VET, ETHERTYPE_VLAN);
  em_get_mgmt(sc);
  em_init_tx_unit(sc);
  e1000_update_mc_addr_list(hw, 0, 0);
  em_init_rx_unit(sc);
  E1000_WRITE_REG(hw, E1000_RCTL, E1000_READ_REG(hw, E1000_RCTL) & ~(E1000_RCTL_UPE | E1000_RCTL_MPE));
  e1000_clear_hw_cntrs_base_generic(hw);
  if (hw->mac.type == e1000_82574) {
    u32 tmp = E1000_READ_REG(hw, E1000_CTRL_EXT);
    E1000_WRITE_REG(hw, E1000_CTRL_EXT, tmp | E1000_CTRL_EXT_PBA_CLR);
    E1000_WRITE_REG(hw, E1000_IVAR, 0x800A0908);
  }
  E1000_WRITE_REG(hw, E1000_IMS, ims);
  if (sc->has_mgmt && sc->has_amt && hw->mac.type >= e1000_82571)
    em_get_hw_control(sc);
  mtx_enter(&sc->lock);
  sc->running = 1;
  callout_reset(&sc->timer, HZ, em_local_timer, sc);
  mtx_leave(&sc->lock);
  return 0;
}

static void em_rings_free(struct em_softc *sc) {
  uint i;

  for (i = 0; i < EM_NTXD; i += 2)
    if (sc->txbuf[i] != 0)
      page_free(sc->txbuf[i]);
  for (i = 0; i < EM_NRXD; i += 2)
    if (sc->rxbuf[i] != 0)
      page_free(sc->rxbuf[i]);
  if (sc->txd != 0)
    page_free((char *)sc->txd);
  if (sc->rxd != 0)
    page_free((char *)sc->rxd);
}

static int em_rings_alloc(struct em_softc *sc) {
  uint i;

  if ((sc->txd = (struct e1000_tx_desc *)page_alloc()) == 0 ||
      (sc->rxd = (struct e1000_rx_desc *)page_alloc()) == 0)
    return -1;
  memset(sc->txd, 0, PGSIZE);
  memset(sc->rxd, 0, PGSIZE);
  for (i = 0; i < EM_NTXD; i += 2) {
    char *p = page_alloc();
    if (p == 0)
      return -1;
    sc->txbuf[i] = p;
    sc->txbuf[i + 1] = p + EM_BUFSIZE;
  }
  for (i = 0; i < EM_NRXD; i += 2) {
    char *p = page_alloc();
    if (p == 0)
      return -1;
    sc->rxbuf[i] = p;
    sc->rxbuf[i + 1] = p + EM_BUFSIZE;
  }
  return 0;
}

static int em_attach(struct device *dev) {
  struct em_softc *sc;
  struct e1000_hw *hw;
  struct pci_device *p = &dev->pci;
  u16 eeprom_data = 0, apme_mask;
  const char *name = em_name(p->device_id);
  int b;

  _Static_assert(EM_NTXD * sizeof(struct e1000_tx_desc) <= PGSIZE, "tx ring");
  _Static_assert(EM_NRXD * sizeof(struct e1000_rx_desc) <= PGSIZE, "rx ring");
  if (dev->mmio_vaddr == 0) {
    printf("%s: registers not mapped\n", dev->name);
    return -1;
  }
  sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
  hw = &sc->hw;
  mtx_init(&sc->lock, "em");
  callout_init(&sc->timer);
  sc->dev = dev;
  sc->osdep.pci = p;
  sc->osdep.mem = (volatile uchar *)dev->mmio_vaddr;
  hw->back = &sc->osdep;
  hw->hw_addr = (u8 *)dev->mmio_vaddr;
  hw->vendor_id = p->vendor_id;
  hw->device_id = p->device_id;
  hw->revision_id = pci_read8(p->bus, p->device, p->function, 0x08);
  hw->subsystem_vendor_id = pci_read16(p->bus, p->device, p->function, 0x2c);
  hw->subsystem_device_id = pci_read16(p->bus, p->device, p->function, 0x2e);
  if (e1000_set_mac_type(hw) != E1000_SUCCESS) {
    printf("%s: unknown mac\n", sc->dev->name);
    goto fail;
  }
  for (b = 1; b < 6; b++)
    if (p->bar_is_io[b] && p->bar[b] != 0) {
      sc->osdep.io = (ushort)p->bar[b];
      hw->io_base = sc->osdep.io;
      break;
    }
  switch (hw->mac.type) {
  case e1000_ich8lan:
  case e1000_ich9lan:
  case e1000_ich10lan:
  case e1000_pchlan:
  case e1000_pch2lan:
  case e1000_pch_lpt:
    if (p->bar[1] == 0 || p->bar_is_io[1] ||
        (sc->osdep.flash = (volatile uchar *)ioremap(p->bar[1], p->bar_size[1])) == 0) {
      printf("%s: the flash is not mapped\n", sc->dev->name);
      goto fail;
    }
    hw->flash_address = (u8 *)sc->osdep.flash;
    break;
  default:
    if (hw->mac.type >= e1000_pch_spt && hw->mac.type < e1000_82575)
      sc->osdep.flash = sc->osdep.mem + E1000_FLASH_BASE_ADDR;
    break;
  }

  if (e1000_setup_init_funcs(hw, true) != E1000_SUCCESS) {
    printf("%s: the shared code refused this mac\n", sc->dev->name);
    goto fail;
  }
  e1000_get_bus_info(hw);
  hw->mac.autoneg = 1;
  hw->phy.autoneg_wait_to_complete = false;
  hw->phy.autoneg_advertised = ADVERTISE_10_HALF | ADVERTISE_10_FULL | ADVERTISE_100_HALF |
                               ADVERTISE_100_FULL | ADVERTISE_1000_FULL;
  e1000_init_script_state_82541(hw, true);
  e1000_set_tbi_compatibility_82543(hw, true);
  if (hw->phy.media_type == e1000_media_type_copper) {
    hw->phy.mdix = AUTO_ALL_MODES;
    hw->phy.disable_polarity_correction = false;
    hw->phy.ms_type = e1000_ms_hw_default;
  }
  hw->mac.max_frame_size = ETHERMTU + 14 + 4;
  sc->igb = hw->mac.type >= e1000_82575;
  hw->mac.report_tx_early = 1;
  hw->dev_spec.ich8lan.eee_disable = true;
  if (em_rings_alloc(sc) < 0) {
    printf("%s: no memory for the rings\n", sc->dev->name);
    goto fail;
  }
  if (e1000_check_reset_block(hw))
    printf("%s: phy reset is blocked by a SOL/IDER session\n", sc->dev->name);

  e1000_reset_hw(hw);
  if (e1000_validate_nvm_checksum(hw) < 0 && e1000_validate_nvm_checksum(hw) < 0)
    printf("%s: the eeprom checksum is not valid\n", sc->dev->name);
  if (e1000_read_mac_addr(hw) < 0) {
    printf("%s: eeprom read error while reading the mac address\n", sc->dev->name);
    goto fail;
  }
  if ((hw->mac.addr[0] & 1) || memcmp(hw->mac.addr, "\0\0\0\0\0\0", 6) == 0) {
    printf("%s: invalid mac address\n", sc->dev->name);
    goto fail;
  }
  e1000_disable_ulp_lpt_lp(hw, true);
  E1000_WRITE_REG(hw, E1000_IMC, 0xffffffff);
  sc->has_mgmt = e1000_enable_mng_pass_thru(hw);
  apme_mask = 0x0400;
  switch (hw->mac.type) {
  case e1000_82542:
  case e1000_82543:
    break;
  case e1000_82573:
  case e1000_82583:
    sc->has_amt = 1;
    /* FALLTHROUGH */
  case e1000_82546:
  case e1000_82546_rev_3:
  case e1000_82571:
  case e1000_82572:
  case e1000_80003es2lan:
    e1000_read_nvm(hw, hw->bus.func == 1 ? NVM_INIT_CONTROL3_PORT_B : NVM_INIT_CONTROL3_PORT_A,
                   1, &eeprom_data);
    break;
  case e1000_ich8lan:
  case e1000_ich9lan:
  case e1000_ich10lan:
  case e1000_pchlan:
  case e1000_pch2lan:
  case e1000_pch_lpt:
  case e1000_pch_spt:
  case e1000_pch_cnp:
  case e1000_pch_tgp:
  case e1000_pch_adp:
  case e1000_pch_mtp:
  case e1000_pch_ptp:
  case e1000_pch_nvp:
    apme_mask = E1000_WUC_APME;
    sc->has_amt = 1;
    eeprom_data = (u16)E1000_READ_REG(hw, E1000_WUC);
    break;
  default:
    e1000_read_nvm(hw, NVM_INIT_CONTROL3_PORT_A, 1, &eeprom_data);
    break;
  }
  (void)apme_mask;

  printf("%s: %s\n", dev->name, name);
  if (em_reset(sc) < 0 && em_reset(sc) < 0) {
    printf("%s: unable to reset the hardware\n", dev->name);
    goto fail;
  }
  hw->mac.get_link_status = 1;
  if (sc->has_mgmt && !sc->has_amt && hw->mac.type >= e1000_82571)
    em_get_hw_control(sc);
  if (driver_setup_intr_best(dev, PCI_INTR_MSI) < 0) {
    printf("%s: no interrupt\n", dev->name);
    goto fail;
  }
  dev->driver_data = sc;
  if (em_init(sc) < 0) {
    dev->driver_data = 0;
    goto fail;
  }
  strlcpy(sc->ifnet.if_xname, dev->name, sizeof(sc->ifnet.if_xname));
  memmove(sc->ifnet.if_lladdr, hw->mac.addr, ETHER_ADDR_LEN);
  sc->ifnet.if_mtu = ETHERMTU;
  sc->ifnet.if_transmit = em_transmit;
  sc->ifnet.if_softc = sc;
  if_attach(&sc->ifnet);
  return 0;

fail:
  em_rel_hw_control(sc);
  em_rings_free(sc);
  free(sc, M_DEVBUF);
  return -1;
}

static int em_detach(struct device *dev) {
  struct em_softc *sc = dev->driver_data;

  if (sc->running)
    em_stop(sc);
  em_rel_mgmt(sc);
  em_rel_hw_control(sc);
  return 0;
}

static void em_shutdown(struct device *dev) {
  em_detach(dev);
}

// SLEEP
static int em_suspend(struct device *dev) {
  em_detach(dev);
  return 0;
}

static int em_resume(struct device *dev) {
  return em_init(dev->driver_data);
}

static struct driver em_driver = {
    .name = "em",
    .bus_type = BUS_TYPE_PCI,
    .dev_type = DEVICE_TYPE_NET,
    .probe = em_probe,
    .attach = em_attach,
    .detach = em_detach,
    .intr = em_intr,
    .shutdown = em_shutdown,
    .suspend = em_suspend,
    .resume = em_resume,
};

void e1000_driver_init(void) { driver_register(&em_driver); }
