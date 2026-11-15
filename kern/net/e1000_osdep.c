#include "e1000_api.h"

int e1000_debug;

static int pcie_cap(struct e1000_hw *hw) {
  return pci_find_capability(E1000_OSDEP(hw)->pci, 0x10);
}

void e1000_write_pci_cfg(struct e1000_hw *hw, u32 reg, u16 *value) {
  const struct pci_device *p = E1000_OSDEP(hw)->pci;
  pci_write16(p->bus, p->device, p->function, reg, *value);
}

void e1000_read_pci_cfg(struct e1000_hw *hw, u32 reg, u16 *value) {
  const struct pci_device *p = E1000_OSDEP(hw)->pci;
  *value = pci_read16(p->bus, p->device, p->function, reg);
}

void e1000_pci_set_mwi(struct e1000_hw *hw) {
  u16 v = hw->bus.pci_cmd_word | CMD_MEM_WRT_INVALIDATE;
  e1000_write_pci_cfg(hw, PCI_COMMAND_REGISTER, &v);
}

void e1000_pci_clear_mwi(struct e1000_hw *hw) {
  u16 v = hw->bus.pci_cmd_word & ~CMD_MEM_WRT_INVALIDATE;
  e1000_write_pci_cfg(hw, PCI_COMMAND_REGISTER, &v);
}

s32 e1000_read_pcie_cap_reg(struct e1000_hw *hw, u32 reg, u16 *value) {
  int cap = pcie_cap(hw);

  if (cap < 0)
    return -E1000_ERR_CONFIG;
  e1000_read_pci_cfg(hw, (u32)cap + reg, value);
  return E1000_SUCCESS;
}

s32 e1000_write_pcie_cap_reg(struct e1000_hw *hw, u32 reg, u16 *value) {
  int cap = pcie_cap(hw);

  if (cap < 0)
    return -E1000_ERR_CONFIG;
  e1000_write_pci_cfg(hw, (u32)cap + reg, value);
  return E1000_SUCCESS;
}
