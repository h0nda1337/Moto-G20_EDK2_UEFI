#include <Uefi.h>
#include "../../Library/MotoG20TranslationTableLib/MotoG20TranslationTableLib.h"

EFI_STATUS
EFIAPI
Mmu2aCompileProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  MOTO_G20_TRANSLATION_LOOKUP  Lookup;

  (VOID)ImageHandle;
  (VOID)SystemTable;
  (VOID)Lookup;
  return EFI_SUCCESS;
}
