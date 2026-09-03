#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

/**
 * Simple UEFI entry point that prints a greeting and exits.
 */
EFI_STATUS
EFIAPI
UefiMain(
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  // Print a simple message to the UEFI console
  Print(L"\r\nOpenProninx UEFI bootloader starting...\r\n");

  // Here you would normally load the kernel ELF image, set up paging,
  // and jump to the kernel entry point. For a minimal stub we just exit.
  return EFI_SUCCESS;
}
