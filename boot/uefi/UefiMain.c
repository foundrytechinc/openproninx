#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

// It's not a working. 
EFI_STATUS
EFIAPI
UefiMain(
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  Print(L"\r\nOpenProninx UEFI bootloader starting...\r\n");


  return EFI_SUCCESS;
}
