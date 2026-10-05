/** @file
  Allocation-free DXE telemetry, framebuffer diagnostics and bounded recovery.

  The persistent record is written to ramoops dmesg zone 4.  Its signature is the
  final store, and no ramoops byte is touched until the exact active EL2 map
  has been validated.  Zone 5, used by the frozen pre/post-MMU writer, is not
  modified by this library.
**/

#include <Base.h>
#include <AArch64Mmu.h>
#include <IndustryStandard/ArmStdSmc.h>
#include <Library/ArmSmcLib.h>
#include <Library/MotoG20DxeTelemetryLib.h>

#define MOTO_G20_FB_BASE                  0x9E000000ULL
#define MOTO_G20_FB_WIDTH                 720U
#define MOTO_G20_FB_HEIGHT                1600U
#define MOTO_G20_FB_STRIDE                720U

#define MOTO_G20_FONT_WIDTH               5U
#define MOTO_G20_FONT_HEIGHT              7U
#define MOTO_G20_FONT_SCALE               2U
#define MOTO_G20_GLYPH_WIDTH              10U
#define MOTO_G20_GLYPH_HEIGHT             14U
#define MOTO_G20_GLYPH_ADVANCE            12U
#define MOTO_G20_ROW_FIRST_Y              144U
#define MOTO_G20_ROW_PITCH                48U
#define MOTO_G20_LABEL_X                  32U
#define MOTO_G20_LABEL_CELLS              18U
#define MOTO_G20_VALUE_X                  280U
#define MOTO_G20_VALUE_CELLS              22U
#define MOTO_G20_MARKER_X                 672U
#define MOTO_G20_MARKER_WIDTH             40U
#define MOTO_G20_MARKER_HEIGHT            28U
#define MOTO_G20_PSTORE_ROW_INDEX         24U

#define MOTO_G20_COLOR_BLACK              0x00000000U
#define MOTO_G20_COLOR_WHITE              0x00FFFFFFU
#define MOTO_G20_COLOR_CYAN               0x0000FFFFU
#define MOTO_G20_COLOR_GREEN              0x0000FF00U
#define MOTO_G20_COLOR_ORANGE             0x000080FFU
#define MOTO_G20_COLOR_RED                0x00FF0000U
#define MOTO_G20_COLOR_DARK_BLUE          0x00201008U

#define MOTO_G20_RAMOOPS_BASE             0xFFF80000ULL
#define MOTO_G20_RAMOOPS_SIZE             0x00040000ULL
#define MOTO_G20_RAMOOPS_END              (MOTO_G20_RAMOOPS_BASE + MOTO_G20_RAMOOPS_SIZE)
#define MOTO_G20_RAMOOPS_RECORD_SIZE      0x00008000ULL
#define MOTO_G20_DXE_ZONE_INDEX           4U
#define MOTO_G20_DXE_ZONE_BASE            \
  (MOTO_G20_RAMOOPS_BASE + (MOTO_G20_DXE_ZONE_INDEX * MOTO_G20_RAMOOPS_RECORD_SIZE))
#define MOTO_G20_DXE_ZONE_SIZE            0x00008000U
#define MOTO_G20_PSTORE_SIGNATURE         0x43474244U
#define MOTO_G20_DXE_FRAME_SIZE           256U
#define MOTO_G20_DXE_PAYLOAD_SIZE         (8U + MOTO_G20_DXE_FRAME_SIZE)

#define MOTO_G20_EXPECTED_TCR_EL2         0x0000000080823518ULL
#define MOTO_G20_EXPECTED_TTBR0_EL2       0x0000000081981000ULL
#define MOTO_G20_EXPECTED_MAIR_EL2        0x00000000FFBB4400ULL
#define MOTO_G20_TABLE_BASE               0x8197A000ULL
#define MOTO_G20_TABLE_LIMIT              0x81982000ULL
#define MOTO_G20_PAGE_SIZE                0x1000ULL
#define MOTO_G20_PAGE_MASK                (MOTO_G20_PAGE_SIZE - 1U)
#define MOTO_G20_TABLE_INDEX_MASK         0x1FFULL
#define MOTO_G20_ADDRESS_MASK             0x000000FFFFFFFFFFULL

#define MOTO_G20_RECORD_FLAG_EXCEPTION    0x00000001U
#define MOTO_G20_RECORD_FLAG_ASSERT       0x00000002U
#define MOTO_G20_RECORD_FLAG_FAILURE      0x00000004U

#define MOTO_G20_ENTRY_FLAG_RANGE_VALID        0x00000001U
#define MOTO_G20_ENTRY_FLAG_PRE_CAPTURED       0x00000002U
#define MOTO_G20_ENTRY_FLAG_CACHE_SYNCED       0x00000004U
#define MOTO_G20_ENTRY_FLAG_POST_CAPTURED      0x00000008U
#define MOTO_G20_ENTRY_FLAG_CODE_STABLE        0x00000010U
#define MOTO_G20_ENTRY_FLAG_ALIGNED            0x00000020U
#define MOTO_G20_ENTRY_FLAG_FAULT_IN_IMAGE     0x00000040U
#define MOTO_G20_ENTRY_FLAG_FAULT_CAPTURED     0x00000080U
#define MOTO_G20_ENTRY_FLAG_FAULT_AT_ENTRY     0x00000100U

#define MOTO_G20_PSTORE_COMMIT_PASS             0U
#define MOTO_G20_PSTORE_COMMIT_MAP_REJECTED     1U
#define MOTO_G20_PSTORE_COMMIT_READBACK_REJECTED 2U

#define M3DXE_OFF_MAGIC                   0U
#define M3DXE_OFF_VERSION                 8U
#define M3DXE_OFF_SIZE                    12U
#define M3DXE_OFF_SEQUENCE                16U
#define M3DXE_OFF_STAGE                   24U
#define M3DXE_OFF_FLAGS                   28U
#define M3DXE_OFF_STATUS                  32U
#define M3DXE_OFF_DETAIL0                 40U
#define M3DXE_OFF_DETAIL1                 48U
#define M3DXE_OFF_CURRENT_EL              56U
#define M3DXE_OFF_SCTLR_EL2               64U
#define M3DXE_OFF_TCR_EL2                 72U
#define M3DXE_OFF_TTBR0_EL2               80U
#define M3DXE_OFF_MAIR_EL2                88U
#define M3DXE_OFF_VBAR_EL2                96U
#define M3DXE_OFF_ESR_EL2                 104U
#define M3DXE_OFF_ELR_EL2                 112U
#define M3DXE_OFF_FAR_EL2                 120U
#define M3DXE_OFF_SPSR_EL2                128U
#define M3DXE_OFF_SP                      136U
#define M3DXE_OFF_LR                      144U
#define M3DXE_OFF_FP                      152U
#define M3DXE_OFF_EXCEPTION_TYPE          160U
#define M3DXE_OFF_LINE                    168U
#define M3DXE_OFF_FILE_HASH               176U
#define M3DXE_OFF_DESCRIPTION_HASH        184U
#define M3DXE_OFF_TICKS                   192U
#define M3DXE_OFF_ENTRY_IMAGE_BASE        200U
#define M3DXE_OFF_ENTRY_IMAGE_SIZE        208U
#define M3DXE_OFF_ENTRY_POINT             216U
#define M3DXE_OFF_ENTRY_CODE_PRE          224U
#define M3DXE_OFF_ENTRY_CODE_POST         232U
#define M3DXE_OFF_ENTRY_FAULT_CODE        240U
#define M3DXE_OFF_ENTRY_FLAGS             244U
#define M3DXE_OFF_ENTRY_SEQUENCE          248U
#define M3DXE_OFF_CRC32                   252U

STATIC_ASSERT (MOTO_G20_DXE_ZONE_BASE == 0xFFFA0000ULL, "DXE pstore zone drifted");
STATIC_ASSERT (MOTO_G20_DXE_ZONE_SIZE == MOTO_G20_RAMOOPS_RECORD_SIZE, "DXE pstore size drifted");

typedef struct {
  CHAR8  Character;
  UINT8  Rows[MOTO_G20_FONT_HEIGHT];
} MOTO_G20_GLYPH;

typedef enum {
  MotoG20LookupInvalid,
  MotoG20LookupUnmapped,
  MotoG20LookupMapped
} MOTO_G20_LOOKUP_STATE;

typedef struct {
  MOTO_G20_LOOKUP_STATE  State;
  UINTN                  Level;
  UINT64                 Descriptor;
  UINT64                 PhysicalAddress;
} MOTO_G20_LOOKUP;

STATIC CONST MOTO_G20_GLYPH  mFont[] = {
  { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
  { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
  { ':', { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00 } },
  { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06 } },
  { '/', { 0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10 } },
  { '_', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F } },
  { '?', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 } },
  { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
  { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
  { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
  { '3', { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E } },
  { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
  { '5', { 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E } },
  { '6', { 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
  { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
  { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
  { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E } },
  { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
  { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
  { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
  { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
  { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
  { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
  { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
  { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
  { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
  { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
  { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
  { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
  { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
  { 'N', { 0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11 } },
  { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
  { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
  { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
  { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
  { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
  { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
  { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
  { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
  { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } },
  { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
  { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
  { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } }
};

STATIC CONST UINT8  mMagic[8] = { 'M', '3', 'D', 'X', 'E', '0', '0', '2' };
STATIC CONST UINT8  mKmsgHeader[8] = { '=', '=', '=', '=', '0', '.', '0', '\n' };
STATIC BOOLEAN      mDisplayInitialized;
STATIC BOOLEAN      mDisplayReleased;
STATIC BOOLEAN      mPublishing;
STATIC UINT64       mSequence;
STATIC UINT64       mEntryImageId;
STATIC UINT64       mEntryImageBase;
STATIC UINT64       mEntryImageSize;
STATIC UINT64       mEntryPoint;
STATIC UINT64       mEntryCodePre;
STATIC UINT64       mEntryCodePost;
STATIC UINT32       mEntryFaultCode;
STATIC UINT32       mEntryFlags;
STATIC UINT32       mEntrySequence;

STATIC
VOID
Barrier (
  VOID
  )
{
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

STATIC
UINT64
ReadCurrentEl (
  VOID
  )
{
  UINT64  Value;
  __asm__ __volatile__ ("mrs %0, CurrentEL" : "=r" (Value));
  return Value;
}

#define MOTO_G20_READ_SYSREG(Name, Register) \
  STATIC UINT64 Name (VOID) { UINT64 Value; __asm__ __volatile__ ("mrs %0, " Register : "=r" (Value)); return Value; }

MOTO_G20_READ_SYSREG (ReadSctlrEl2, "SCTLR_EL2")
MOTO_G20_READ_SYSREG (ReadTcrEl2,   "TCR_EL2")
MOTO_G20_READ_SYSREG (ReadTtbr0El2, "TTBR0_EL2")
MOTO_G20_READ_SYSREG (ReadMairEl2,  "MAIR_EL2")
MOTO_G20_READ_SYSREG (ReadVbarEl2,  "VBAR_EL2")
MOTO_G20_READ_SYSREG (ReadCntfrq,   "CNTFRQ_EL0")
MOTO_G20_READ_SYSREG (ReadCntpct,   "CNTPCT_EL0")

STATIC
UINT64
ReadStackPointer (
  VOID
  )
{
  UINT64  Value;
  __asm__ __volatile__ ("mov %0, sp" : "=r" (Value));
  return Value;
}

STATIC
VOID
ZeroBytes (
  OUT UINT8  *Buffer,
  IN  UINTN  Size
  )
{
  UINTN  Index;
  for (Index = 0; Index < Size; Index++) {
    Buffer[Index] = 0;
  }
}

STATIC
VOID
Put32 (
  OUT UINT8   *Buffer,
  IN  UINT32  Value
  )
{
  UINTN  Index;
  for (Index = 0; Index < 4U; Index++) {
    Buffer[Index] = (UINT8)(Value >> (Index * 8U));
  }
}

STATIC
VOID
Put64 (
  OUT UINT8   *Buffer,
  IN  UINT64  Value
  )
{
  UINTN  Index;
  for (Index = 0; Index < 8U; Index++) {
    Buffer[Index] = (UINT8)(Value >> (Index * 8U));
  }
}

STATIC
UINT32
Crc32 (
  IN CONST UINT8  *Buffer,
  IN UINTN        Size
  )
{
  UINT32  Crc;
  UINTN   ByteIndex;
  UINTN   BitIndex;

  Crc = 0xFFFFFFFFU;
  for (ByteIndex = 0; ByteIndex < Size; ByteIndex++) {
    Crc ^= Buffer[ByteIndex];
    for (BitIndex = 0; BitIndex < 8U; BitIndex++) {
      Crc = (Crc >> 1) ^ ((0U - (Crc & 1U)) & 0xEDB88320U);
    }
  }

  return ~Crc;
}

STATIC
UINT64
HashAscii (
  IN CONST CHAR8  *Text
  )
{
  UINT64  Hash;
  UINTN   Index;

  Hash = 0xCBF29CE484222325ULL;
  if (Text == NULL) {
    return Hash;
  }

  for (Index = 0; (Index < 1024U) && (Text[Index] != '\0'); Index++) {
    Hash ^= (UINT8)Text[Index];
    Hash *= 0x100000001B3ULL;
  }

  return Hash;
}

STATIC
BOOLEAN
EntryRangeContains (
  IN UINT64  Address,
  IN UINT64  RequiredSize
  )
{
  UINT64  Offset;

  if ((mEntryImageBase == 0U) ||
      (RequiredSize == 0U) ||
      (mEntryImageSize < RequiredSize) ||
      (Address < mEntryImageBase)) {
    return FALSE;
  }

  Offset = Address - mEntryImageBase;
  return (BOOLEAN)(Offset <= (mEntryImageSize - RequiredSize));
}

STATIC
UINT64
HashEntryImageName (
  IN CONST CHAR8  *Text OPTIONAL
  )
{
  UINT64  Address;
  UINT64  Hash;
  UINT64  Remaining;
  UINTN   Index;
  UINTN   Limit;

  Address = (UINT64)(UINTN)Text;
  if ((Text == NULL) || !EntryRangeContains (Address, 1U)) {
    return 0U;
  }

  Remaining = mEntryImageSize - (Address - mEntryImageBase);
  Limit     = (Remaining < 1024U) ? (UINTN)Remaining : 1024U;
  Hash      = 0xCBF29CE484222325ULL;
  for (Index = 0U; Index < Limit; Index++) {
    if (Text[Index] == '\0') {
      return Hash;
    }

    Hash ^= (UINT8)Text[Index];
    Hash *= 0x100000001B3ULL;
  }

  // An unterminated or out-of-image name must not become an authoritative ID.
  return 0U;
}

STATIC
UINT64
ReadEntry64 (
  IN UINT64  Address
  )
{
  volatile CONST UINT8  *Source;
  UINT64                Value;
  UINTN                 Index;

  Source = (volatile CONST UINT8 *)(UINTN)Address;
  Value  = 0U;
  for (Index = 0U; Index < sizeof (Value); Index++) {
    Value |= ((UINT64)Source[Index] << (Index * 8U));
  }

  return Value;
}

STATIC
UINT32
ReadEntry32 (
  IN UINT64  Address
  )
{
  volatile CONST UINT8  *Source;
  UINT32                Value;
  UINTN                 Index;

  Source = (volatile CONST UINT8 *)(UINTN)Address;
  Value  = 0U;
  for (Index = 0U; Index < sizeof (Value); Index++) {
    Value |= ((UINT32)Source[Index] << (Index * 8U));
  }

  return Value;
}

STATIC
UINT32
CaptureFaultInstruction (
  IN  UINT64  Elr,
  OUT UINT32  *EntryFlags
  )
{
  UINT32  FaultCode;

  if ((EntryFlags == NULL) ||
      ((*EntryFlags & MOTO_G20_ENTRY_FLAG_POST_CAPTURED) == 0U) ||
      !EntryRangeContains (Elr, sizeof (FaultCode))) {
    return 0U;
  }

  *EntryFlags |= MOTO_G20_ENTRY_FLAG_FAULT_IN_IMAGE;
  FaultCode    = ReadEntry32 (Elr);
  *EntryFlags |= MOTO_G20_ENTRY_FLAG_FAULT_CAPTURED;
  if (Elr == mEntryPoint) {
    *EntryFlags |= MOTO_G20_ENTRY_FLAG_FAULT_AT_ENTRY;
  }

  return FaultCode;
}

STATIC
VOID
FillRectangle (
  IN UINTN   X,
  IN UINTN   Y,
  IN UINTN   Width,
  IN UINTN   Height,
  IN UINT32  Color
  )
{
  volatile UINT32  *Framebuffer;
  UINTN             Row;
  UINTN             Column;

  if ((Width == 0U) || (Height == 0U) ||
      (X >= MOTO_G20_FB_WIDTH) || (Y >= MOTO_G20_FB_HEIGHT) ||
      (Width > (MOTO_G20_FB_WIDTH - X)) ||
      (Height > (MOTO_G20_FB_HEIGHT - Y))) {
    return;
  }

  Framebuffer = (volatile UINT32 *)(UINTN)MOTO_G20_FB_BASE;
  for (Row = 0; Row < Height; Row++) {
    for (Column = 0; Column < Width; Column++) {
      Framebuffer[((Y + Row) * MOTO_G20_FB_STRIDE) + X + Column] = Color;
    }
  }
}

STATIC
CONST UINT8 *
FindGlyph (
  IN CHAR8  Character
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mFont); Index++) {
    if (mFont[Index].Character == Character) {
      return mFont[Index].Rows;
    }
  }

  for (Index = 0; Index < ARRAY_SIZE (mFont); Index++) {
    if (mFont[Index].Character == '?') {
      return mFont[Index].Rows;
    }
  }

  return mFont[0].Rows;
}

STATIC
VOID
DrawGlyph (
  IN UINTN   X,
  IN UINTN   Y,
  IN CHAR8   Character,
  IN UINT32  Color
  )
{
  CONST UINT8  *Rows;
  UINTN        Row;
  UINTN        Column;

  if ((X > (MOTO_G20_FB_WIDTH - MOTO_G20_GLYPH_WIDTH)) ||
      (Y > (MOTO_G20_FB_HEIGHT - MOTO_G20_GLYPH_HEIGHT))) {
    return;
  }

  Rows = FindGlyph (Character);
  for (Row = 0; Row < MOTO_G20_FONT_HEIGHT; Row++) {
    for (Column = 0; Column < MOTO_G20_FONT_WIDTH; Column++) {
      if ((Rows[Row] & (1U << (MOTO_G20_FONT_WIDTH - 1U - Column))) != 0U) {
        FillRectangle (
          X + (Column * MOTO_G20_FONT_SCALE),
          Y + (Row * MOTO_G20_FONT_SCALE),
          MOTO_G20_FONT_SCALE,
          MOTO_G20_FONT_SCALE,
          Color
          );
      }
    }
  }
}

STATIC
VOID
DrawString (
  IN UINTN        X,
  IN UINTN        Y,
  IN UINTN        MaximumCharacters,
  IN CONST CHAR8  *Text,
  IN UINT32       Color
  )
{
  UINTN  Index;

  if ((Text == NULL) || (MaximumCharacters == 0U) ||
      (MaximumCharacters > (MOTO_G20_FB_WIDTH / MOTO_G20_GLYPH_ADVANCE)) ||
      (X > (MOTO_G20_FB_WIDTH - (MaximumCharacters * MOTO_G20_GLYPH_ADVANCE))) ||
      (Y > (MOTO_G20_FB_HEIGHT - MOTO_G20_GLYPH_HEIGHT))) {
    return;
  }

  FillRectangle (
    X,
    Y,
    MaximumCharacters * MOTO_G20_GLYPH_ADVANCE,
    MOTO_G20_GLYPH_HEIGHT,
    MOTO_G20_COLOR_BLACK
    );
  for (Index = 0; (Index < MaximumCharacters) && (Text[Index] != '\0'); Index++) {
    DrawGlyph (X + (Index * MOTO_G20_GLYPH_ADVANCE), Y, Text[Index], Color);
  }
}

STATIC
VOID
DrawHex64 (
  IN UINTN   X,
  IN UINTN   Y,
  IN UINT64  Value,
  IN UINT32  Color
  )
{
  STATIC CONST CHAR8  Hex[] = "0123456789ABCDEF";
  CHAR8               Buffer[17];
  UINTN               Index;

  for (Index = 0; Index < 16U; Index++) {
    Buffer[Index] = Hex[(Value >> ((15U - Index) * 4U)) & 0xFU];
  }

  Buffer[16] = '\0';
  DrawString (X, Y, MOTO_G20_VALUE_CELLS, Buffer, Color);
}

STATIC
CONST CHAR8 *
StageName (
  IN MOTO_G20_DXE_STAGE  Stage
  )
{
  switch (Stage) {
    case MotoG20DxeStageCoreEntry:           return "DXE CORE ENTRY";
    case MotoG20DxeStageMainEntry:           return "DXE MAIN ENTRY";
    case MotoG20DxeStageExceptionsReady:     return "EXCEPTIONS READY";
    case MotoG20DxeStageMemoryServices:      return "MEMORY SERVICES";
    case MotoG20DxeStageHandleServices:      return "HANDLE SERVICES";
    case MotoG20DxeStageImageServices:       return "IMAGE SERVICES";
    case MotoG20DxeStageGcdServices:         return "GCD SERVICES";
    case MotoG20DxeStageSystemTables:        return "SYSTEM TABLES";
    case MotoG20DxeStageConstructors:        return "CONSTRUCTORS";
    case MotoG20DxeStageEvents:              return "EVENT SERVICES";
    case MotoG20DxeStageFirmwareVolumes:     return "FIRMWARE VOLUMES";
    case MotoG20DxeStageDispatcherReady:     return "DISPATCHER READY";
    case MotoG20DxeStageDispatcherEnter:     return "DISPATCHER ENTER";
    case MotoG20DxeStageDispatcherReturned:  return "DISPATCHER RETURNED";
    case MotoG20DxeStageArchitecturalReady:  return "ARCH PROTOCOLS READY";
    case MotoG20DxeStageBdsTransfer:         return "BDS TRANSFER";
    case MotoG20DxeStageProgressMonitor:     return "PROGRESS MONITOR";
    case MotoG20DxeStageCpuArch:             return "CPU ARCH";
    case MotoG20DxeStageInterruptArch:       return "INTERRUPT ARCH";
    case MotoG20DxeStageTimerArch:           return "TIMER ARCH";
    case MotoG20DxeStageGraphicsOutput:      return "GRAPHICS OUTPUT";
    case MotoG20DxeStageTextOutput:          return "TEXT OUTPUT";
    case MotoG20DxeStageBdsArch:             return "BDS ARCH";
    case MotoG20DxeStageBeforeConsole:       return "BEFORE CONSOLE";
    case MotoG20DxeStageConsoleReady:        return "CONSOLE READY";
    case MotoG20DxeStageShellLoad:           return "SHELL LOAD CALL";
    case MotoG20DxeStageShellStart:          return "SHELL START CALL";
    case MotoG20DxeStageBdsEntry: return "BDS ENTRY";
    case MotoG20DxeStageConOutConnectCall: return "CONOUT CONNECT CALL";
    case MotoG20DxeStageConOutConnectReturn: return "CONOUT CONNECT RETURN";
    case MotoG20DxeStageErrOutConnectCall: return "ERROUT CONNECT CALL";
    case MotoG20DxeStageErrOutConnectReturn: return "ERROUT CONNECT RETURN";
    case MotoG20DxeStagePhysicalConsole: return "PHYSICAL TEXT CHECK";
    case MotoG20DxeStageConsoleWrite: return "CONSOLE WRITE";
    case MotoG20DxeStageShellLoadReturn: return "SHELL LOAD RETURN";
    case MotoG20DxeStageShellReturn: return "SHELL RETURN";
    case MotoG20DxeStageCoreReturned:        return "DXE CORE RETURNED";
    case MotoG20DxeStageException:           return "DXE EXCEPTION";
    case MotoG20DxeStageAssert:              return "DXE ASSERT";
    case MotoG20DxeStageFailure:             return "DXE FAILURE";
    case MotoG20DxeStageArchitecturalMissing:return "ARCH MISSING";
    default:                                 return "UNKNOWN";
  }
}

STATIC
VOID
InitializePanel (
  VOID
  )
{
  STATIC CONST CHAR8  *Labels[] = {
    "STAGE", "STATUS", "DETAIL 0", "DETAIL 1", "CURRENT EL",
    "SCTLR EL2", "TCR EL2", "TTBR0 EL2", "MAIR EL2", "VBAR EL2",
    "ESR EL2", "ELR EL2", "FAR EL2", "SP", "LR", "IMAGE ID",
    "IMAGE BASE", "IMAGE SIZE", "ENTRY POINT", "ENTRY CODE PRE",
    "ENTRY CODE POST", "FAULT CODE", "ENTRY FLAGS", "ENTRY SEQUENCE",
    "PSTORE CODE"
  };
  UINTN  Index;

  FillRectangle (0U, 0U, MOTO_G20_FB_WIDTH, MOTO_G20_FB_HEIGHT, MOTO_G20_COLOR_BLACK);
  FillRectangle (0U, 0U, MOTO_G20_FB_WIDTH, 112U, MOTO_G20_COLOR_DARK_BLUE);
  DrawString (32U, 24U, 36U, "MOTO G20 UEFI DXE TRACE", MOTO_G20_COLOR_WHITE);
  DrawString (32U, 64U, 36U, "BOUNDED NO OVERLAP PANEL", MOTO_G20_COLOR_CYAN);
  for (Index = 0; Index < ARRAY_SIZE (Labels); Index++) {
    DrawString (
      MOTO_G20_LABEL_X,
      MOTO_G20_ROW_FIRST_Y + (Index * MOTO_G20_ROW_PITCH),
      MOTO_G20_LABEL_CELLS,
      Labels[Index],
      MOTO_G20_COLOR_WHITE
      );
  }

  DrawString (32U, 1472U, 52U, "ENTRY GATE AND PSTORE INSTRUMENTED", MOTO_G20_COLOR_CYAN);
  mDisplayInitialized = TRUE;
}

STATIC
VOID
RenderRecord (
  IN MOTO_G20_DXE_STAGE  Stage,
  IN EFI_STATUS          Status,
  IN UINT64              Detail0,
  IN UINT64              Detail1,
  IN UINT64              CurrentEl,
  IN UINT64              SctlrEl2,
  IN UINT64              TcrEl2,
  IN UINT64              Ttbr0El2,
  IN UINT64              MairEl2,
  IN UINT64              VbarEl2,
  IN UINT64              EsrEl2,
  IN UINT64              ElrEl2,
  IN UINT64              FarEl2,
  IN UINT64              StackPointer,
  IN UINT64              LinkRegister,
  IN BOOLEAN             Failure
  )
{
  UINT32  Color;

  if (!mDisplayInitialized) {
    InitializePanel ();
  }

  Color = Failure ? MOTO_G20_COLOR_RED : MOTO_G20_COLOR_CYAN;
  DrawString (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y, MOTO_G20_VALUE_CELLS, StageName (Stage), Color);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (1U * MOTO_G20_ROW_PITCH), (UINT64)Status, Color);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (2U * MOTO_G20_ROW_PITCH), Detail0, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (3U * MOTO_G20_ROW_PITCH), Detail1, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (4U * MOTO_G20_ROW_PITCH), CurrentEl, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (5U * MOTO_G20_ROW_PITCH), SctlrEl2, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (6U * MOTO_G20_ROW_PITCH), TcrEl2, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (7U * MOTO_G20_ROW_PITCH), Ttbr0El2, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (8U * MOTO_G20_ROW_PITCH), MairEl2, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (9U * MOTO_G20_ROW_PITCH), VbarEl2, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (10U * MOTO_G20_ROW_PITCH), EsrEl2, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (11U * MOTO_G20_ROW_PITCH), ElrEl2, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (12U * MOTO_G20_ROW_PITCH), FarEl2, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (13U * MOTO_G20_ROW_PITCH), StackPointer, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (14U * MOTO_G20_ROW_PITCH), LinkRegister, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (15U * MOTO_G20_ROW_PITCH), mEntryImageId, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (16U * MOTO_G20_ROW_PITCH), mEntryImageBase, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (17U * MOTO_G20_ROW_PITCH), mEntryImageSize, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (18U * MOTO_G20_ROW_PITCH), mEntryPoint, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (19U * MOTO_G20_ROW_PITCH), mEntryCodePre, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (20U * MOTO_G20_ROW_PITCH), mEntryCodePost, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (21U * MOTO_G20_ROW_PITCH), (UINT64)mEntryFaultCode, MOTO_G20_COLOR_GREEN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (22U * MOTO_G20_ROW_PITCH), (UINT64)mEntryFlags, MOTO_G20_COLOR_CYAN);
  DrawHex64 (MOTO_G20_VALUE_X, MOTO_G20_ROW_FIRST_Y + (23U * MOTO_G20_ROW_PITCH), (UINT64)mEntrySequence, MOTO_G20_COLOR_CYAN);
  FillRectangle (
    MOTO_G20_MARKER_X,
    MOTO_G20_ROW_FIRST_Y,
    MOTO_G20_MARKER_WIDTH,
    MOTO_G20_MARKER_HEIGHT,
    Failure ? MOTO_G20_COLOR_RED : MOTO_G20_COLOR_GREEN
    );
  if (Failure) {
    DrawString (32U, 1472U, 52U, "AUTO RECOVERY IN 20 SECONDS", MOTO_G20_COLOR_RED);
  }

  Barrier ();
}

STATIC
BOOLEAN
TableAddressAllowed (
  IN UINT64  Address
  )
{
  return (BOOLEAN)(
           (Address >= MOTO_G20_TABLE_BASE) &&
           (Address < MOTO_G20_TABLE_LIMIT) &&
           ((Address & MOTO_G20_PAGE_MASK) == 0U)
           );
}

STATIC
UINTN
LevelShift (
  IN UINTN  Level
  )
{
  return 12U + ((3U - Level) * 9U);
}

STATIC
VOID
StrictLookup (
  IN  UINT64             VirtualAddress,
  OUT MOTO_G20_LOOKUP    *Result
  )
{
  volatile CONST UINT64  *Table;
  UINT64                 Descriptor;
  UINT64                 TableAddress;
  UINT64                 BlockSize;
  UINT64                 BlockMask;
  UINTN                  Index;
  UINTN                  Level;

  Result->State           = MotoG20LookupInvalid;
  Result->Level           = 0U;
  Result->Descriptor      = 0U;
  Result->PhysicalAddress = 0U;
  if ((VirtualAddress & ~MOTO_G20_ADDRESS_MASK) != 0U) {
    return;
  }

  Table = (volatile CONST UINT64 *)(UINTN)MOTO_G20_EXPECTED_TTBR0_EL2;
  for (Level = 0U; Level < 4U; Level++) {
    Index      = (UINTN)((VirtualAddress >> LevelShift (Level)) & MOTO_G20_TABLE_INDEX_MASK);
    Descriptor = Table[Index];
    Result->Level      = Level;
    Result->Descriptor = Descriptor;
    if (Descriptor == 0U) {
      Result->State = MotoG20LookupUnmapped;
      return;
    }

    if (Level == 3U) {
      if ((Descriptor & TT_TYPE_MASK) != TT_TYPE_BLOCK_ENTRY_LEVEL3) {
        return;
      }

      Result->State = MotoG20LookupMapped;
      Result->PhysicalAddress =
        (Descriptor & TT_ADDRESS_MASK_BLOCK_ENTRY) |
        (VirtualAddress & MOTO_G20_PAGE_MASK);
      return;
    }

    if ((Descriptor & TT_TYPE_MASK) == TT_TYPE_TABLE_ENTRY) {
      TableAddress = Descriptor & TT_ADDRESS_MASK_DESCRIPTION_TABLE;
      if (!TableAddressAllowed (TableAddress)) {
        return;
      }

      Table = (volatile CONST UINT64 *)(UINTN)TableAddress;
      continue;
    }

    if ((Level == 0U) || ((Descriptor & TT_TYPE_MASK) != TT_TYPE_BLOCK_ENTRY)) {
      return;
    }

    BlockSize = 1ULL << LevelShift (Level);
    BlockMask = BlockSize - 1U;
    Result->State = MotoG20LookupMapped;
    Result->PhysicalAddress =
      (Descriptor & TT_ADDRESS_MASK_BLOCK_ENTRY & ~BlockMask) |
      (VirtualAddress & BlockMask);
    return;
  }
}

STATIC
BOOLEAN
ValidatePersistentMap (
  IN UINT64  CurrentEl,
  IN UINT64  SctlrEl2,
  IN UINT64  TcrEl2,
  IN UINT64  Ttbr0El2,
  IN UINT64  MairEl2
  )
{
  MOTO_G20_LOOKUP  Lookup;
  UINT64           Address;
  UINT64           RequiredAttributes;

  if (((CurrentEl & 0xCU) != 0x8U) || ((SctlrEl2 & 1U) == 0U) ||
      (TcrEl2 != MOTO_G20_EXPECTED_TCR_EL2) ||
      (Ttbr0El2 != MOTO_G20_EXPECTED_TTBR0_EL2) ||
      (MairEl2 != MOTO_G20_EXPECTED_MAIR_EL2)) {
    return FALSE;
  }

  RequiredAttributes =
    TT_ATTR_INDX_DEVICE_MEMORY |
    TT_AP_NO_RW |
    TT_SH_NON_SHAREABLE |
    TT_AF |
    TT_XN_MASK;
  for (Address = MOTO_G20_RAMOOPS_BASE; Address < MOTO_G20_RAMOOPS_END; Address += MOTO_G20_PAGE_SIZE) {
    StrictLookup (Address, &Lookup);
    if ((Lookup.State != MotoG20LookupMapped) ||
        (Lookup.Level != 3U) ||
        (Lookup.PhysicalAddress != Address) ||
        ((Lookup.Descriptor & TT_ATTRIBUTES_MASK) != RequiredAttributes)) {
      return FALSE;
    }
  }

  StrictLookup (MOTO_G20_RAMOOPS_BASE - MOTO_G20_PAGE_SIZE, &Lookup);
  if (Lookup.State != MotoG20LookupUnmapped) {
    return FALSE;
  }

  StrictLookup (MOTO_G20_RAMOOPS_END, &Lookup);
  return (BOOLEAN)(Lookup.State == MotoG20LookupUnmapped);
}

STATIC
VOID
Write32 (
  OUT volatile UINT8  *Address,
  IN  UINT32          Value
  )
{
  *(volatile UINT32 *)(VOID *)Address = Value;
}

STATIC
UINT32
Read32 (
  IN volatile CONST UINT8  *Address
  )
{
  return *(volatile CONST UINT32 *)(CONST VOID *)Address;
}

STATIC
VOID
CopyWords (
  OUT volatile UINT8  *Destination,
  IN  CONST UINT8     *Source,
  IN  UINTN           Size
  )
{
  UINT32  Value;
  UINTN   Index;

  for (Index = 0U; Index < Size; Index += 4U) {
    Value = (UINT32)Source[Index] |
            ((UINT32)Source[Index + 1U] << 8) |
            ((UINT32)Source[Index + 2U] << 16) |
            ((UINT32)Source[Index + 3U] << 24);
    Write32 (Destination + Index, Value);
  }
}

STATIC
BOOLEAN
BytesMatch (
  IN volatile CONST UINT8  *Stored,
  IN CONST UINT8           *Expected,
  IN UINTN                 Size
  )
{
  UINTN  Index;
  for (Index = 0U; Index < Size; Index++) {
    if (Stored[Index] != Expected[Index]) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC
UINT32
CommitRecord (
  IN CONST UINT8  Record[MOTO_G20_DXE_FRAME_SIZE],
  IN UINT64       CurrentEl,
  IN UINT64       SctlrEl2,
  IN UINT64       TcrEl2,
  IN UINT64       Ttbr0El2,
  IN UINT64       MairEl2
  )
{
  volatile UINT8  *Zone;

  if (!ValidatePersistentMap (CurrentEl, SctlrEl2, TcrEl2, Ttbr0El2, MairEl2)) {
    return MOTO_G20_PSTORE_COMMIT_MAP_REJECTED;
  }

  Zone = (volatile UINT8 *)(UINTN)MOTO_G20_DXE_ZONE_BASE;
  Write32 (Zone, 0U);
  Barrier ();
  CopyWords (Zone + 12U, mKmsgHeader, sizeof (mKmsgHeader));
  CopyWords (Zone + 20U, Record, MOTO_G20_DXE_FRAME_SIZE);
  Barrier ();
  Write32 (Zone + 4U, MOTO_G20_DXE_PAYLOAD_SIZE);
  Write32 (Zone + 8U, MOTO_G20_DXE_PAYLOAD_SIZE);
  Barrier ();

  if ((Read32 (Zone) != 0U) ||
      (Read32 (Zone + 4U) != MOTO_G20_DXE_PAYLOAD_SIZE) ||
      (Read32 (Zone + 8U) != MOTO_G20_DXE_PAYLOAD_SIZE) ||
      !BytesMatch (Zone + 12U, mKmsgHeader, sizeof (mKmsgHeader)) ||
      !BytesMatch (Zone + 20U, Record, MOTO_G20_DXE_FRAME_SIZE) ||
      !ValidatePersistentMap (CurrentEl, SctlrEl2, TcrEl2, Ttbr0El2, MairEl2)) {
    return MOTO_G20_PSTORE_COMMIT_READBACK_REJECTED;
  }

  Write32 (Zone, MOTO_G20_PSTORE_SIGNATURE);
  Barrier ();
  return MOTO_G20_PSTORE_COMMIT_PASS;
}

STATIC
VOID
Publish (
  IN MOTO_G20_DXE_STAGE            Stage,
  IN EFI_STATUS                    Status,
  IN UINT64                        Detail0,
  IN UINT64                        Detail1,
  IN UINT32                        Flags,
  IN EFI_EXCEPTION_TYPE            ExceptionType,
  IN CONST EFI_SYSTEM_CONTEXT_AARCH64  *Context OPTIONAL,
  IN UINTN                         LineNumber,
  IN UINT64                        FileHash,
  IN UINT64                        DescriptionHash
  )
{
  UINT8   Record[MOTO_G20_DXE_FRAME_SIZE];
  UINT64  CurrentEl;
  UINT64  SctlrEl2;
  UINT64  TcrEl2;
  UINT64  Ttbr0El2;
  UINT64  MairEl2;
  UINT64  VbarEl2;
  UINT64  EsrEl2;
  UINT64  ElrEl2;
  UINT64  FarEl2;
  UINT64  SpsrEl2;
  UINT64  StackPointer;
  UINT64  LinkRegister;
  UINT64  FramePointer;
  UINT32  CommitCode;

  if (mPublishing) {
    return;
  }

  mPublishing = TRUE;
  __asm__ __volatile__ ("isb" ::: "memory");
  CurrentEl    = ReadCurrentEl ();
  SctlrEl2     = ReadSctlrEl2 ();
  TcrEl2       = ReadTcrEl2 ();
  Ttbr0El2     = ReadTtbr0El2 ();
  MairEl2      = ReadMairEl2 ();
  VbarEl2      = ReadVbarEl2 ();
  EsrEl2       = 0U;
  ElrEl2       = 0U;
  FarEl2       = 0U;
  SpsrEl2      = 0U;
  StackPointer = ReadStackPointer ();
  LinkRegister = 0U;
  FramePointer = 0U;
  if (Context != NULL) {
    EsrEl2       = Context->ESR;
    ElrEl2       = Context->ELR;
    FarEl2       = Context->FAR;
    SpsrEl2      = Context->SPSR;
    StackPointer = Context->SP;
    LinkRegister = Context->LR;
    FramePointer = Context->FP;
  }

  if (!mDisplayReleased || ((Flags & MOTO_G20_RECORD_FLAG_FAILURE) != 0U)) {
    if (mDisplayReleased) {
      // A post-console failure owns the screen again, but only after erasing
      // all console pixels so diagnostic and console glyphs cannot overlap.
      mDisplayInitialized = FALSE;
    }

    RenderRecord (
      Stage,
      Status,
      Detail0,
      Detail1,
      CurrentEl,
      SctlrEl2,
      TcrEl2,
      Ttbr0El2,
      MairEl2,
      VbarEl2,
      EsrEl2,
      ElrEl2,
      FarEl2,
      StackPointer,
      LinkRegister,
      (BOOLEAN)((Flags & MOTO_G20_RECORD_FLAG_FAILURE) != 0U)
      );
  }

  ZeroBytes (Record, sizeof (Record));
  {
    UINTN  Index;
    for (Index = 0U; Index < sizeof (mMagic); Index++) {
      Record[M3DXE_OFF_MAGIC + Index] = mMagic[Index];
    }
  }

  Put32 (Record + M3DXE_OFF_VERSION, 2U);
  Put32 (Record + M3DXE_OFF_SIZE, MOTO_G20_DXE_FRAME_SIZE);
  Put64 (Record + M3DXE_OFF_SEQUENCE, ++mSequence);
  Put32 (Record + M3DXE_OFF_STAGE, (UINT32)Stage);
  Put32 (Record + M3DXE_OFF_FLAGS, Flags);
  Put64 (Record + M3DXE_OFF_STATUS, (UINT64)Status);
  Put64 (Record + M3DXE_OFF_DETAIL0, Detail0);
  Put64 (Record + M3DXE_OFF_DETAIL1, Detail1);
  Put64 (Record + M3DXE_OFF_CURRENT_EL, CurrentEl);
  Put64 (Record + M3DXE_OFF_SCTLR_EL2, SctlrEl2);
  Put64 (Record + M3DXE_OFF_TCR_EL2, TcrEl2);
  Put64 (Record + M3DXE_OFF_TTBR0_EL2, Ttbr0El2);
  Put64 (Record + M3DXE_OFF_MAIR_EL2, MairEl2);
  Put64 (Record + M3DXE_OFF_VBAR_EL2, VbarEl2);
  Put64 (Record + M3DXE_OFF_ESR_EL2, EsrEl2);
  Put64 (Record + M3DXE_OFF_ELR_EL2, ElrEl2);
  Put64 (Record + M3DXE_OFF_FAR_EL2, FarEl2);
  Put64 (Record + M3DXE_OFF_SPSR_EL2, SpsrEl2);
  Put64 (Record + M3DXE_OFF_SP, StackPointer);
  Put64 (Record + M3DXE_OFF_LR, LinkRegister);
  Put64 (Record + M3DXE_OFF_FP, FramePointer);
  Put64 (Record + M3DXE_OFF_EXCEPTION_TYPE, (UINT64)ExceptionType);
  Put64 (Record + M3DXE_OFF_LINE, (UINT64)LineNumber);
  Put64 (Record + M3DXE_OFF_FILE_HASH, FileHash);
  Put64 (Record + M3DXE_OFF_DESCRIPTION_HASH, DescriptionHash);
  Put64 (Record + M3DXE_OFF_TICKS, ReadCntpct ());
  Put64 (Record + M3DXE_OFF_ENTRY_IMAGE_BASE, mEntryImageBase);
  Put64 (Record + M3DXE_OFF_ENTRY_IMAGE_SIZE, mEntryImageSize);
  Put64 (Record + M3DXE_OFF_ENTRY_POINT, mEntryPoint);
  Put64 (Record + M3DXE_OFF_ENTRY_CODE_PRE, mEntryCodePre);
  Put64 (Record + M3DXE_OFF_ENTRY_CODE_POST, mEntryCodePost);
  Put32 (Record + M3DXE_OFF_ENTRY_FAULT_CODE, mEntryFaultCode);
  Put32 (Record + M3DXE_OFF_ENTRY_FLAGS, mEntryFlags);
  Put32 (Record + M3DXE_OFF_ENTRY_SEQUENCE, mEntrySequence);
  Put32 (Record + M3DXE_OFF_CRC32, Crc32 (Record, M3DXE_OFF_CRC32));
  CommitCode = CommitRecord (Record, CurrentEl, SctlrEl2, TcrEl2, Ttbr0El2, MairEl2);
  if (!mDisplayReleased || ((Flags & MOTO_G20_RECORD_FLAG_FAILURE) != 0U)) {
    DrawHex64 (
      MOTO_G20_VALUE_X,
      MOTO_G20_ROW_FIRST_Y + (MOTO_G20_PSTORE_ROW_INDEX * MOTO_G20_ROW_PITCH),
      (UINT64)CommitCode,
      (CommitCode == MOTO_G20_PSTORE_COMMIT_PASS) ? MOTO_G20_COLOR_GREEN : MOTO_G20_COLOR_RED
      );
    Barrier ();
  }

  mPublishing = FALSE;
}

STATIC
VOID
WaitAndReset (
  VOID
  )
{
  ARM_SMC_ARGS  Args;
  UINT64        Frequency;
  UINT64        Start;
  UINT64        Duration;

  Frequency = ReadCntfrq ();
  Start     = ReadCntpct ();
  Duration  = Frequency * 20U;
  if ((Frequency != 0U) && (Duration / 20U == Frequency)) {
    while ((ReadCntpct () - Start) < Duration) {
      __asm__ __volatile__ ("yield" ::: "memory");
    }
  }

  ZeroBytes ((UINT8 *)&Args, sizeof (Args));
  Args.Arg0 = ARM_SMC_ID_PSCI_SYSTEM_RESET;
  Barrier ();
  ArmCallSmc (&Args);
  for (;;) {
    __asm__ __volatile__ ("wfe" ::: "memory");
  }
}

VOID
EFIAPI
MotoG20DxeTelemetryCheckpoint (
  IN MOTO_G20_DXE_STAGE  Stage,
  IN EFI_STATUS          Status,
  IN UINT64              Detail0,
  IN UINT64              Detail1
  )
{
  Publish (Stage, Status, Detail0, Detail1, 0U, 0U, NULL, 0U, 0U, 0U);
}

VOID
EFIAPI
MotoG20DxeTelemetryImageEntryPrepare (
  IN CONST VOID   *ImageBase,
  IN UINT64       ImageSize,
  IN CONST VOID   *EntryPoint,
  IN CONST CHAR8  *ImageName OPTIONAL
  )
{
  UINT32  EntryFlags;

  // Publish validity last.  An exception during a guarded capture therefore
  // observes either the previous fully armed entry or an explicitly invalid
  // snapshot, never a partially valid one.
  mEntryFlags = 0U;
  Barrier ();

  mEntryImageBase  = (UINT64)(UINTN)ImageBase;
  mEntryImageSize  = ImageSize;
  mEntryPoint      = (UINT64)(UINTN)EntryPoint;
  mEntryImageId    = HashEntryImageName (ImageName);
  mEntryCodePre    = 0U;
  mEntryCodePost   = 0U;
  mEntryFaultCode  = 0U;
  mEntrySequence++;
  EntryFlags = 0U;

  if ((mEntryPoint & 0x3U) == 0U) {
    EntryFlags |= MOTO_G20_ENTRY_FLAG_ALIGNED;
  }

  if (EntryRangeContains (mEntryPoint, sizeof (mEntryCodePre))) {
    EntryFlags       |= MOTO_G20_ENTRY_FLAG_RANGE_VALID;
    mEntryCodePre     = ReadEntry64 (mEntryPoint);
    EntryFlags       |= MOTO_G20_ENTRY_FLAG_PRE_CAPTURED;
  }

  Barrier ();
  mEntryFlags = EntryFlags;
  Barrier ();
}

VOID
EFIAPI
MotoG20DxeTelemetryImageEntryArmed (
  VOID
  )
{
  UINT32  EntryFlags;

  EntryFlags  = mEntryFlags | MOTO_G20_ENTRY_FLAG_CACHE_SYNCED;
  if ((EntryFlags & MOTO_G20_ENTRY_FLAG_RANGE_VALID) != 0U) {
    mEntryCodePost = ReadEntry64 (mEntryPoint);
    EntryFlags    |= MOTO_G20_ENTRY_FLAG_POST_CAPTURED;
    if (mEntryCodePost == mEntryCodePre) {
      EntryFlags |= MOTO_G20_ENTRY_FLAG_CODE_STABLE;
    }
  }

  Barrier ();
  mEntryFlags = EntryFlags;
  Barrier ();
}

VOID
EFIAPI
MotoG20DxeTelemetryReleaseDisplay (
  VOID
  )
{
  mDisplayReleased = TRUE;
  Barrier ();
}

VOID
EFIAPI
MotoG20DxeTelemetryException (
  IN EFI_EXCEPTION_TYPE  ExceptionType,
  IN EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  CONST EFI_SYSTEM_CONTEXT_AARCH64  *Context;
  UINT32                            EntryFlags;

  Context = SystemContext.SystemContextAArch64;
  EntryFlags = mEntryFlags;
  mEntryFaultCode = (Context == NULL) ? 0U :
                    CaptureFaultInstruction (Context->ELR, &EntryFlags);
  mEntryFlags = EntryFlags;
  Barrier ();
  Publish (
    MotoG20DxeStageException,
    EFI_ABORTED,
    ((UINT64)EntryFlags << 32U) | mEntryFaultCode,
    (Context == NULL) ? 0U : Context->ESR,
    MOTO_G20_RECORD_FLAG_EXCEPTION | MOTO_G20_RECORD_FLAG_FAILURE,
    ExceptionType,
    Context,
    0U,
    mEntryImageId,
    (UINT64)mEntrySequence
    );
  WaitAndReset ();
}

VOID
EFIAPI
MotoG20DxeTelemetryAssert (
  IN CONST CHAR8  *FileName OPTIONAL,
  IN UINTN        LineNumber,
  IN CONST CHAR8  *Description OPTIONAL
  )
{
  UINT64  FileHash;
  UINT64  DescriptionHash;

  FileHash        = HashAscii (FileName);
  DescriptionHash = HashAscii (Description);
  Publish (
    MotoG20DxeStageAssert,
    EFI_ABORTED,
    (UINT64)LineNumber,
    DescriptionHash,
    MOTO_G20_RECORD_FLAG_ASSERT | MOTO_G20_RECORD_FLAG_FAILURE,
    0U,
    NULL,
    LineNumber,
    FileHash,
    DescriptionHash
    );
  WaitAndReset ();
}

VOID
EFIAPI
MotoG20DxeTelemetryFailAndReset (
  IN MOTO_G20_DXE_STAGE  Stage,
  IN EFI_STATUS          Status,
  IN UINT64              Detail0,
  IN UINT64              Detail1
  )
{
  Publish (
    Stage,
    Status,
    Detail0,
    Detail1,
    MOTO_G20_RECORD_FLAG_FAILURE,
    0U,
    NULL,
    0U,
    0U,
    0U
    );
  WaitAndReset ();
}
