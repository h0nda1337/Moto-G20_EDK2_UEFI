/** @file
  Bounded post-MMU Foundation status renderer for the Moto G20.

  This renderer intentionally does not reuse the physically frozen diagnostic
  font.  Every glyph lookup is bounded, every draw has a fixed cell, and the
  label/value columns end before the independent marker column begins.
**/

#include <Base.h>
#include <Library/ArmLib.h>

#include "MotoG20FoundationDisplay.h"

#define FOUNDATION_TEXT    __attribute__((section (".foundation.text")))
#define FOUNDATION_DATA    __attribute__((section (".foundation.data")))
#define FOUNDATION_RODATA  __attribute__((section (".foundation.rodata")))

#define MOTO_G20_FB_BASE            0x9E000000ULL
#define MOTO_G20_FB_WIDTH           720U
#define MOTO_G20_FB_HEIGHT          1600U
#define MOTO_G20_FB_STRIDE_PIXELS   720U

#define MOTO_G20_FONT_WIDTH         5U
#define MOTO_G20_FONT_HEIGHT        7U
#define MOTO_G20_FONT_SCALE         2U
#define MOTO_G20_GLYPH_WIDTH        (MOTO_G20_FONT_WIDTH * MOTO_G20_FONT_SCALE)
#define MOTO_G20_GLYPH_HEIGHT       (MOTO_G20_FONT_HEIGHT * MOTO_G20_FONT_SCALE)
#define MOTO_G20_GLYPH_ADVANCE      12U
#define MOTO_G20_ROW_PITCH          48U

#define MOTO_G20_LABEL_X            32U
#define MOTO_G20_LABEL_CELLS        20U
#define MOTO_G20_VALUE_X            300U
#define MOTO_G20_VALUE_CELLS        22U
// The frozen unexpected-exception stripe can touch x=620..671.  Keep the
// Foundation marker column strictly to its right so even a fail-stop cannot
// paint over a Foundation cell or marker.
#define MOTO_G20_MARKER_X           672U
#define MOTO_G20_MARKER_WIDTH       40U
#define MOTO_G20_MARKER_HEIGHT      28U
#define MOTO_G20_MARKER_FIRST_Y     144U
#define MOTO_G20_MARKER_PITCH       64U
#define MOTO_G20_MAX_STAGE          MotoG20FoundationStageDxeEntryDispatch

#define MOTO_G20_COLOR_BLACK        0x00000000U
#define MOTO_G20_COLOR_WHITE        0x00FFFFFFU
#define MOTO_G20_COLOR_CYAN         0x0000FFFFU
#define MOTO_G20_COLOR_GREEN        0x0000FF00U
#define MOTO_G20_COLOR_YELLOW       0x0000FFFFU
#define MOTO_G20_COLOR_ORANGE       0x000080FFU
#define MOTO_G20_COLOR_RED          0x00FF0000U
#define MOTO_G20_COLOR_DARK_BLUE    0x00201008U

typedef struct {
  CHAR8  Character;
  UINT8  Rows[MOTO_G20_FONT_HEIGHT];
} MOTO_G20_FONT_GLYPH;

STATIC CONST MOTO_G20_FONT_GLYPH mMotoG20FoundationFont[] FOUNDATION_RODATA = {
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

STATIC BOOLEAN mMotoG20FoundationDisplayInitialized FOUNDATION_DATA;
STATIC UINT64 mMotoG20FoundationLastStage FOUNDATION_DATA;
STATIC UINT64 mMotoG20FoundationLastDetail FOUNDATION_DATA;

STATIC
VOID
FOUNDATION_TEXT
MotoG20FoundationFillRectangle (
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

  if ((Width == 0) || (Height == 0) ||
      (X >= MOTO_G20_FB_WIDTH) || (Y >= MOTO_G20_FB_HEIGHT) ||
      (Width > (MOTO_G20_FB_WIDTH - X)) ||
      (Height > (MOTO_G20_FB_HEIGHT - Y))) {
    return;
  }

  Framebuffer = (volatile UINT32 *)(UINTN)MOTO_G20_FB_BASE;
  for (Row = 0; Row < Height; Row++) {
    for (Column = 0; Column < Width; Column++) {
      Framebuffer[((Y + Row) * MOTO_G20_FB_STRIDE_PIXELS) + X + Column] = Color;
    }
  }
}

STATIC
CONST UINT8 *
FOUNDATION_TEXT
MotoG20FoundationFindGlyph (
  IN CHAR8  Character
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mMotoG20FoundationFont); Index++) {
    if (mMotoG20FoundationFont[Index].Character == Character) {
      return mMotoG20FoundationFont[Index].Rows;
    }
  }

  for (Index = 0; Index < ARRAY_SIZE (mMotoG20FoundationFont); Index++) {
    if (mMotoG20FoundationFont[Index].Character == '?') {
      return mMotoG20FoundationFont[Index].Rows;
    }
  }

  return mMotoG20FoundationFont[0].Rows;
}

STATIC
VOID
FOUNDATION_TEXT
MotoG20FoundationDrawGlyph (
  IN UINTN   X,
  IN UINTN   Y,
  IN CHAR8   Character,
  IN UINT32  Color
  )
{
  CONST UINT8  *Rows;
  UINTN         Row;
  UINTN         Column;

  if ((X > (MOTO_G20_FB_WIDTH - MOTO_G20_GLYPH_WIDTH)) ||
      (Y > (MOTO_G20_FB_HEIGHT - MOTO_G20_GLYPH_HEIGHT))) {
    return;
  }

  Rows = MotoG20FoundationFindGlyph (Character);
  for (Row = 0; Row < MOTO_G20_FONT_HEIGHT; Row++) {
    for (Column = 0; Column < MOTO_G20_FONT_WIDTH; Column++) {
      if ((Rows[Row] & (1U << (MOTO_G20_FONT_WIDTH - 1U - Column))) != 0) {
        MotoG20FoundationFillRectangle (
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
FOUNDATION_TEXT
MotoG20FoundationDrawString (
  IN UINTN        X,
  IN UINTN        Y,
  IN UINTN        MaximumCharacters,
  IN CONST CHAR8  *Text,
  IN UINT32       Color
  )
{
  UINTN  Index;

  if ((Text == NULL) || (MaximumCharacters == 0) ||
      (MaximumCharacters > (MOTO_G20_FB_WIDTH / MOTO_G20_GLYPH_ADVANCE)) ||
      (X > (MOTO_G20_FB_WIDTH - (MaximumCharacters * MOTO_G20_GLYPH_ADVANCE))) ||
      (Y > (MOTO_G20_FB_HEIGHT - MOTO_G20_GLYPH_HEIGHT))) {
    return;
  }

  MotoG20FoundationFillRectangle (
    X,
    Y,
    MaximumCharacters * MOTO_G20_GLYPH_ADVANCE,
    MOTO_G20_GLYPH_HEIGHT,
    MOTO_G20_COLOR_BLACK
    );
  for (Index = 0; (Index < MaximumCharacters) && (Text[Index] != '\0'); Index++) {
    MotoG20FoundationDrawGlyph (
      X + (Index * MOTO_G20_GLYPH_ADVANCE),
      Y,
      Text[Index],
      Color
      );
  }
}

STATIC
VOID
FOUNDATION_TEXT
MotoG20FoundationDrawHex64 (
  IN UINTN   X,
  IN UINTN   Y,
  IN UINT64  Value,
  IN UINT32  Color
  )
{
  STATIC CONST CHAR8 HexDigits[] FOUNDATION_RODATA = "0123456789ABCDEF";
  CHAR8              Buffer[17];
  UINTN              Index;

  for (Index = 0; Index < 16; Index++) {
    Buffer[Index] = HexDigits[(Value >> ((15U - Index) * 4U)) & 0xFULL];
  }

  Buffer[16] = '\0';
  MotoG20FoundationDrawString (X, Y, MOTO_G20_VALUE_CELLS, Buffer, Color);
}

STATIC
CONST CHAR8 *
FOUNDATION_TEXT
MotoG20FoundationStageName (
  IN MOTO_G20_FOUNDATION_STAGE  Stage
  )
{
  switch (Stage) {
    case MotoG20FoundationStageNone:             return "NONE";
    case MotoG20FoundationStageIdentity:         return "IDENTITY";
    case MotoG20FoundationStageFdt:              return "FDT";
    case MotoG20FoundationStageFvSource:         return "FV SOURCE";
    case MotoG20FoundationStageFvHash:           return "FV HASH";
    case MotoG20FoundationStageFvCopy:           return "FV COPY";
    case MotoG20FoundationStageFramebufferMap:   return "FB MAP";
    case MotoG20FoundationStageMmuProof:         return "MMU PROOF";
    case MotoG20FoundationStageHighMap:          return "HIGH MAP";
    case MotoG20FoundationStageDxeMemoryClear:   return "DXE MEMORY CLEAR";
    case MotoG20FoundationStageHobConstruct:     return "HOB CONSTRUCT";
    case MotoG20FoundationStageHobPublish:       return "HOB PUBLISH";
    case MotoG20FoundationStageFvHob:            return "FV HOB";
    case MotoG20FoundationStageDxeCoreLocate:    return "DXE CORE LOCATE";
    case MotoG20FoundationStageDxeCoreLoad:      return "DXE CORE LOAD";
    case MotoG20FoundationStageDxeStack:         return "DXE STACK";
    case MotoG20FoundationStageDxeCoreTransfer:  return "DXE CORE TRANSFER";
    case MotoG20FoundationStageDxeCoreReturned:  return "DXE CORE RETURNED";
    case MotoG20FoundationStageDxeStackBridge:   return "DXE STACK BRIDGE";
    case MotoG20FoundationStageDxeEntryDispatch: return "DXE ENTRY DISPATCH";
    default:                                     return "UNKNOWN";
  }
}

STATIC
VOID
FOUNDATION_TEXT
MotoG20FoundationDrawMarker (
  IN MOTO_G20_FOUNDATION_STAGE  Stage,
  IN EFI_STATUS                 Status
  )
{
  UINTN   Y;
  UINT32  Color;

  if ((UINTN)Stage > (UINTN)MOTO_G20_MAX_STAGE) {
    return;
  }

  Y = MOTO_G20_MARKER_FIRST_Y + ((UINTN)Stage * MOTO_G20_MARKER_PITCH);
  if (Y > (MOTO_G20_FB_HEIGHT - MOTO_G20_MARKER_HEIGHT)) {
    return;
  }

  if (Status == EFI_NOT_READY) {
    Color = MOTO_G20_COLOR_ORANGE;
  } else if (EFI_ERROR (Status)) {
    Color = MOTO_G20_COLOR_RED;
  } else {
    Color = MOTO_G20_COLOR_GREEN;
  }

  MotoG20FoundationFillRectangle (
    MOTO_G20_MARKER_X,
    Y,
    MOTO_G20_MARKER_WIDTH,
    MOTO_G20_MARKER_HEIGHT,
    Color
    );
}

VOID
EFIAPI
FOUNDATION_TEXT
MotoG20FoundationDisplayBegin (
  IN UINT64  DtbAddress,
  IN UINT64  SctlrEl2
  )
{
  MotoG20FoundationFillRectangle (
    0,
    0,
    MOTO_G20_FB_WIDTH,
    MOTO_G20_FB_HEIGHT,
    MOTO_G20_COLOR_BLACK
    );
  MotoG20FoundationFillRectangle (0, 0, MOTO_G20_FB_WIDTH, 112U, MOTO_G20_COLOR_DARK_BLUE);
  MotoG20FoundationDrawString (32, 24, 36, "MOTO G20 UEFI FOUNDATION", MOTO_G20_COLOR_WHITE);
  MotoG20FoundationDrawString (32, 64, 36, "BOUNDED NO OVERLAP PANEL", MOTO_G20_COLOR_CYAN);

  MotoG20FoundationDrawString (MOTO_G20_LABEL_X, 144, MOTO_G20_LABEL_CELLS, "STAGE", MOTO_G20_COLOR_WHITE);
  MotoG20FoundationDrawString (MOTO_G20_LABEL_X, 144 + MOTO_G20_ROW_PITCH, MOTO_G20_LABEL_CELLS, "STATUS", MOTO_G20_COLOR_WHITE);
  MotoG20FoundationDrawString (MOTO_G20_LABEL_X, 144 + (2U * MOTO_G20_ROW_PITCH), MOTO_G20_LABEL_CELLS, "DETAIL", MOTO_G20_COLOR_WHITE);
  MotoG20FoundationDrawString (MOTO_G20_LABEL_X, 144 + (3U * MOTO_G20_ROW_PITCH), MOTO_G20_LABEL_CELLS, "SCTLR EL2", MOTO_G20_COLOR_WHITE);
  MotoG20FoundationDrawString (MOTO_G20_LABEL_X, 144 + (4U * MOTO_G20_ROW_PITCH), MOTO_G20_LABEL_CELLS, "DTB", MOTO_G20_COLOR_WHITE);
  MotoG20FoundationDrawString (MOTO_G20_LABEL_X, 144 + (5U * MOTO_G20_ROW_PITCH), MOTO_G20_LABEL_CELLS, "FV BASE", MOTO_G20_COLOR_WHITE);
  MotoG20FoundationDrawString (MOTO_G20_LABEL_X, 144 + (6U * MOTO_G20_ROW_PITCH), MOTO_G20_LABEL_CELLS, "FV SIZE", MOTO_G20_COLOR_WHITE);
  MotoG20FoundationDrawString (MOTO_G20_LABEL_X, 144 + (7U * MOTO_G20_ROW_PITCH), MOTO_G20_LABEL_CELLS, "HOB BASE", MOTO_G20_COLOR_WHITE);
  MotoG20FoundationDrawString (MOTO_G20_LABEL_X, 144 + (8U * MOTO_G20_ROW_PITCH), MOTO_G20_LABEL_CELLS, "DXE MEMORY END", MOTO_G20_COLOR_WHITE);

  MotoG20FoundationDrawHex64 (MOTO_G20_VALUE_X, 144 + (3U * MOTO_G20_ROW_PITCH), SctlrEl2, MOTO_G20_COLOR_CYAN);
  MotoG20FoundationDrawHex64 (MOTO_G20_VALUE_X, 144 + (4U * MOTO_G20_ROW_PITCH), DtbAddress, MOTO_G20_COLOR_CYAN);
  MotoG20FoundationDrawHex64 (MOTO_G20_VALUE_X, 144 + (5U * MOTO_G20_ROW_PITCH), MOTO_G20_FOUNDATION_FV_STAGING_BASE, MOTO_G20_COLOR_CYAN);
  MotoG20FoundationDrawHex64 (MOTO_G20_VALUE_X, 144 + (6U * MOTO_G20_ROW_PITCH), MOTO_G20_FOUNDATION_FV_TRANSPORT_MAX, MOTO_G20_COLOR_CYAN);
  MotoG20FoundationDrawHex64 (MOTO_G20_VALUE_X, 144 + (7U * MOTO_G20_ROW_PITCH), MOTO_G20_FOUNDATION_DXE_MEMORY_BASE, MOTO_G20_COLOR_CYAN);
  MotoG20FoundationDrawHex64 (MOTO_G20_VALUE_X, 144 + (8U * MOTO_G20_ROW_PITCH), MOTO_G20_FOUNDATION_DXE_MEMORY_END, MOTO_G20_COLOR_CYAN);
  MotoG20FoundationDrawString (32, 1472, 44, "FROZEN MMU PATH PRESERVED - NO RETRY", MOTO_G20_COLOR_CYAN);
  mMotoG20FoundationLastStage  = MotoG20FoundationStageNone;
  mMotoG20FoundationLastDetail = 0;
  mMotoG20FoundationDisplayInitialized = TRUE;
  ArmDataSynchronizationBarrier ();
}

VOID
EFIAPI
FOUNDATION_TEXT
MotoG20FoundationDisplayProgress (
  IN MOTO_G20_FOUNDATION_STAGE  Stage,
  IN EFI_STATUS                 Status,
  IN UINT64                     Detail
  )
{
  UINT32  TextColor;

  if (!mMotoG20FoundationDisplayInitialized) {
    return;
  }

  if (Status == EFI_NOT_READY) {
    TextColor = MOTO_G20_COLOR_ORANGE;
  } else if (EFI_ERROR (Status)) {
    TextColor = MOTO_G20_COLOR_RED;
  } else {
    TextColor = MOTO_G20_COLOR_CYAN;
  }

  MotoG20FoundationDrawString (
    MOTO_G20_VALUE_X,
    144,
    MOTO_G20_VALUE_CELLS,
    MotoG20FoundationStageName (Stage),
    TextColor
    );
  MotoG20FoundationDrawHex64 (
    MOTO_G20_VALUE_X,
    144 + MOTO_G20_ROW_PITCH,
    (UINT64)Status,
    TextColor
    );
  MotoG20FoundationDrawHex64 (
    MOTO_G20_VALUE_X,
    144 + (2U * MOTO_G20_ROW_PITCH),
    Detail,
    MOTO_G20_COLOR_CYAN
    );
  if ((Stage == MotoG20FoundationStageMmuProof) &&
      !EFI_ERROR (Status) &&
      ((Detail & (BIT0 | BIT2)) == (BIT0 | BIT2))) {
    MotoG20FoundationDrawHex64 (
      MOTO_G20_VALUE_X,
      144 + (3U * MOTO_G20_ROW_PITCH),
      Detail,
      MOTO_G20_COLOR_CYAN
      );
  }
  MotoG20FoundationDrawMarker (Stage, Status);
  mMotoG20FoundationLastStage  = Stage;
  mMotoG20FoundationLastDetail = Detail;
  ArmDataSynchronizationBarrier ();
}

VOID
EFIAPI
FOUNDATION_TEXT
MotoG20FoundationDisplayFailure (
  IN CONST MOTO_G20_FOUNDATION_FAILURE_RECORD  *Record
  )
{
  UINT64  Detail;

  if (Record == NULL) {
    return;
  }

  Detail = Record->DtbAddress;
  if (mMotoG20FoundationDisplayInitialized &&
      (mMotoG20FoundationLastStage ==
       Record->Stage)) {
    Detail = mMotoG20FoundationLastDetail;
  }

  if (!mMotoG20FoundationDisplayInitialized) {
    MotoG20FoundationDisplayBegin (Record->DtbAddress, Record->SctlrEl2);
  }

  MotoG20FoundationDrawString (32, 640, 36, "FOUNDATION FAIL CLOSED", MOTO_G20_COLOR_RED);
  MotoG20FoundationDisplayProgress (
    (MOTO_G20_FOUNDATION_STAGE)Record->Stage,
    Record->Status,
    Detail
    );
}
