/** @file
  Exact invalid-to-valid extension of the frozen MMU3C2I EL2 page tables.
**/

#include <Base.h>
#include <AArch64/AArch64Mmu.h>
#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>

#include "MotoG20FoundationBootstrap.h"

#define FOUNDATION_TEXT  __attribute__((section (".foundation.text")))

#define MOTO_G20_L0_INDEX_80000000        0U
#define MOTO_G20_L1_INDEX_80000000        2U
#define MOTO_G20_L2_INDEX_82A00000        21U
#define MOTO_G20_L2_FIRST_BLOCK_INDEX     22U
#define MOTO_G20_L2_LAST_BLOCK_INDEX      57U
#define MOTO_G20_L2_NEW_TABLE_INDEX       58U
#define MOTO_G20_EXISTING_L3_FIRST_INDEX  11U
#define MOTO_G20_EXISTING_L3_LAST_INDEX   511U
#define MOTO_G20_NEW_L3_LAST_INDEX        255U

STATIC
UINT64
FOUNDATION_TEXT
MotoG20ReadSctlrEl2 (
  VOID
  )
{
  UINT64  Value;

  __asm__ volatile ("mrs %0, SCTLR_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
FOUNDATION_TEXT
MotoG20ReadCurrentEl (
  VOID
  )
{
  UINT64  Value;

  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (Value));
  return Value;
}

STATIC
UINT64
FOUNDATION_TEXT
MotoG20TableAddress (
  IN UINT64  Descriptor
  )
{
  return Descriptor & TT_ADDRESS_MASK_DESCRIPTION_TABLE;
}

STATIC
BOOLEAN
FOUNDATION_TEXT
MotoG20IsTableTo (
  IN UINT64  Descriptor,
  IN UINT64  ExpectedAddress
  )
{
  return (BOOLEAN)(
                    ((Descriptor & TT_TYPE_MASK) == TT_TYPE_TABLE_ENTRY) &&
                    (MotoG20TableAddress (Descriptor) == ExpectedAddress)
                    );
}

EFI_STATUS
EFIAPI
FOUNDATION_TEXT
MotoG20FoundationAppendHighMap (
  OUT MOTO_G20_FOUNDATION_MAP_RESULT  *Result
  )
{
  volatile UINT64  *Root;
  volatile UINT64  *L1;
  volatile UINT64  *L2;
  volatile UINT64  *ExistingL3;
  volatile UINT64  *NewL3;
  UINT64           Attributes;
  UINT64           Expected;
  UINT64           Sctlr;
  UINT64           Address;
  UINTN            Index;

  if (Result == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  SetMem (Result, sizeof (*Result), 0);
  Sctlr = MotoG20ReadSctlrEl2 ();
  if ((MotoG20ReadCurrentEl () != 8ULL) ||
      ((Sctlr & (BIT0 | BIT2 | BIT12)) != BIT0) ||
      ((UINT64)(UINTN)ArmGetTTBR0BaseAddress () != MOTO_G20_FOUNDATION_TTBR0)) {
    return EFI_SECURITY_VIOLATION;
  }

  Root       = (volatile UINT64 *)(UINTN)MOTO_G20_FOUNDATION_TTBR0;
  L1         = (volatile UINT64 *)(UINTN)MOTO_G20_FOUNDATION_L1;
  L2         = (volatile UINT64 *)(UINTN)MOTO_G20_FOUNDATION_L2_80000000;
  ExistingL3 = (volatile UINT64 *)(UINTN)MOTO_G20_FOUNDATION_EXISTING_L3_82A00000;
  NewL3      = (volatile UINT64 *)(UINTN)MOTO_G20_FOUNDATION_NEW_L3_87400000;

  if (!MotoG20IsTableTo (Root[MOTO_G20_L0_INDEX_80000000], MOTO_G20_FOUNDATION_L1) ||
      !MotoG20IsTableTo (L1[MOTO_G20_L1_INDEX_80000000], MOTO_G20_FOUNDATION_L2_80000000) ||
      !MotoG20IsTableTo (L2[MOTO_G20_L2_INDEX_82A00000], MOTO_G20_FOUNDATION_EXISTING_L3_82A00000)) {
    return EFI_COMPROMISED_DATA;
  }

  Attributes = L2[MOTO_G20_L2_INDEX_82A00000 - 1U] & TT_ATTRIBUTES_MASK;
  Expected   = TT_ATTR_INDX_MEMORY_WRITE_BACK | TT_SH_INNER_SHAREABLE | TT_AF;
  if ((Attributes != Expected) ||
      ((L2[MOTO_G20_L2_INDEX_82A00000 - 1U] & TT_TYPE_MASK) != TT_TYPE_BLOCK_ENTRY) ||
      ((ExistingL3[MOTO_G20_EXISTING_L3_FIRST_INDEX - 1U] & TT_TYPE_MASK) != TT_TYPE_BLOCK_ENTRY_LEVEL3) ||
      ((ExistingL3[MOTO_G20_EXISTING_L3_FIRST_INDEX - 1U] & TT_ATTRIBUTES_MASK) != Attributes)) {
    return EFI_COMPROMISED_DATA;
  }

  for (Index = MOTO_G20_EXISTING_L3_FIRST_INDEX; Index <= MOTO_G20_EXISTING_L3_LAST_INDEX; Index++) {
    if (ExistingL3[Index] != 0) {
      return EFI_ALREADY_STARTED;
    }
  }

  for (Index = MOTO_G20_L2_FIRST_BLOCK_INDEX; Index <= MOTO_G20_L2_NEW_TABLE_INDEX; Index++) {
    if (L2[Index] != 0) {
      return EFI_ALREADY_STARTED;
    }
  }

  for (Index = 0; Index < TT_ENTRY_COUNT; Index++) {
    if (NewL3[Index] != 0) {
      return EFI_ALREADY_STARTED;
    }
  }

  // Preserve the proven 794-write map envelope.  The 16-byte-aligned FV
  // starts exactly at LOW_RESERVED_END and the endpoint/counts stay frozen.
  Address = MOTO_G20_FOUNDATION_LOW_RESERVED_END;
  for (Index = MOTO_G20_EXISTING_L3_FIRST_INDEX; Index <= MOTO_G20_EXISTING_L3_LAST_INDEX; Index++) {
    ExistingL3[Index] = (Address & TT_ADDRESS_MASK_BLOCK_ENTRY) | Attributes | TT_TYPE_BLOCK_ENTRY_LEVEL3;
    Address += SIZE_4KB;
    Result->ExistingL3PageWrites++;
  }

  for (Index = MOTO_G20_L2_FIRST_BLOCK_INDEX; Index <= MOTO_G20_L2_LAST_BLOCK_INDEX; Index++) {
    L2[Index] = (Address & TT_ADDRESS_MASK_BLOCK_ENTRY) | Attributes | TT_TYPE_BLOCK_ENTRY;
    Address += SIZE_2MB;
    Result->L2BlockWrites++;
  }

  for (Index = 0; Index <= MOTO_G20_NEW_L3_LAST_INDEX; Index++) {
    NewL3[Index] = (Address & TT_ADDRESS_MASK_BLOCK_ENTRY) | Attributes | TT_TYPE_BLOCK_ENTRY_LEVEL3;
    Address += SIZE_4KB;
    Result->NewL3PageWrites++;
  }

  if (Address != MOTO_G20_FOUNDATION_DXE_MEMORY_END) {
    return EFI_COMPROMISED_DATA;
  }

  ArmDataSynchronizationBarrier ();
  L2[MOTO_G20_L2_NEW_TABLE_INDEX] =
    (MOTO_G20_FOUNDATION_NEW_L3_87400000 & TT_ADDRESS_MASK_DESCRIPTION_TABLE) |
    TT_TYPE_TABLE_ENTRY;
  Result->ParentTableWrites = 1;
  ArmDataSynchronizationBarrier ();
  ArmInvalidateTlb ();
  ArmDataSynchronizationBarrier ();
  ArmInstructionSynchronizationBarrier ();

  Result->TotalDescriptorWrites =
    Result->ExistingL3PageWrites +
    Result->L2BlockWrites +
    Result->NewL3PageWrites +
    Result->ParentTableWrites;
  if ((Result->ExistingL3PageWrites != 501U) ||
      (Result->L2BlockWrites != 36U) ||
      (Result->NewL3PageWrites != 256U) ||
      (Result->ParentTableWrites != 1U) ||
      (Result->TotalDescriptorWrites != 794U)) {
    return EFI_COMPROMISED_DATA;
  }

  return EFI_SUCCESS;
}
