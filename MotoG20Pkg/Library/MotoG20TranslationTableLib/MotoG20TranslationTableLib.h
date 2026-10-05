/** @file
  Build-only translation table construction and software lookup for Moto G20.
**/

#pragma once

#include <Uefi/UefiBaseType.h>
#include <Library/ArmLib.h>

typedef struct {
  BOOLEAN  Mapped;
  UINTN    Level;
  UINT64   Descriptor;
  UINT64   PhysicalAddress;
} MOTO_G20_TRANSLATION_LOOKUP;

EFI_STATUS
EFIAPI
MotoG20BuildTranslationTables (
  IN  CONST ARM_MEMORY_REGION_DESCRIPTOR  *MemoryTable,
  OUT VOID                                **TranslationTableBase,
  OUT UINTN                               *TranslationTableSize,
  OUT UINTN                               *AllocatedTablePages
  );

EFI_STATUS
EFIAPI
MotoG20LookupTranslation (
  IN  CONST VOID                         *TranslationTableBase,
  IN  UINT64                              VirtualAddress,
  OUT MOTO_G20_TRANSLATION_LOOKUP        *Result
  );
