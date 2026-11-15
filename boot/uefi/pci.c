// OPTION ROMS
// copied out before ExitBootServices, after that they are gone
#include "boot/uefi/uefi.h"

#define EFI_BY_PROTOCOL 2

static const struct efi_guid pci_io_guid = EFI_PCI_IO_GUID;

static void keep(struct fnu_bootinfo *bi, struct efi_pci_io *io, uint32 id,
                 uint32 class) {
  struct fnu_pcirom *r = &bi->pcirom[bi->pciroms];
  efi_uintn seg, bus, dev, fn;
  efi_phys at;

  if (io->get_location(io, &seg, &bus, &dev, &fn) != EFI_SUCCESS)
    return;
  if ((at = boot_alloc(io->rom_size)) == 0)
    return;
  memcpy((void *)(uintptr_t)at, io->rom_image, io->rom_size);
  r->base = at;
  r->size = (uint32)io->rom_size;
  r->vendor = (uint16)id;
  r->device = (uint16)(id >> 16);
  r->class = class >> 8;
  r->segment = (uint16)seg;
  r->bus = (uint8)bus;
  r->slot = (uint8)dev;
  r->function = (uint8)fn;
  bi->pciroms++;
}

void pci_roms(struct fnu_bootinfo *bi) {
  efi_handle *h;
  efi_uintn n, i;
  struct efi_pci_io *io;
  uint32 id, class;
  int pass;

  if (BS->locate_handle_buffer(EFI_BY_PROTOCOL, &pci_io_guid, 0, &n, &h) !=
      EFI_SUCCESS)
    return;
  for (pass = 0; pass < 2; pass++)
    for (i = 0; i < n && bi->pciroms < FNU_PCIROM_MAX; i++) {
      if (BS->handle_protocol(h[i], &pci_io_guid, (void **)&io) != EFI_SUCCESS)
        continue;
      if (io->rom_image == 0 || io->rom_size == 0 ||
          io->rom_size > 0x1000000)
        continue;
      if (io->pci.read(io, EFI_PCI_IO_WIDTH_UINT32, 0x00, 1, &id) !=
              EFI_SUCCESS ||
          io->pci.read(io, EFI_PCI_IO_WIDTH_UINT32, 0x08, 1, &class) !=
              EFI_SUCCESS)
        continue;
      if ((class >> 24 == 0x03) != (pass == 0))
        continue;
      keep(bi, io, id, class);
    }
  BS->free_pool(h);
}
