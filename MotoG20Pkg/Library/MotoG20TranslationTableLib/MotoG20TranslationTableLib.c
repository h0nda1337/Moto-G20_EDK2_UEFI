/** @file

  Adapted algorithmically from upstream EDK2 ArmMmuLib:
    edk2/UefiCpuPkg/Library/ArmMmuLib/AArch64/ArmMmuLibCore.c
  Frozen upstream SHA256:
    BF3BE06245F00C6D1B24B94EB0068F607387780966297070836799FEA2AA3521

  Purpose: MMU2A build-only translation tables for the Moto G20.
  This library performs no live-table updates and no CPU translation-state,
  cache, TLB, or system-register manipulation.  All tables are ordinary RAM
  objects and all descriptor replacement is a normal C store.

**/

#include <Base.h>
#include <Uefi/UefiBaseType.h>
#include <AArch64/AArch64Mmu.h>
#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

#include "MotoG20TranslationTableLib.h"

#define MOTO_G20_PAGE_SHIFT             12U
#define MOTO_G20_PAGE_SIZE              (1ULL << MOTO_G20_PAGE_SHIFT)
#define MOTO_G20_PAGE_MASK              (MOTO_G20_PAGE_SIZE - 1)
#define MOTO_G20_PHYSICAL_ADDRESS_BITS  40U
#define MOTO_G20_VIRTUAL_ADDRESS_BITS   40U
#define MOTO_G20_PHYSICAL_ADDRESS_LIMIT (1ULL << MOTO_G20_PHYSICAL_ADDRESS_BITS)
#define MOTO_G20_VIRTUAL_ADDRESS_LIMIT  (1ULL << MOTO_G20_VIRTUAL_ADDRESS_BITS)
#define MOTO_G20_ADDRESS_LIMIT          MOTO_G20_VIRTUAL_ADDRESS_LIMIT
#define MOTO_G20_T0SZ                   24U
#define MOTO_G20_ROOT_LEVEL             0U
#define MOTO_G20_ROOT_ENTRY_COUNT       2U
#define MOTO_G20_LPA2                   FALSE
#define MOTO_G20_CCA_PROTECTION         0ULL
#define MOTO_G20_TABLE_ENTRIES          TT_ENTRY_COUNT
#define MOTO_G20_LOGICAL_ROOT_SIZE      (MOTO_G20_ROOT_ENTRY_COUNT * sizeof (UINT64))

#define MOTO_G20_LEVEL_COUNT            4U
#define MOTO_G20_BITS_PER_LEVEL         9U

typedef struct {
  UINTN  AllocatedPages;
} MOTO_G20_BUILD_CONTEXT;

STATIC
UINTN
MotoG20LevelShift (
  IN UINTN  Level
  )
{
  return TT_ADDRESS_OFFSET_AT_LEVEL (Level);
}

STATIC
UINT64
MotoG20LevelSize (
  IN UINTN  Level
  )
{
  return 1ULL << MotoG20LevelShift (Level);
}

STATIC
UINT64
MotoG20TableAddress (
  IN UINT64  Descriptor
  )
{
  return Descriptor & TT_ADDRESS_MASK_DESCRIPTION_TABLE;
}

STATIC
UINT64
MotoG20DescriptorAddress (
  IN UINT64  Descriptor
  )
{
  return Descriptor & TT_ADDRESS_MASK_BLOCK_ENTRY;
}

STATIC
UINT64
MotoG20SetOutputAddress (
  IN UINT64  Descriptor,
  IN UINT64  Address
  )
{
  Descriptor &= ~TT_ADDRESS_MASK_BLOCK_ENTRY;
  Descriptor |= Address & TT_ADDRESS_MASK_BLOCK_ENTRY;
  return Descriptor;
}

STATIC
BOOLEAN
MotoG20IsTableEntry (
  IN UINT64  Descriptor,
  IN UINTN   Level
  )
{
  return (Level < 3) && ((Descriptor & TT_TYPE_MASK) == TT_TYPE_TABLE_ENTRY);
}

STATIC
BOOLEAN
MotoG20IsBlockEntry (
  IN UINT64  Descriptor,
  IN UINTN   Level
  )
{
  if (Level == 3) {
    return (Descriptor & TT_TYPE_MASK) == TT_TYPE_BLOCK_ENTRY_LEVEL3;
  }

  return (Descriptor & TT_TYPE_MASK) == TT_TYPE_BLOCK_ENTRY;
}

STATIC
EFI_STATUS
MotoG20AttributesToDescriptor (
  IN  ARM_MEMORY_REGION_ATTRIBUTES  Attributes,
  OUT UINT64                        *DescriptorAttributes
  )
{
  if (DescriptorAttributes == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  switch (Attributes) {
    case ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK:
      *DescriptorAttributes = TT_ATTR_INDX_MEMORY_WRITE_BACK | TT_SH_INNER_SHAREABLE;
      return EFI_SUCCESS;

    case ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_RO:
      *DescriptorAttributes = TT_ATTR_INDX_MEMORY_WRITE_BACK |
                              TT_SH_INNER_SHAREABLE |
                              TT_AP_NO_RO;
      return EFI_SUCCESS;

    case ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP:
      *DescriptorAttributes = TT_ATTR_INDX_MEMORY_WRITE_BACK |
                              TT_SH_INNER_SHAREABLE |
                              TT_XN_MASK;
      return EFI_SUCCESS;

    case ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_NONSHAREABLE:
      *DescriptorAttributes = TT_ATTR_INDX_MEMORY_WRITE_BACK;
      return EFI_SUCCESS;

    case ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH:
      *DescriptorAttributes = TT_ATTR_INDX_MEMORY_WRITE_THROUGH | TT_SH_INNER_SHAREABLE;
      return EFI_SUCCESS;

    case ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED:
      *DescriptorAttributes = TT_ATTR_INDX_MEMORY_NON_CACHEABLE;
      return EFI_SUCCESS;

    case ARM_MEMORY_REGION_ATTRIBUTE_DEVICE:
      *DescriptorAttributes = TT_ATTR_INDX_DEVICE_MEMORY | TT_XN_MASK;
      return EFI_SUCCESS;

    default:
      return EFI_INVALID_PARAMETER;
  }
}

STATIC
EFI_STATUS
MotoG20AllocateTable (
  IN  OUT MOTO_G20_BUILD_CONTEXT  *Context,
  OUT UINT64                      **Table
  )
{
  VOID  *Memory;

  if ((Context == NULL) || (Table == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  // Construction is monotonic: allocated pages are never speculatively
  // released, because the local allocator does not reclaim released pages.
  Memory = AllocatePages (1);
  if (Memory == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (Memory, EFI_PAGE_SIZE);
  Context->AllocatedPages++;
  *Table = (UINT64 *)Memory;
  return EFI_SUCCESS;
}

STATIC
VOID
MotoG20PopulateSplitTable (
  OUT UINT64  *Table,
  IN  UINTN   ChildLevel,
  IN  UINT64  BlockDescriptor
  )
{
  UINTN   Index;
  UINT64  ChildSize;
  UINT64  Address;
  UINT64  ChildDescriptor;

  ChildSize = MotoG20LevelSize (ChildLevel);
  Address   = MotoG20DescriptorAddress (BlockDescriptor);
  for (Index = 0; Index < MOTO_G20_TABLE_ENTRIES; Index++) {
    ChildDescriptor = BlockDescriptor & TT_ATTRIBUTES_MASK;
    ChildDescriptor = MotoG20SetOutputAddress (
                        ChildDescriptor,
                        Address + (Index * ChildSize)
                        );
    ChildDescriptor |= (ChildLevel == 3) ? TT_TYPE_BLOCK_ENTRY_LEVEL3
                                         : TT_TYPE_BLOCK_ENTRY;
    Table[Index] = ChildDescriptor;
  }
}

STATIC
EFI_STATUS
MotoG20MapRangeRecursive (
  IN  OUT MOTO_G20_BUILD_CONTEXT  *Context,
  IN  UINT64                      *Table,
  IN  UINTN                       Level,
  IN  UINT64                      RegionStart,
  IN  UINT64                      RegionEnd,
  IN  UINT64                      DescriptorAttributes
  )
{
  EFI_STATUS  Status;
  UINT64      BlockSize;
  UINT64      BlockEnd;
  UINT64      EntryValue;
  UINT64      *Entry;
  UINT64      *ChildTable;
  UINT64      ChildEnd;
  UINTN       Index;

  if ((Context == NULL) || (Table == NULL) ||
      (Level >= MOTO_G20_LEVEL_COUNT) ||
      (RegionStart >= RegionEnd) ||
      (RegionEnd > MOTO_G20_ADDRESS_LIMIT))
  {
    return EFI_INVALID_PARAMETER;
  }

  BlockSize = MotoG20LevelSize (Level);
  BlockEnd  = ((RegionStart / BlockSize) + 1) * BlockSize;
  if (BlockEnd > RegionEnd) {
    BlockEnd = RegionEnd;
  }

  Index = (UINTN)((RegionStart >> MotoG20LevelShift (Level)) & (MOTO_G20_TABLE_ENTRIES - 1));
  Entry = &Table[Index];

  if ((Level == 0) ||
      (((RegionStart | BlockEnd) & (BlockSize - 1)) != 0) ||
      MotoG20IsTableEntry (*Entry, Level))
  {
    if (Level >= 3) {
      return EFI_COMPROMISED_DATA;
    }

    if (MotoG20IsTableEntry (*Entry, Level)) {
      ChildTable = (UINT64 *)(UINTN)MotoG20TableAddress (*Entry);
    } else {
      if ((Level == MOTO_G20_ROOT_LEVEL) && ((*Entry & TT_TYPE_MASK) != 0)) {
        return EFI_COMPROMISED_DATA;
      }

      Status = MotoG20AllocateTable (Context, &ChildTable);
      if (EFI_ERROR (Status)) {
        return Status;
      }

      if (MotoG20IsBlockEntry (*Entry, Level)) {
        MotoG20PopulateSplitTable (ChildTable, Level + 1, *Entry);
      } else if ((*Entry & TT_TYPE_MASK) != 0) {
        return EFI_COMPROMISED_DATA;
      }
    }

    Status = MotoG20MapRangeRecursive (
               Context,
               ChildTable,
               Level + 1,
               RegionStart,
               BlockEnd,
               DescriptorAttributes
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    EntryValue = TT_TYPE_TABLE_ENTRY;
    EntryValue = MotoG20SetOutputAddress (EntryValue, (UINT64)(UINTN)ChildTable);
    *Entry = EntryValue;
    return (BlockEnd == RegionEnd) ? EFI_SUCCESS : MotoG20MapRangeRecursive (
             Context,
             Table,
             Level,
             BlockEnd,
             RegionEnd,
             DescriptorAttributes
             );
  }

  if (Level == 0) {
    return EFI_COMPROMISED_DATA;
  }

  EntryValue = DescriptorAttributes | TT_AF;
  EntryValue = MotoG20SetOutputAddress (EntryValue, RegionStart);
  EntryValue |= (Level == 3) ? TT_TYPE_BLOCK_ENTRY_LEVEL3 : TT_TYPE_BLOCK_ENTRY;
  *Entry = EntryValue;

  ChildEnd = BlockEnd;
  return (ChildEnd == RegionEnd) ? EFI_SUCCESS : MotoG20MapRangeRecursive (
           Context,
           Table,
           Level,
           ChildEnd,
           RegionEnd,
           DescriptorAttributes
           );
}

STATIC
EFI_STATUS
MotoG20ValidateMemoryTable (
  IN CONST ARM_MEMORY_REGION_DESCRIPTOR  *MemoryTable
  )
{
  EFI_STATUS                         Status;
  CONST ARM_MEMORY_REGION_DESCRIPTOR *Region;
  UINT64                             Attributes;
  UINT64                             End;

  Region = MemoryTable;
  while (Region->Length != 0) {
    if ((Region->PhysicalBase & MOTO_G20_PAGE_MASK) != 0 ||
        (Region->VirtualBase & MOTO_G20_PAGE_MASK) != 0 ||
        (Region->Length & MOTO_G20_PAGE_MASK) != 0 ||
        (Region->PhysicalBase != Region->VirtualBase) ||
        (Region->PhysicalBase >= MOTO_G20_PHYSICAL_ADDRESS_LIMIT) ||
        (Region->VirtualBase >= MOTO_G20_VIRTUAL_ADDRESS_LIMIT) ||
        (Region->Length > (MOTO_G20_PHYSICAL_ADDRESS_LIMIT - Region->PhysicalBase)) ||
        (Region->Length > (MOTO_G20_VIRTUAL_ADDRESS_LIMIT - Region->VirtualBase)))
    {
      return EFI_INVALID_PARAMETER;
    }

    End = Region->PhysicalBase + Region->Length;
    if ((End < Region->PhysicalBase) || (End > MOTO_G20_ADDRESS_LIMIT)) {
      return EFI_INVALID_PARAMETER;
    }

    Status = MotoG20AttributesToDescriptor (Region->Attributes, &Attributes);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Region++;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MotoG20BuildTranslationTables (
  IN  CONST ARM_MEMORY_REGION_DESCRIPTOR  *MemoryTable,
  OUT VOID                                **TranslationTableBase,
  OUT UINTN                               *TranslationTableSize,
  OUT UINTN                               *AllocatedTablePages
  )
{
  EFI_STATUS                         Status;
  CONST ARM_MEMORY_REGION_DESCRIPTOR *Region;
  UINT64                             Attributes;
  UINT64                             *RootTable;
  MOTO_G20_BUILD_CONTEXT              Context;

  if ((MemoryTable == NULL) || (TranslationTableBase == NULL) ||
      (TranslationTableSize == NULL) || (AllocatedTablePages == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *TranslationTableBase = NULL;
  *TranslationTableSize = 0;
  *AllocatedTablePages = 0;

  Status = MotoG20ValidateMemoryTable (MemoryTable);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Context.AllocatedPages = 0;
  Status = MotoG20AllocateTable (&Context, &RootTable);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Region = MemoryTable;
  while (Region->Length != 0) {
    Status = MotoG20AttributesToDescriptor (Region->Attributes, &Attributes);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = MotoG20MapRangeRecursive (
               &Context,
               RootTable,
               MOTO_G20_ROOT_LEVEL,
               Region->VirtualBase,
               Region->VirtualBase + Region->Length,
               Attributes
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Region++;
  }

  *TranslationTableBase = RootTable;
  *TranslationTableSize = MOTO_G20_LOGICAL_ROOT_SIZE;
  *AllocatedTablePages = Context.AllocatedPages;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MotoG20ReadTableDescriptor (
  IN  UINT64   Descriptor,
  OUT UINT64  **Table
  )
{
  UINT64  Address;

  if (Table == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Address = MotoG20TableAddress (Descriptor);
  if ((Address >= MOTO_G20_ADDRESS_LIMIT) || ((Address & MOTO_G20_PAGE_MASK) != 0)) {
    return EFI_COMPROMISED_DATA;
  }

  *Table = (UINT64 *)(UINTN)Address;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MotoG20LookupTranslation (
  IN  CONST VOID                  *TranslationTableBase,
  IN  UINT64                       VirtualAddress,
  OUT MOTO_G20_TRANSLATION_LOOKUP *Result
  )
{
  UINT64  Descriptor;
  UINT64  *Table;
  UINTN   Level;
  UINTN   Index;
  UINT64  BlockSize;

  if ((TranslationTableBase == NULL) || (Result == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Result, sizeof (*Result));
  if (VirtualAddress >= MOTO_G20_ADDRESS_LIMIT) {
    return EFI_INVALID_PARAMETER;
  }

  if ((((UINTN)TranslationTableBase & MOTO_G20_PAGE_MASK) != 0) ||
      ((UINT64)(UINTN)TranslationTableBase >= MOTO_G20_ADDRESS_LIMIT))
  {
    return EFI_INVALID_PARAMETER;
  }

  Table = (UINT64 *)(UINTN)TranslationTableBase;
  for (Level = MOTO_G20_ROOT_LEVEL; Level < MOTO_G20_LEVEL_COUNT; Level++) {
    Index = (UINTN)((VirtualAddress >> MotoG20LevelShift (Level)) & (MOTO_G20_TABLE_ENTRIES - 1));
    Descriptor = Table[Index];
    Result->Descriptor = Descriptor;
    Result->Level = Level;

    if (Descriptor == 0) {
      return EFI_SUCCESS;
    }

    if ((Level >= 1) && (Level < 3) && MotoG20IsBlockEntry (Descriptor, Level)) {
      BlockSize = MotoG20LevelSize (Level);
      Result->Mapped = TRUE;
      Result->PhysicalAddress = MotoG20DescriptorAddress (Descriptor) |
                                (VirtualAddress & (BlockSize - 1));
      return EFI_SUCCESS;
    }

    if ((Level == 3) && MotoG20IsBlockEntry (Descriptor, Level)) {
      Result->Mapped = TRUE;
      Result->PhysicalAddress = MotoG20DescriptorAddress (Descriptor) |
                                (VirtualAddress & MOTO_G20_PAGE_MASK);
      return EFI_SUCCESS;
    }

    if (!MotoG20IsTableEntry (Descriptor, Level)) {
      return EFI_COMPROMISED_DATA;
    }

    if (EFI_ERROR (MotoG20ReadTableDescriptor (Descriptor, &Table))) {
      return EFI_COMPROMISED_DATA;
    }
  }

  return EFI_COMPROMISED_DATA;
}
