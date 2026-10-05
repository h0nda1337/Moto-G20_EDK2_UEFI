#include <Base.h>
#include <Uefi/UefiBaseType.h>
#include <Uefi/UefiSpec.h>
#include <Pi/PiBootMode.h>
#include <Pi/PiHob.h>

#include <Library/HobLib.h>
#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <AArch64/AArch64.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrePiLib.h>
#include <Library/PrePiHobListPointerLib.h>
#include <Library/MotoG20TranslationTableLib.h>

#include <Platform.h>

//
// MMU1 PrePi workspace.
//
// This starts exactly at the first byte after the known original
// ARM64 Image footprint:
//
//   Image end        = 0x81881FFF
//   Workspace start  = 0x81882000
//
// It lies inside HOB3 SYSTEM_MEMORY:
//   0x81882000 - 0x81FEFFFF
//
#define MOTO_G20_HOB_BASE        0x81882000ULL
#define MOTO_G20_HOB_SIZE        0x00100000ULL
#define MOTO_G20_HOB_END         0x81982000ULL

#define MOTO_G20_MMU2A_TABLE_COUNT         6U
#define MOTO_G20_MMU2A_TABLE_BYTES        0x00006000ULL
#define MOTO_G20_MMU2A_EXPECTED_FINAL_TOP 0x8197C000ULL
#define MOTO_G20_MMU2A_MARKER_X            656U
#define MOTO_G20_MMU2A_MARKER_Y            304U
#define MOTO_G20_MMU2C_MARKER_X            656U
#define MOTO_G20_MMU2C_MARKER_Y            464U
#define MOTO_G20_MMU3_MARKER_X            656U
#define MOTO_G20_MMU3_MARKER_Y            544U

STATIC CONST ARM_MEMORY_REGION_DESCRIPTOR
mMotoG20Mmu2aMemoryTable[] = {
  { 0x80000000ULL, 0x80000000ULL, 0x01FF0000ULL,
    ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK },
  { 0x82000000ULL, 0x82000000ULL, 0x00A0B000ULL,
    ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK },
  { 0x9E000000ULL, 0x9E000000ULL, 0x009E4000ULL,
    ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED },
  { 0, 0, 0, 0 }
};

STATIC CONST UINT64
mMotoG20Mmu2aExpectedTablePages[MOTO_G20_MMU2A_TABLE_COUNT] = {
  0x81981000ULL, 0x81980000ULL, 0x8197F000ULL,
  0x8197E000ULL, 0x8197D000ULL, 0x8197C000ULL
};

STATIC VOID *mMotoG20Mmu2aSeenTables[MOTO_G20_MMU2A_TABLE_COUNT];
STATIC BOOLEAN mMotoG20Mmu2aUsedEntries[MOTO_G20_MMU2A_TABLE_COUNT][TT_ENTRY_COUNT];
STATIC CONST UINT64 mMotoG20Mmu2aCriticalAddresses[] = {
  0x80080000ULL, 0x80081000ULL, 0x81000000ULL, 0x811D0000ULL,
  0x811F0000ULL, 0x81882000ULL, 0x81981000ULL, 0x81980000ULL,
  0x8197F000ULL, 0x8197E000ULL, 0x8197D000ULL, 0x8197C000ULL,
  0x82000000ULL, 0x82100000ULL, 0x82A0A000ULL, 0x9E000000ULL,
  0x9E9E3000ULL
};
STATIC CONST UINT64 mMotoG20Mmu2aGuardAddresses[] = {
  0x00000000ULL, 0x7FFFF000ULL, 0x81FF0000ULL, 0x81FFF000ULL,
  0x82A0B000ULL, 0x9D000000ULL, 0x9DFFF000ULL, 0x9E9E4000ULL,
  0x94000000ULL, 0xF4100000ULL, 0x100000000ULL
};

STATIC
INTN
MotoG20Mmu2aTableIndex (
  IN CONST VOID *Table
  )
{
  UINTN Index;

  for (Index = 0; Index < MOTO_G20_MMU2A_TABLE_COUNT; Index++) {
    if ((UINTN)Table == (UINTN)mMotoG20Mmu2aExpectedTablePages[Index]) {
      return (INTN)Index;
    }
  }

  return -1;
}

STATIC
BOOLEAN
MotoG20Mmu2aWalkTableGraph (
  IN UINT64 *Table,
  IN UINTN   Level,
  IN OUT UINTN *SeenCount
  )
{
  UINTN Index;
  INTN TableIndex;
  UINT64 Descriptor;
  UINT64 ChildAddress;
  UINT64 *Child;

  if ((Table == NULL) || (Level > 3) ||
      (((UINTN)Table & EFI_PAGE_MASK) != 0) ||
      ((UINT64)(UINTN)Table < MOTO_G20_HOB_BASE) ||
      ((UINT64)(UINTN)Table >= MOTO_G20_HOB_END) ||
      (*SeenCount >= MOTO_G20_MMU2A_TABLE_COUNT)) {
    return FALSE;
  }

  TableIndex = MotoG20Mmu2aTableIndex (Table);
  if (TableIndex < 0) {
    return FALSE;
  }

  for (Index = 0; Index < *SeenCount; Index++) {
    if (mMotoG20Mmu2aSeenTables[Index] == Table) {
      return FALSE;
    }
  }

  mMotoG20Mmu2aSeenTables[*SeenCount] = Table;
  (*SeenCount)++;

  for (Index = 0; Index < TT_ENTRY_COUNT; Index++) {
    Descriptor = Table[Index];
    mMotoG20Mmu2aUsedEntries[TableIndex][Index] =
      (BOOLEAN)(Descriptor != 0);
    if ((Level < 3) &&
        ((Descriptor & TT_TYPE_MASK) == TT_TYPE_TABLE_ENTRY)) {
      ChildAddress = Descriptor & TT_ADDRESS_MASK_DESCRIPTION_TABLE;
      Child = (UINT64 *)(UINTN)ChildAddress;
      if (!MotoG20Mmu2aWalkTableGraph (Child, Level + 1, SeenCount)) {
        return FALSE;
      }
    }
  }

  return TRUE;
}

STATIC
BOOLEAN
MotoG20Mmu2aLookup (
  IN VOID    *Root,
  IN UINT64   VirtualAddress,
  IN BOOLEAN  ExpectedMapped,
  IN UINT64   ExpectedAttribute
  )
{
  EFI_STATUS Status;
  MOTO_G20_TRANSLATION_LOOKUP Result;

  Status = MotoG20LookupTranslation (Root, VirtualAddress, &Result);
  return (BOOLEAN)(
    !EFI_ERROR(Status) &&
    (Result.Mapped == ExpectedMapped) &&
    (!ExpectedMapped ||
     ((Result.PhysicalAddress == VirtualAddress) &&
      ((Result.Descriptor & TT_AF) != 0) &&
      ((Result.Descriptor & TT_ATTR_INDX_MASK) == ExpectedAttribute)))
    );
}

STATIC
BOOLEAN
MotoG20Mmu2aSelfTest (
  IN VOID  *Root,
  IN UINTN  TableSize,
  IN UINTN  AllocatedPages,
  IN UINTN  FinalTop
  )
{
  UINTN Index;
  UINTN TableIndex;
  UINTN SeenCount;
  UINTN ZeroIndex;
  BOOLEAN GraphOk;
  if ((Root == NULL) || ((UINTN)Root != 0x81981000ULL) ||
      (TableSize != 16) || (AllocatedPages != 6) ||
      (FinalTop != MOTO_G20_MMU2A_EXPECTED_FINAL_TOP)) {
    return FALSE;
  }

  ZeroMem (mMotoG20Mmu2aSeenTables, sizeof(mMotoG20Mmu2aSeenTables));
  ZeroMem (mMotoG20Mmu2aUsedEntries, sizeof(mMotoG20Mmu2aUsedEntries));
  SeenCount = 0;
  GraphOk = MotoG20Mmu2aWalkTableGraph ((UINT64 *)Root, 0, &SeenCount);
  if (!GraphOk || (SeenCount != MOTO_G20_MMU2A_TABLE_COUNT)) {
    return FALSE;
  }

  for (TableIndex = 0; TableIndex < MOTO_G20_MMU2A_TABLE_COUNT; TableIndex++) {
    if (mMotoG20Mmu2aSeenTables[TableIndex] == NULL) {
      return FALSE;
    }
    for (ZeroIndex = 0; ZeroIndex < TT_ENTRY_COUNT; ZeroIndex++) {
      if (!mMotoG20Mmu2aUsedEntries[TableIndex][ZeroIndex] &&
          (((UINT64 *)mMotoG20Mmu2aSeenTables[TableIndex])[ZeroIndex] != 0)) {
        return FALSE;
      }
    }
  }

  for (Index = 0; Index < sizeof(mMotoG20Mmu2aCriticalAddresses) / sizeof(mMotoG20Mmu2aCriticalAddresses[0]); Index++) {
    if (!MotoG20Mmu2aLookup(Root, mMotoG20Mmu2aCriticalAddresses[Index], TRUE,
                             TT_ATTR_INDX_MEMORY_WRITE_BACK)) {
      if ((mMotoG20Mmu2aCriticalAddresses[Index] >= 0x9E000000ULL) &&
          (mMotoG20Mmu2aCriticalAddresses[Index] < 0x9E9E4000ULL)) {
        if (!MotoG20Mmu2aLookup(Root, mMotoG20Mmu2aCriticalAddresses[Index], TRUE,
                                 TT_ATTR_INDX_MEMORY_NON_CACHEABLE)) {
          return FALSE;
        }
      } else {
        return FALSE;
      }
    }
  }

  for (Index = 0; Index < sizeof(mMotoG20Mmu2aGuardAddresses) / sizeof(mMotoG20Mmu2aGuardAddresses[0]); Index++) {
    if (!MotoG20Mmu2aLookup(Root, mMotoG20Mmu2aGuardAddresses[Index], FALSE, 0)) {
      return FALSE;
    }
  }

  for (Index = 0; Index < MOTO_G20_MMU2A_TABLE_COUNT; Index++) {
    if (!MotoG20Mmu2aLookup(Root, mMotoG20Mmu2aExpectedTablePages[Index],
                            TRUE, TT_ATTR_INDX_MEMORY_WRITE_BACK)) {
      return FALSE;
    }
  }

  return TRUE;
}

#define MOTO_G20_STACK_BASE      0x811F0000ULL
#define MOTO_G20_STACK_SIZE      0x00010000ULL

STATIC volatile UINT64 mRelocCookie =
  0x5052455049484F42ULL;

STATIC volatile UINT64 * volatile mRelocCookiePointer =
  &mRelocCookie;

// MMU3 assembly hand-off state.  The enable stub consumes only the boolean;
// the SCTLR value is a post-enable proof record written by the stub.
volatile BOOLEAN gMotoG20Mmu3PreEnableOk = FALSE;
volatile UINT64  gMotoG20Mmu3AfterEnableSctlr = 0;
volatile UINT64  gMotoG20Mmu3DataProbe = 0;

STATIC CONST EFI_GUID mMotoG20RealHobGuid = {
  0x8c84d850,
  0xa9a6,
  0x48bb,
  { 0xa4, 0x55, 0x4d, 0x4f, 0x54, 0x4f, 0x47, 0x32 }
};

typedef struct {
  UINT64 MagicA;
  UINT64 MagicB;
  UINT64 DtbAddress;
  UINT64 PhysicalAddressBits;
} MOTO_G20_REAL_HOB_PAYLOAD;

typedef struct {
  EFI_RESOURCE_TYPE     Type;
  EFI_PHYSICAL_ADDRESS  Base;
  UINT64                Length;
} MOTO_G20_DRAM_RESOURCE;

#define HOB3_DRAM_END_EXCLUSIVE         0x17FFFF000ULL
#define HOB3_DRAM_TOTAL                 0xFFFFF000ULL

#define HOB3_SYSTEM_TOTAL_EXPECTED      0xDA957000ULL
#define HOB3_RESERVED_TOTAL_EXPECTED    0x256A8000ULL

#define HOB3_DRAM_MAP_COUNT             16U

STATIC CONST MOTO_G20_DRAM_RESOURCE
mMotoG20DramMap[HOB3_DRAM_MAP_COUNT] = {
  { EFI_RESOURCE_MEMORY_RESERVED, 0x80000000ULL,  0x01882000ULL },
  { EFI_RESOURCE_SYSTEM_MEMORY,   0x81882000ULL,  0x0076E000ULL },
  { EFI_RESOURCE_MEMORY_RESERVED, 0x81FF0000ULL,  0x00A1B000ULL },
  { EFI_RESOURCE_SYSTEM_MEMORY,   0x82A0B000ULL,  0x04AF5000ULL },
  { EFI_RESOURCE_MEMORY_RESERVED, 0x87500000ULL,  0x06A00000ULL },
  { EFI_RESOURCE_SYSTEM_MEMORY,   0x8DF00000ULL,  0x06100000ULL },
  { EFI_RESOURCE_MEMORY_RESERVED, 0x94000000ULL,  0x06000000ULL },
  { EFI_RESOURCE_SYSTEM_MEMORY,   0x9A000000ULL,  0x03000000ULL },
  { EFI_RESOURCE_MEMORY_RESERVED, 0x9D000000ULL,  0x03000000ULL },
  { EFI_RESOURCE_SYSTEM_MEMORY,   0xA0000000ULL,  0x432AE000ULL },
  { EFI_RESOURCE_MEMORY_RESERVED, 0xE32AE000ULL,  0x11152000ULL },
  { EFI_RESOURCE_SYSTEM_MEMORY,   0xF4400000ULL,  0x09308000ULL },
  { EFI_RESOURCE_MEMORY_RESERVED, 0xFD708000ULL,  0x028B8000ULL },
  { EFI_RESOURCE_SYSTEM_MEMORY,   0xFFFC0000ULL,  0x00040000ULL },
  { EFI_RESOURCE_MEMORY_RESERVED, 0x100000000ULL, 0x00001000ULL },
  { EFI_RESOURCE_SYSTEM_MEMORY,   0x100001000ULL, 0x7FFFE000ULL }
};

STATIC
VOID
ZeroBytes (
  IN VOID  *Buffer,
  IN UINTN  Size
  )
{
  volatile UINT8 *Ptr;
  UINTN Index;

  Ptr = (volatile UINT8 *)Buffer;

  for (Index = 0; Index < Size; Index++) {
    Ptr[Index] = 0;
  }
}

STATIC
__attribute__((noinline))
VOID
FillRectangle (
  IN UINTN  StartX,
  IN UINTN  StartY,
  IN UINTN  Width,
  IN UINTN  Height,
  IN UINT32 Color
  )
{
  volatile UINT32 *Framebuffer;
  UINTN X;
  UINTN Y;

  Framebuffer =
    (volatile UINT32 *)(UINTN)
    MOTO_G20_FRAMEBUFFER_BASE;

  for (Y = StartY; Y < StartY + Height; Y++) {
    for (X = StartX; X < StartX + Width; X++) {
      Framebuffer[
        (Y * MOTO_G20_DISPLAY_WIDTH) + X
      ] = Color;
    }
  }
}

//
// MMU2B/MMU2C diagnostic support.  The original MMU2B capture remains
// read-only and self-contained; MMU2C adds only the audited translation
// register preload helpers below.  All values render as full 64-bit hex
// without libc or formatting helpers.
//
STATIC CONST UINT8 mMotoG20HexFont[16][7] = {
  { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
  { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E },
  { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E },
  { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
  { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x1C },
  { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
  { 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F }, { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },
  { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }
};
STATIC CHAR8 mMotoG20HexDigits[17];

STATIC
UINTN
MotoG20FontIndex (
  IN CHAR8 Character
  )
{
  if ((Character >= '0') && (Character <= '9')) {
    return (UINTN)(Character - '0');
  }

  if ((Character >= 'A') && (Character <= 'Z')) {
    return (UINTN)(Character - 'A') + 10U;
  }

  return 0;
}

STATIC
VOID
MotoG20DrawText (
  IN UINTN       StartX,
  IN UINTN       StartY,
  IN CONST CHAR8 *Text,
  IN UINT32      Color
  )
{
  UINTN CharacterIndex;
  UINTN Row;
  UINTN Column;
  UINT8 Bits;

  for (CharacterIndex = 0; Text[CharacterIndex] != '\0'; CharacterIndex++) {
    if (Text[CharacterIndex] == ' ') {
      continue;
    }

    for (Row = 0; Row < 5; Row++) {
      Bits = mMotoG20HexFont[MotoG20FontIndex (Text[CharacterIndex])][Row];
      for (Column = 0; Column < 5; Column++) {
        if ((Bits & (1U << (4U - Column))) != 0) {
          FillRectangle(
            StartX + (CharacterIndex * 12U) + (Column * 2U),
            StartY + (Row * 2U), 2, 2, Color
            );
        }
      }
    }
  }
}

STATIC
VOID
MotoG20DrawHexString (
  IN UINTN       StartX,
  IN UINTN       StartY,
  IN CONST CHAR8 *Text,
  IN UINT32      Color
  )
{
  UINTN CharacterIndex;
  UINTN Row;
  UINTN Column;
  UINTN GlyphIndex;
  UINT8 Bits;

  for (CharacterIndex = 0; Text[CharacterIndex] != '\0'; CharacterIndex++) {
    GlyphIndex = MotoG20FontIndex (Text[CharacterIndex]);
    for (Row = 0; Row < 7; Row++) {
      Bits = mMotoG20HexFont[GlyphIndex][Row];
      for (Column = 0; Column < 5; Column++) {
        if ((Bits & (1U << (4U - Column))) != 0) {
          FillRectangle (
            StartX + (CharacterIndex * 18U) + (Column * 3U),
            StartY + (Row * 3U), 3, 3, Color
            );
        }
      }
    }
  }
}

STATIC
VOID
MotoG20DrawHex64 (
  IN UINTN  StartX,
  IN UINTN  StartY,
  IN UINT64 Value,
  IN UINT32 Color
  )
{
  STATIC CONST CHAR8 Hex[] = "0123456789ABCDEF";
  UINTN Index;

  mMotoG20HexDigits[16] = '\0';
  for (Index = 0; Index < 16; Index++) {
    mMotoG20HexDigits[Index] =
      Hex[(Value >> ((15U - Index) * 4U)) & 0xFULL];
  }

  MotoG20DrawHexString (StartX, StartY, mMotoG20HexDigits, Color);
}

STATIC
UINT64
MotoG20ReadMmfr0 (
  VOID
  )
{
  UINT64 Value;

  __asm__ volatile ("mrs %0, ID_AA64MMFR0_EL1" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20ReadSctlrEl2 (
  VOID
  )
{
  UINT64 Value;

  __asm__ volatile ("mrs %0, SCTLR_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20ReadTcrEl2 (
  VOID
  )
{
  UINT64 Value;

  __asm__ volatile ("mrs %0, TCR_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20ReadTtbr0El2 (
  VOID
  )
{
  UINT64 Value;

  __asm__ volatile ("mrs %0, TTBR0_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20ReadMairEl2 (
  VOID
  )
{
  UINT64 Value;

  __asm__ volatile ("mrs %0, MAIR_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20ReadVbarEl2 (
  VOID
  )
{
  UINT64 Value;

  __asm__ volatile ("mrs %0, VBAR_EL2" : "=r" (Value));
  return Value;
}

//
// MMU2C translation-register preload primitives.  These deliberately do
// not touch SCTLR_EL2 and each write is followed by the architectural ISB
// pattern used by the local EDK2 ArmLib register helpers.
//
STATIC
VOID
MotoG20WriteMairEl2 (
  IN UINT64 Value
  )
{
  __asm__ volatile (
    "msr MAIR_EL2, %0\n"
    "isb"
    :
    : "r" (Value)
    : "memory"
    );
}

STATIC
VOID
MotoG20WriteTcrEl2 (
  IN UINT64 Value
  )
{
  __asm__ volatile (
    "msr TCR_EL2, %0\n"
    "isb"
    :
    : "r" (Value)
    : "memory"
    );
}

STATIC
VOID
MotoG20WriteTtbr0El2 (
  IN UINT64 Value
  )
{
  __asm__ volatile (
    "msr TTBR0_EL2, %0\n"
    "isb"
    :
    : "r" (Value)
    : "memory"
    );
}

STATIC
VOID
MotoG20TranslationTableCommitBarrier (
  VOID
  )
{
  // The tables were populated while translation and caches were disabled.
  // DSB SY completes those descriptor stores before register programming.
  __asm__ volatile ("dsb sy" ::: "memory");
}

STATIC
UINT8
MotoG20DecodePaBits (
  IN UINT64 Mmfr0
  )
{
  STATIC CONST UINT8 PaBits[] = { 32, 36, 40, 42, 44, 48, 52, 0 };
  UINTN Index;

  Index = (UINTN)(Mmfr0 & 0xFU);
  return (Index < sizeof(PaBits)) ? PaBits[Index] : 0;
}

STATIC
EFI_RESOURCE_ATTRIBUTE_TYPE
MotoG20ResourceAttributes (
  IN EFI_RESOURCE_TYPE Type
  )
{
  if (Type == EFI_RESOURCE_SYSTEM_MEMORY) {
    return (
      EFI_RESOURCE_ATTRIBUTE_PRESENT |
      EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
      EFI_RESOURCE_ATTRIBUTE_TESTED |
      EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE |
      EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |
      EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |
      EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE
      );
  }

  //
  // Conservative reserved-memory descriptor.
  // Do not advertise cache capabilities here yet.
  //
  return (
    EFI_RESOURCE_ATTRIBUTE_PRESENT |
    EFI_RESOURCE_ATTRIBUTE_INITIALIZED
    );
}

STATIC
UINT32
CpuPaBitsColor (
  IN UINT8 Bits
  )
{
  switch (Bits) {
    case 32:
      return 0x00FF8000; // orange
    case 36:
      return 0x00FFFF00; // yellow
    case 40:
      return 0x00FF00FF; // magenta
    case 42:
      return 0x0000FFFF; // cyan
    case 44:
      return 0x000000FF; // blue
    case 48:
      return 0x00FFFFFF; // white
    case 52:
      return 0x0000FF00; // green
    default:
      return 0x00FF0000; // red = invalid/unknown
  }
}

VOID
EFIAPI
MotoG20EarlyInit (
  IN UINTN DtbAddress,
  IN UINTN OriginalStack
  )
{
  EFI_HOB_HANDOFF_INFO_TABLE   *HobList;
  EFI_HOB_HANDOFF_INFO_TABLE   *PublishedHobList;
  EFI_HOB_RESOURCE_DESCRIPTOR  *Resource;
  EFI_HOB_MEMORY_ALLOCATION    *StackHob;
  EFI_HOB_GUID_TYPE            *GuidHob;
  EFI_HOB_CPU                  *CpuHob;

  MOTO_G20_REAL_HOB_PAYLOAD *Payload;

  EFI_HOB_GENERIC_HEADER *Current;

  EFI_STATUS Status;

  BOOLEAN RelocOk;
  BOOLEAN ConstructorOk;
  BOOLEAN ResourceOk;
  BOOLEAN StackOk;
  BOOLEAN CpuOk;
  BOOLEAN DtbOk;
  BOOLEAN ChainOk;
  BOOLEAN Mmu2aOk;

  UINT8 PhysicalAddressBits;

  UINTN CurrentEl;
  UINTN HcrEl2;
  BOOLEAN E2hEnabled;

  UINT64 LiveMmfr0;
  UINT64 LiveSctlrEl2;
  UINT64 LiveTcrEl2;
  UINT64 LiveTtbr0El2;
  UINT64 LiveMairEl2;
  UINT64 LiveChecksum;
  BOOLEAN Mmu2bProbeOk;

  UINT64 BeforeSctlr;
  UINT64 BeforeTcr;
  UINT64 BeforeTtbr0;
  UINT64 BeforeMair;
  UINT64 AfterSctlr;
  UINT64 AfterTcr;
  UINT64 AfterTtbr0;
  UINT64 AfterMair;
  BOOLEAN Mmu2cMairOk;
  BOOLEAN Mmu2cTcrOk;
  BOOLEAN Mmu2cTtbr0Ok;
  BOOLEAN Mmu2cWalkOk;
  BOOLEAN Mmu2cOk;
  BOOLEAN Mmu3VbarOk;

  UINT64 VbarEl2;

  UINTN HobCount;
  UINTN ResourceIndex;
  UINTN MemoryAllocationCount;

  VOID *TranslationTableBase;
  UINTN TranslationTableSize;
  UINTN AllocatedTablePages;

  UINT64 SystemTotal;
  UINT64 ReservedTotal;

  EFI_PHYSICAL_ADDRESS DramCursor;

  EFI_HOB_GENERIC_HEADER *MapWalker;

  (VOID)DtbAddress;
  (VOID)OriginalStack;

  RelocOk =
    (BOOLEAN)(
      (mRelocCookiePointer == &mRelocCookie) &&
      (*mRelocCookiePointer ==
       0x5052455049484F42ULL)
    );

  //
  // MMU0 prerequisite probe.
  // Read-only inspection of the EL2 translation regime.
  //
  CurrentEl =
    ArmReadCurrentEL();

  HcrEl2 = 0;
  E2hEnabled = FALSE;

  if (CurrentEl == AARCH64_EL2) {
    HcrEl2 =
      ArmReadHcr();

    E2hEnabled =
      (BOOLEAN)(
        (HcrEl2 & ARM_HCR_E2H) != 0
        );
  }

  //
  // MMU2B_READ_ONLY_REGISTER_PROBE.  These seven captures are the only
  // system-register reads in this block; all values remain 64-bit.
  //
  LiveMmfr0    = MotoG20ReadMmfr0 ();
  LiveSctlrEl2 = MotoG20ReadSctlrEl2 ();
  LiveTcrEl2   = MotoG20ReadTcrEl2 ();
  LiveTtbr0El2 = MotoG20ReadTtbr0El2 ();
  LiveMairEl2  = MotoG20ReadMairEl2 ();
  PhysicalAddressBits = MotoG20DecodePaBits (LiveMmfr0);
  LiveChecksum =
    (UINT64)CurrentEl ^ (UINT64)HcrEl2 ^ LiveMmfr0 ^ LiveSctlrEl2 ^
      LiveTcrEl2 ^ LiveTtbr0El2 ^ LiveMairEl2;

  // MMU2C retains the exact early MMU2B capture as its BEFORE snapshot.
  BeforeSctlr = LiveSctlrEl2;
  BeforeTcr   = LiveTcrEl2;
  BeforeTtbr0 = LiveTtbr0El2;
  BeforeMair  = LiveMairEl2;

  AfterSctlr = BeforeSctlr;
  AfterTcr   = BeforeTcr;
  AfterTtbr0 = BeforeTtbr0;
  AfterMair  = BeforeMair;
  Mmu2cMairOk = FALSE;
  Mmu2cTcrOk = FALSE;
  Mmu2cTtbr0Ok = FALSE;
  Mmu2cWalkOk = FALSE;
  Mmu2cOk = FALSE;
  Mmu3VbarOk = FALSE;
  VbarEl2 = 0;

  Mmu2bProbeOk =
    (BOOLEAN)(
      (CurrentEl == AARCH64_EL2) &&
      ((HcrEl2 & ARM_HCR_E2H) == 0) &&
      ((LiveSctlrEl2 & (BIT0 | BIT2 | BIT12)) == 0) &&
      (PhysicalAddressBits == 40) &&
      (LiveMmfr0 != 0)
      );

  //
  // Preserve and validate the FDT handoff received in X0.
  // FDT magic is big-endian D0 0D FE ED.
  //
  DtbOk =
    (BOOLEAN)(
      (DtbAddress != 0) &&
      (((volatile UINT8 *)(UINTN)DtbAddress)[0] == 0xD0) &&
      (((volatile UINT8 *)(UINTN)DtbAddress)[1] == 0x0D) &&
      (((volatile UINT8 *)(UINTN)DtbAddress)[2] == 0xFE) &&
      (((volatile UINT8 *)(UINTN)DtbAddress)[3] == 0xED)
    );

  //
  // Clean the HOB arena before giving it to EDK2.
  //
  ZeroBytes(
    (VOID *)(UINTN)MOTO_G20_HOB_BASE,
    MOTO_G20_HOB_SIZE
    );

  //
  // REAL EmbeddedPkg/PrePiHobLib HobConstructor().
  //
  HobList = HobConstructor(
    (VOID *)(UINTN)MOTO_G20_HOB_BASE,
    MOTO_G20_HOB_SIZE,
    (VOID *)(UINTN)MOTO_G20_HOB_BASE,
    (VOID *)(UINTN)(
      MOTO_G20_HOB_BASE +
      MOTO_G20_HOB_SIZE
      )
    );

  //
  // Publish it using the PrePi pointer interface.
  //
  Status = PrePeiSetHobList(HobList);

  PublishedHobList =
    (EFI_HOB_HANDOFF_INFO_TABLE *)
    PrePeiGetHobList();

  ConstructorOk =
    (BOOLEAN)(
      (HobList != NULL) &&
      !EFI_ERROR(Status) &&
      (PublishedHobList == HobList) &&
      (HobList->Header.HobType ==
       EFI_HOB_TYPE_HANDOFF) &&
      (HobList->Version ==
       EFI_HOB_HANDOFF_TABLE_VERSION)
    );

  //
  // HOB3: publish all 16 DRAM Resource Descriptor HOBs.
  //
  for (
    ResourceIndex = 0;
    ResourceIndex < HOB3_DRAM_MAP_COUNT;
    ResourceIndex++
    )
  {
    BuildResourceDescriptorHob(
      mMotoG20DramMap[ResourceIndex].Type,
      MotoG20ResourceAttributes(
        mMotoG20DramMap[ResourceIndex].Type
        ),
      mMotoG20DramMap[ResourceIndex].Base,
      mMotoG20DramMap[ResourceIndex].Length
      );
  }

  //
  // REAL PrePiHobLib stack allocation HOB.
  //
  BuildStackHob(
    MOTO_G20_STACK_BASE,
    MOTO_G20_STACK_SIZE
    );

  //
  // REAL PrePiHobLib CPU HOB.
  //
  BuildCpuHob(
    PhysicalAddressBits,
    16
    );

  //
  // REAL PrePiHobLib GUID HOB.
  //
  Payload =
    (MOTO_G20_REAL_HOB_PAYLOAD *)
    BuildGuidHob(
      &mMotoG20RealHobGuid,
      sizeof(MOTO_G20_REAL_HOB_PAYLOAD)
      );

  if (Payload != NULL) {
    Payload->MagicA =
      0x1122334455667788ULL;

    Payload->MagicB =
      0xDEADBEEFA55A5AA5ULL;

    Payload->DtbAddress =
      DtbAddress;

    Payload->PhysicalAddressBits =
      PhysicalAddressBits;
  }

  //
  // MMU2A build-only integration.  The MMU and both caches remain OFF.
  // The local PrePi allocator starts at 0x81982000 and therefore returns
  // the six table pages at the exact host-proven physical addresses.
  //
  TranslationTableBase = NULL;
  TranslationTableSize = 0;
  AllocatedTablePages = 0;
  Status = MotoG20BuildTranslationTables(
             mMotoG20Mmu2aMemoryTable,
             &TranslationTableBase,
             &TranslationTableSize,
             &AllocatedTablePages
             );
  Mmu2aOk = (BOOLEAN)(
              !EFI_ERROR(Status) &&
              (HobList->EfiFreeMemoryTop ==
               MOTO_G20_MMU2A_EXPECTED_FINAL_TOP) &&
              MotoG20Mmu2aSelfTest(
                TranslationTableBase,
                TranslationTableSize,
                AllocatedTablePages,
                HobList->EfiFreeMemoryTop
                )
              );

  //
  // MMU2C_TRANSLATION_REGISTER_PRELOAD.  Fail-stop sequencing: no
  // translation-register write is attempted unless the complete MMU2A
  // build/self-test has passed.  There is intentionally no TLBI here:
  // SCTLR_EL2.M remains zero, so no active EL2 translation regime is being
  // replaced in this milestone.  TLBI is reserved for a later activation
  // milestone if required by its own audit.
  //
  if (Mmu2aOk) {
    MotoG20TranslationTableCommitBarrier ();

    MotoG20WriteMairEl2 (0x00000000FFBB4400ULL);
    AfterMair = MotoG20ReadMairEl2 ();
    Mmu2cMairOk =
      (BOOLEAN)(AfterMair == 0x00000000FFBB4400ULL);

    if (Mmu2cMairOk) {
      MotoG20WriteTcrEl2 (0x0000000080823518ULL);
      AfterTcr = MotoG20ReadTcrEl2 ();
      Mmu2cTcrOk =
        (BOOLEAN)(AfterTcr == 0x0000000080823518ULL);
    }

    if (Mmu2cTcrOk) {
      MotoG20WriteTtbr0El2 (0x0000000081981000ULL);
      AfterTtbr0 = MotoG20ReadTtbr0El2 ();
      Mmu2cTtbr0Ok =
        (BOOLEAN)(AfterTtbr0 == 0x0000000081981000ULL);
    }
  }

  // Read back SCTLR and all translation registers after the sequence.  No
  // SCTLR write exists in MMU2C; the full BEFORE value must be preserved.
  AfterSctlr = MotoG20ReadSctlrEl2 ();
  if (!Mmu2cMairOk) {
    AfterMair = MotoG20ReadMairEl2 ();
  }
  if (!Mmu2cTcrOk) {
    AfterTcr = MotoG20ReadTcrEl2 ();
  }
  if (!Mmu2cTtbr0Ok) {
    AfterTtbr0 = MotoG20ReadTtbr0El2 ();
  }

  Mmu2cWalkOk =
    (BOOLEAN)(
      Mmu2aOk &&
      MotoG20Mmu2aSelfTest(
        TranslationTableBase,
        TranslationTableSize,
        AllocatedTablePages,
        HobList->EfiFreeMemoryTop
        )
      );

  Mmu2cOk =
    (BOOLEAN)(
      Mmu2bProbeOk &&
      Mmu2cMairOk &&
      Mmu2cTcrOk &&
      Mmu2cTtbr0Ok &&
      Mmu2cWalkOk &&
      (AfterSctlr == BeforeSctlr) &&
      ((AfterSctlr & (BIT0 | BIT2 | BIT12)) == 0)
      );

  // MMU3 exception-vector policy: read-only, fail-closed preflight.  No
  // VBAR_EL2 write is performed.  M may be enabled only when the live vector
  // table is 2 KiB aligned and its complete 2 KiB envelope is identity-mapped.
  VbarEl2 = MotoG20ReadVbarEl2 ();
  Mmu3VbarOk =
    (BOOLEAN)(
      ((VbarEl2 & 0x7FFULL) == 0) &&
      (((VbarEl2 >= 0x80000000ULL) &&
        (VbarEl2 <= (0x81FF0000ULL - 0x800ULL))) ||
       ((VbarEl2 >= 0x82000000ULL) &&
        (VbarEl2 <= (0x82A0B000ULL - 0x800ULL))))
      );
  gMotoG20Mmu3PreEnableOk =
    (BOOLEAN)(Mmu2cOk && Mmu2aOk && Mmu2bProbeOk && Mmu3VbarOk);

  //
  // HOB3: independently walk and validate all Resource HOBs.
  //
  ResourceOk           = TRUE;
  ResourceIndex        = 0;
  MemoryAllocationCount = 0;
  SystemTotal          = 0;
  ReservedTotal        = 0;
  DramCursor           = MOTO_G20_DRAM_BASE;

  MapWalker =
    (EFI_HOB_GENERIC_HEADER *)
    PublishedHobList;

  while (TRUE) {
    UINTN WalkerAddress;
    UINTN ArenaEnd;

    WalkerAddress = (UINTN)MapWalker;
    ArenaEnd =
      MOTO_G20_HOB_BASE +
      MOTO_G20_HOB_SIZE;

    //
    // Every HOB header must live completely inside the arena.
    //
    if (
      (WalkerAddress < MOTO_G20_HOB_BASE) ||
      (WalkerAddress > ArenaEnd) ||
      (
        (ArenaEnd - WalkerAddress) <
        sizeof(EFI_HOB_GENERIC_HEADER)
      )
      )
    {
      ResourceOk = FALSE;
      break;
    }

    if (
      MapWalker->HobType ==
      EFI_HOB_TYPE_END_OF_HOB_LIST
      )
    {
      break;
    }

    //
    // Reject zero/short/misaligned or out-of-arena HOBs.
    //
    if (
      (MapWalker->HobLength <
       sizeof(EFI_HOB_GENERIC_HEADER)) ||
      ((MapWalker->HobLength & 7U) != 0) ||
      (
        MapWalker->HobLength >
        (ArenaEnd - WalkerAddress)
      )
      )
    {
      ResourceOk = FALSE;
      break;
    }

    if (
      MapWalker->HobType ==
      EFI_HOB_TYPE_MEMORY_ALLOCATION
      )
    {
      MemoryAllocationCount++;

    }

    if (
      MapWalker->HobType ==
      EFI_HOB_TYPE_RESOURCE_DESCRIPTOR
      )
    {
      Resource =
        (EFI_HOB_RESOURCE_DESCRIPTOR *)
        (VOID *)MapWalker;

      if (
        ResourceIndex >=
        HOB3_DRAM_MAP_COUNT
        )
      {
        ResourceOk = FALSE;
        break;
      }

      if (
        (Resource->ResourceType !=
         mMotoG20DramMap[ResourceIndex].Type) ||

        (Resource->PhysicalStart !=
         mMotoG20DramMap[ResourceIndex].Base) ||

        (Resource->ResourceLength !=
         mMotoG20DramMap[ResourceIndex].Length) ||

        (Resource->ResourceAttribute !=
         MotoG20ResourceAttributes(
           mMotoG20DramMap[ResourceIndex].Type
           )) ||

        (Resource->PhysicalStart !=
         DramCursor)
        )
      {
        ResourceOk = FALSE;
        break;
      }

      if (
        Resource->ResourceType ==
        EFI_RESOURCE_SYSTEM_MEMORY
        )
      {
        SystemTotal +=
          Resource->ResourceLength;
      }
      else if (
        Resource->ResourceType ==
        EFI_RESOURCE_MEMORY_RESERVED
        )
      {
        ReservedTotal +=
          Resource->ResourceLength;
      }
      else
      {
        ResourceOk = FALSE;
        break;
      }

      DramCursor =
        Resource->PhysicalStart +
        Resource->ResourceLength;

      ResourceIndex++;
    }

    MapWalker =
      (EFI_HOB_GENERIC_HEADER *)(
        (UINT8 *)MapWalker +
        MapWalker->HobLength
        );
  }

  if (MemoryAllocationCount != (MOTO_G20_MMU2A_TABLE_COUNT + 1)) {
    Mmu2aOk = FALSE;
  }

  if (
    (ResourceIndex != HOB3_DRAM_MAP_COUNT) ||

    (SystemTotal !=
     HOB3_SYSTEM_TOTAL_EXPECTED) ||

    (ReservedTotal !=
     HOB3_RESERVED_TOTAL_EXPECTED) ||

    ((SystemTotal + ReservedTotal) !=
     HOB3_DRAM_TOTAL) ||

    (DramCursor !=
     HOB3_DRAM_END_EXCLUSIVE)
    )
  {
    ResourceOk = FALSE;
  }

  //
  // Find the stack allocation HOB.
  //
  StackHob =
    (EFI_HOB_MEMORY_ALLOCATION *)
    GetFirstHob(
      EFI_HOB_TYPE_MEMORY_ALLOCATION
      );

  StackOk =
    (BOOLEAN)(
      (StackHob != NULL) &&
      (StackHob->AllocDescriptor.MemoryBaseAddress ==
       MOTO_G20_STACK_BASE) &&
      (StackHob->AllocDescriptor.MemoryLength ==
       MOTO_G20_STACK_SIZE) &&
      (StackHob->AllocDescriptor.MemoryType ==
       EfiBootServicesData)
    );

  //
  // Find and validate the real CPU HOB.
  //
  CpuHob =
    (EFI_HOB_CPU *)
    GetFirstHob(
      EFI_HOB_TYPE_CPU
      );

  CpuOk =
    (BOOLEAN)(
      (CpuHob != NULL) &&
      (CpuHob->SizeOfMemorySpace ==
       PhysicalAddressBits) &&
      (CpuHob->SizeOfIoSpace == 16) &&
      (
        (PhysicalAddressBits == 32) ||
        (PhysicalAddressBits == 36) ||
        (PhysicalAddressBits == 40) ||
        (PhysicalAddressBits == 42) ||
        (PhysicalAddressBits == 44) ||
        (PhysicalAddressBits == 48) ||
        (PhysicalAddressBits == 52)
      )
    );

  //
  // Find our GUID HOB through the real library.
  //
  GuidHob =
    (EFI_HOB_GUID_TYPE *)
    GetFirstGuidHob(
      &mMotoG20RealHobGuid
      );

  ChainOk = TRUE;

  if (GuidHob == NULL) {
    ChainOk = FALSE;
  } else {
    Payload =
      (MOTO_G20_REAL_HOB_PAYLOAD *)
      (GuidHob + 1);

    if ((Payload->MagicA !=
         0x1122334455667788ULL) ||
        (Payload->MagicB !=
         0xDEADBEEFA55A5AA5ULL) ||
        (Payload->DtbAddress !=
         DtbAddress) ||
        (Payload->PhysicalAddressBits !=
         PhysicalAddressBits)) {
      ChainOk = FALSE;
    }
  }

  //
  // Independently walk the whole HOB chain.
  //
  Current =
    (EFI_HOB_GENERIC_HEADER *)HobList;

  HobCount = 0;

  while (HobCount < 32) {
    UINTN CurrentAddress;
    UINTN ArenaEnd;

    CurrentAddress = (UINTN)Current;
    ArenaEnd =
      MOTO_G20_HOB_BASE +
      MOTO_G20_HOB_SIZE;

    //
    // Never dereference a HOB header outside the HOB arena.
    //
    if (
      (CurrentAddress < MOTO_G20_HOB_BASE) ||
      (CurrentAddress > ArenaEnd) ||
      (
        (ArenaEnd - CurrentAddress) <
        sizeof(EFI_HOB_GENERIC_HEADER)
      )
      )
    {
      ChainOk = FALSE;
      break;
    }

    if (Current->HobType ==
        EFI_HOB_TYPE_END_OF_HOB_LIST) {
      break;
    }

    if (
      (Current->HobLength <
       sizeof(EFI_HOB_GENERIC_HEADER)) ||

      ((Current->HobLength & 7U) != 0) ||

      (
        Current->HobLength >
        (ArenaEnd - CurrentAddress)
      )
      )
    {
      ChainOk = FALSE;
      break;
    }

    Current =
      (EFI_HOB_GENERIC_HEADER *)(
        (UINT8 *)Current +
        Current->HobLength
        );

    HobCount++;
  }

  //
  // PHIT + 16 Resource + Stack + CPU + GUID +
  // six translation-table allocations = 26 HOBs,
  // then END_OF_HOB_LIST.
  //
  if ((HobCount != 26) ||
      (Current->HobType !=
       EFI_HOB_TYPE_END_OF_HOB_LIST) ||
      ((UINTN)Current !=
       (UINTN)HobList->EfiEndOfHobList)) {
    ChainOk = FALSE;
  }

  //
  // Five large markers.
  //

  // GREEN = PE relocation.
  FillRectangle(
    0, 0, 128, 128,
    RelocOk ? 0x0000FF00 : 0x00FF0000
    );

  // BLUE = real HobConstructor + published list.
  FillRectangle(
    144, 0, 128, 128,
    ConstructorOk ? 0x000000FF : 0x00FF0000
    );

  // CYAN = real Resource Descriptor HOB.
  FillRectangle(
    288, 0, 128, 128,
    ResourceOk ? 0x0000FFFF : 0x00FF0000
    );

  // WHITE = real Stack HOB.
  FillRectangle(
    432, 0, 128, 128,
    StackOk ? 0x00FFFFFF : 0x00FF0000
    );

  // YELLOW = complete HOB3 map + chain + CPU HOB + DTB handoff.
  FillRectangle(
    576, 0, 128, 128,
    (ChainOk && ResourceOk && CpuOk && DtbOk) ?
      0x00FFFF00 :
      0x00FF0000
    );

  //
  // Small PA-width marker at far right, below the yellow block.
  //
  FillRectangle(
    576, 144, 64, 64,
    CpuOk ?
      CpuPaBitsColor(PhysicalAddressBits) :
      0x00FF0000
    );

  //
  // MMU0_PREREQ_PROBE:
  //
  // GREEN   = EL2 classic, HCR_EL2.E2H == 0
  // MAGENTA = EL2 VHE,     HCR_EL2.E2H == 1
  // RED     = unexpected non-EL2 state
  //
  FillRectangle(
    656, 144, 48, 64,
    (CurrentEl != AARCH64_EL2) ?
      0x00FF0000 :
      (
        E2hEnabled ?
          0x00FF00FF :
          0x0000FF00
      )
    );

  // MMU2A = complete build-only translation-table integration and
  // software self-test.  This marker is intentionally separate from
  // all earlier MMU1/HOB markers.
  FillRectangle(
    MOTO_G20_MMU2A_MARKER_X, MOTO_G20_MMU2A_MARKER_Y, 48, 64,
    Mmu2aOk ?
      0x0000FF00 :
      0x00FF0000
    );

  // MMU2B_READ_ONLY_REGISTER_PROBE marker: x=656,y=384,width=48,height=64.
  // Green means all read-only state checks passed; red means probe failure.
  FillRectangle(
    656, 384, 48, 64,
    Mmu2bProbeOk ?
      0x0000FF00 :
      0x00FF0000
    );

  // MMU2C_TRANSLATION_REGISTER_PRELOAD marker: x=656,y=464,width=48,height=64.
  // Green means table validation, three write/readbacks, SCTLR preservation,
  // and the post-write software walk all passed with MMU and caches OFF.
  FillRectangle(
    MOTO_G20_MMU2C_MARKER_X, MOTO_G20_MMU2C_MARKER_Y, 48, 64,
    (Mmu2cOk && Mmu2aOk) ?
      0x0000FF00 :
      0x00FF0000
    );

  // MMU3 PRE-ENABLE state: yellow means the guarded preflight passed; red
  // means the assembly stub will refuse to write SCTLR_EL2.M.
  FillRectangle(
    MOTO_G20_MMU3_MARKER_X, MOTO_G20_MMU3_MARKER_Y, 48, 64,
    gMotoG20Mmu3PreEnableOk ?
      0x00FFFF00 :
      0x00FF0000
    );

  // LIVE values: exactly sixteen hexadecimal digits per captured register.
  MotoG20DrawText (16, 400, "LIVE EL", 0x00FFFFFF);
  MotoG20DrawHex64 (160, 400, (UINT64)CurrentEl, 0x00FFFFFF);
  MotoG20DrawText (16, 430, "HCR", 0x00FFFFFF);
  MotoG20DrawHex64 (160, 430, HcrEl2, 0x00FFFFFF);
  MotoG20DrawText (16, 460, "MMFR0", 0x00FFFFFF);
  MotoG20DrawHex64 (160, 460, LiveMmfr0, 0x00FFFFFF);
  MotoG20DrawText (16, 490, "SCTLR", 0x00FFFFFF);
  MotoG20DrawHex64 (160, 490, LiveSctlrEl2, 0x00FFFFFF);
  MotoG20DrawText (16, 520, "TCR", 0x00FFFFFF);
  MotoG20DrawHex64 (160, 520, LiveTcrEl2, 0x00FFFFFF);
  MotoG20DrawText (16, 550, "TTBR0", 0x00FFFFFF);
  MotoG20DrawHex64 (160, 550, LiveTtbr0El2, 0x00FFFFFF);
  MotoG20DrawText (16, 580, "MAIR", 0x00FFFFFF);
  MotoG20DrawHex64 (160, 580, LiveMairEl2, 0x00FFFFFF);
  MotoG20DrawText (16, 610, "CHK", 0x00FFFFFF);
  MotoG20DrawHex64 (160, 610, LiveChecksum, 0x00FFFFFF);

  // TARGET values are informational and are visually separated from LIVE.
  MotoG20DrawText (16, 680, "TARGET", 0x0000FFFF);
  MotoG20DrawText (16, 710, "MAIR", 0x0000FFFF);
  MotoG20DrawHex64 (160, 710, 0x00000000FFBB4400ULL, 0x0000FFFF);
  MotoG20DrawText (16, 740, "TCR", 0x0000FFFF);
  MotoG20DrawHex64 (160, 740, 0x0000000080823518ULL, 0x0000FFFF);
  MotoG20DrawText (16, 770, "TTBR0", 0x0000FFFF);
  MotoG20DrawHex64 (160, 770, 0x0000000081981000ULL, 0x0000FFFF);

  // Deterministic host-validated glyph/raster test patterns.
  MotoG20DrawText (16, 840, "HEXTEST", 0x0000FFFF);
  MotoG20DrawHexString (16, 870, "0123456789ABCDEF", 0x0000FFFF);
  MotoG20DrawHexString (16, 900, "FEDCBA9876543210", 0x0000FFFF);

  // MMU2C BEFORE/AFTER register snapshots.  Values remain informational;
  // only the explicit MMU2C readback and preservation booleans drive marker.
  MotoG20DrawText (16, 960, "BEFORE", 0x0000FFFF);
  MotoG20DrawText (16, 990, "SCTLR", 0x0000FFFF);
  MotoG20DrawHex64 (160, 990, BeforeSctlr, 0x0000FFFF);
  MotoG20DrawText (16, 1020, "TCR", 0x0000FFFF);
  MotoG20DrawHex64 (160, 1020, BeforeTcr, 0x0000FFFF);
  MotoG20DrawText (16, 1050, "TTBR", 0x0000FFFF);
  MotoG20DrawHex64 (160, 1050, BeforeTtbr0, 0x0000FFFF);
  MotoG20DrawText (16, 1080, "MAIR", 0x0000FFFF);
  MotoG20DrawHex64 (160, 1080, BeforeMair, 0x0000FFFF);

  MotoG20DrawText (16, 1140, "AFTER", 0x0000FFFF);
  MotoG20DrawText (16, 1170, "SCTLR", 0x0000FFFF);
  MotoG20DrawHex64 (160, 1170, AfterSctlr, 0x0000FFFF);
  MotoG20DrawText (16, 1200, "TCR", 0x0000FFFF);
  MotoG20DrawHex64 (160, 1200, AfterTcr, 0x0000FFFF);
  MotoG20DrawText (16, 1230, "TTBR", 0x0000FFFF);
  MotoG20DrawHex64 (160, 1230, AfterTtbr0, 0x0000FFFF);
  MotoG20DrawText (16, 1260, "MAIR", 0x0000FFFF);
  MotoG20DrawHex64 (160, 1260, AfterMair, 0x0000FFFF);
  MotoG20DrawText (16, 1290, "VBAR", 0x0000FFFF);
  MotoG20DrawHex64 (160, 1290, VbarEl2, 0x0000FFFF);
  MotoG20DrawText (16, 1320, "MMU3", 0x0000FFFF);
  MotoG20DrawHex64 (160, 1320, gMotoG20Mmu3PreEnableOk ? 1 : 0, 0x0000FFFF);
}

// MMU3B1 FIX #3: display layout only; reuse the proven hexadecimal renderer.
VOID EFIAPI
MotoG20DrawMmu3b1Diagnostics (
  IN UINT64 VbarBefore,
  IN UINT64 VbarAfter,
  IN UINT64 SctlrBefore,
  IN UINT64 SctlrAfter,
  IN UINT64 TcrDisplay,
  IN UINT64 Ttbr0Display,
  IN UINT64 MairDisplay
  )
{
  MotoG20DrawText (16, 24, "MMU3B1 LOCAL VBAR", 0x00FFFFFF);
  MotoG20DrawText (16, 70, "VBAR BEFORE", 0x00FFFFFF);
  MotoG20DrawHex64 (160, 90, VbarBefore, 0x00FFFFFF);
  MotoG20DrawText (16, 140, "VBAR AFTER", 0x00FFFFFF);
  MotoG20DrawHex64 (160, 160, VbarAfter, 0x00FFFFFF);
  MotoG20DrawText (16, 210, "SCTLR BEFORE", 0x00FFFFFF);
  MotoG20DrawHex64 (160, 230, SctlrBefore, 0x00FFFFFF);
  MotoG20DrawText (16, 280, "SCTLR AFTER", 0x00FFFFFF);
  MotoG20DrawHex64 (160, 300, SctlrAfter, 0x00FFFFFF);
  MotoG20DrawText (16, 350, "TCR", 0x00FFFFFF);
  MotoG20DrawHex64 (160, 370, TcrDisplay, 0x00FFFFFF);
  MotoG20DrawText (16, 420, "TTBR0", 0x00FFFFFF);
  MotoG20DrawHex64 (160, 440, Ttbr0Display, 0x00FFFFFF);
  MotoG20DrawText (16, 490, "MAIR", 0x00FFFFFF);
  MotoG20DrawHex64 (160, 510, MairDisplay, 0x00FFFFFF);
  MotoG20DrawText (16, 560, "TARGET VBAR", 0x0000FFFF);
  MotoG20DrawHex64 (160, 580, 0x0000000081004000ULL, 0x0000FFFF);
  MotoG20DrawText (16, 650, "MMU OFF", 0x0000FFFF);
  MotoG20DrawText (16, 720, "HEXTEST", 0x0000FFFF);
  MotoG20DrawHexString (16, 750, "0123456789ABCDEF", 0x0000FFFF);
  MotoG20DrawHexString (16, 780, "FEDCBA9876543210", 0x0000FFFF);
}

// MMU3B2: render only after the controlled exception returned through ERET.
VOID __attribute__((section (".mmu3b2_diagnostics"))) EFIAPI
MotoG20DrawMmu3b2Diagnostics (
  IN UINT64 BrkImm,
  IN UINT64 PreSpsel,
  IN UINT64 VectorId,
  IN UINT64 Esr,
  IN UINT64 ExpectedEsr,
  IN UINT64 ElrEntry,
  IN UINT64 ElrResume,
  IN UINT64 Spsr,
  IN UINT64 Far,
  IN UINT64 EntrySp,
  IN UINT64 Vbar,
  IN UINT64 Sctlr,
  IN UINT64 PostEret
  )
{
  // Keep the complete MMU3B2 panel away from the right-side marker column.
  // The one-line layout prevents label/value collisions at the physical font
  // scale and the cleared rectangle removes the Android background beneath it.
  FillRectangle (8, 820, 612, 480, 0x00000000);
  MotoG20DrawText (24, 836, "MMU3B2 CONTROLLED BRK", 0x00FFFFFF);
  MotoG20DrawText (24, 868, "BRK IMM", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 868, BrkImm, 0x00FFFFFF);
  MotoG20DrawText (24, 898, "PRE SPSEL", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 898, PreSpsel, 0x00FFFFFF);
  MotoG20DrawText (24, 928, "VECTOR ID", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 928, VectorId, 0x00FFFFFF);
  MotoG20DrawText (24, 958, "ESR EL2", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 958, Esr, 0x00FFFFFF);
  MotoG20DrawText (24, 988, "EXPECT ESR", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 988, ExpectedEsr, 0x00FFFFFF);
  MotoG20DrawText (24, 1018, "ELR ENTRY", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1018, ElrEntry, 0x00FFFFFF);
  MotoG20DrawText (24, 1048, "ELR RESUME", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1048, ElrResume, 0x00FFFFFF);
  MotoG20DrawText (24, 1078, "SPSR EL2", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1078, Spsr, 0x00FFFFFF);
  MotoG20DrawText (24, 1108, "FAR EL2", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1108, Far, 0x00FFFFFF);
  MotoG20DrawText (24, 1138, "SP ENTRY", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1138, EntrySp, 0x00FFFFFF);
  MotoG20DrawText (24, 1168, "VBAR EL2", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1168, Vbar, 0x00FFFFFF);
  MotoG20DrawText (24, 1198, "SCTLR EL2", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1198, Sctlr, 0x00FFFFFF);
  MotoG20DrawText (24, 1228, "POST ERET", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1228, PostEret, 0x00FFFFFF);
  MotoG20DrawText (24, 1258, "MMU", 0x0000FFFF);
  MotoG20DrawText (190, 1258, "OFF", 0x0000FFFF);
}

// TRUE MMU3 M-only page.  This renderer is called only after the stackless
// transition and the post-enable C-side data/stack checks have succeeded.
#if defined(__clang__)
#define MOTO_G20_TRUE_RENDER_ATTRIBUTE __attribute__((section(".mmu3true.text")))
#else
#define MOTO_G20_TRUE_RENDER_ATTRIBUTE __attribute__((section(".mmu3true.text"), optimize("Os")))
#endif
VOID EFIAPI MOTO_G20_TRUE_RENDER_ATTRIBUTE
MotoG20DrawTrueMmu3MOnlyDiagnostics (
  IN UINT64 SctlrBefore,
  IN UINT64 SctlrAfter,
  IN UINT64 Tcr,
  IN UINT64 Ttbr0,
  IN UINT64 Mair,
  IN UINT64 Vbar,
  IN UINT64 CurrentEl,
  IN UINT64 Hcr,
  IN UINT64 CodeCookie,
  IN UINT64 DataCookie,
  IN UINT64 StackCookie,
  IN UINT64 RenderCookie,
  IN UINT64 FinalCookie,
  IN UINT64 Phase
  )
{
  FillRectangle (8, 820, 612, 690, 0x00000000);
  MotoG20DrawText (24, 836, "TRUE MMU3 M ONLY", 0x00FFFFFF);
  MotoG20DrawText (24, 868, "SCTLR BEFORE", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 868, SctlrBefore, 0x00FFFFFF);
  MotoG20DrawText (24, 898, "SCTLR AFTER", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 898, SctlrAfter, 0x00FFFFFF);
  MotoG20DrawText (24, 928, "TCR EL2", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 928, Tcr, 0x00FFFFFF);
  MotoG20DrawText (24, 958, "TTBR0 EL2", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 958, Ttbr0, 0x00FFFFFF);
  MotoG20DrawText (24, 988, "MAIR EL2", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 988, Mair, 0x00FFFFFF);
  MotoG20DrawText (24, 1018, "VBAR EL2", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1018, Vbar, 0x00FFFFFF);
  MotoG20DrawText (24, 1048, "CURRENT EL", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1048, CurrentEl, 0x00FFFFFF);
  MotoG20DrawText (24, 1078, "HCR EL2", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1078, Hcr, 0x00FFFFFF);
  MotoG20DrawText (24, 1108, "M", 0x0000FFFF);
  MotoG20DrawHex64 (190, 1108, 1, 0x0000FFFF);
  MotoG20DrawText (24, 1138, "C", 0x0000FFFF);
  MotoG20DrawHex64 (190, 1138, 0, 0x0000FFFF);
  MotoG20DrawText (24, 1168, "I", 0x0000FFFF);
  MotoG20DrawHex64 (190, 1168, 0, 0x0000FFFF);
  MotoG20DrawText (24, 1198, "CODE COOKIE", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1198, CodeCookie, 0x00FFFFFF);
  MotoG20DrawText (24, 1228, "DATA COOKIE", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1228, DataCookie, 0x00FFFFFF);
  MotoG20DrawText (24, 1258, "STACK COOKIE", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1258, StackCookie, 0x00FFFFFF);
  MotoG20DrawText (24, 1288, "RENDER COOKIE", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1288, RenderCookie, 0x00FFFFFF);
  MotoG20DrawText (24, 1318, "FINAL COOKIE", 0x00FFFFFF);
  MotoG20DrawHex64 (190, 1318, FinalCookie, 0x00FFFFFF);
  MotoG20DrawText (24, 1348, "PHASE", 0x0000FFFF);
  MotoG20DrawHex64 (190, 1348, Phase, 0x0000FFFF);
  MotoG20DrawText (24, 1378, "MMU", 0x0000FFFF);
  MotoG20DrawText (190, 1378, "ON", 0x0000FFFF);
  MotoG20DrawText (24, 1408, "CACHES", 0x0000FFFF);
  MotoG20DrawText (190, 1408, "OFF", 0x0000FFFF);
}
