#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MotoG20TranslationTableLib.h>

#define MOTO_G20_FB_BASE       0x9E000000ULL
#define MOTO_G20_FB_STRIDE     2880U
#define MOTO_G20_FB_OFFSET     0x17F240ULL
#define MOTO_G20_FB_WIDTH      48U
#define MOTO_G20_FB_HEIGHT     64U
#define MOTO_G20_MARKER_X      656U
#define MOTO_G20_MARKER_Y      544U
#define MOTO_G20_FUTURE_X      656U
#define MOTO_G20_FUTURE_Y      624U
#define MOTO_G20_TABLE_ROOT    0x81981000ULL
#define MOTO_G20_VA_LIMIT      0x10000000000ULL
#define MOTO_G20_STACK_LOW     0x811F0000ULL
#define MOTO_G20_STACK_HIGH    0x81200000ULL
#define MOTO_G20_PE_BASE       0x81000000ULL
#define MOTO_G20_PE_TEXT_END   0x81006000ULL

extern VOID MotoG20Mmu3aCapture (OUT UINT64 *, OUT UINT64 *, OUT UINT64 *);
extern VOID MotoG20Mmu3aPcAnchor (VOID);

STATIC CONST UINT8 mFont[16][7] = {
  {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
  {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
  {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E},
  {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
  {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},{0x0E,0x11,0x11,0x0F,0x01,0x02,0x1C},
  {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
  {0x0F,0x10,0x10,0x10,0x10,0x10,0x0F},{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
  {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}
};

STATIC UINTN FontIndex (IN CHAR8 C) {
  if ((C >= '0') && (C <= '9')) return (UINTN)(C - '0');
  if ((C >= 'A') && (C <= 'Z')) return (UINTN)(C - 'A') + 10U;
  return 0;
}

STATIC VOID FillRectangle (IN UINTN X, IN UINTN Y, IN UINTN W, IN UINTN H, IN UINT32 Color) {
  volatile UINT32 *Fb = (volatile UINT32 *)(UINTN)MOTO_G20_FB_BASE;
  UINTN Row; UINTN Col;
  for (Row = 0; Row < H; Row++) for (Col = 0; Col < W; Col++)
    Fb[((MOTO_G20_MARKER_Y - MOTO_G20_MARKER_Y) + Y + Row) * (MOTO_G20_FB_STRIDE / 4U) + X + Col] = Color;
}

STATIC VOID DrawText (IN UINTN X, IN UINTN Y, IN CONST CHAR8 *Text, IN UINT32 Color) {
  UINTN C, Row, Col; UINT8 Bits;
  for (C = 0; Text[C] != '\0'; C++) for (Row = 0; Row < 5; Row++) {
    Bits = mFont[FontIndex(Text[C])][Row];
    for (Col = 0; Col < 5; Col++) if ((Bits & (1U << (4U - Col))) != 0)
      FillRectangle(X + C * 12U + Col * 2U, Y + Row * 2U, 2, 2, Color);
  }
}

STATIC VOID DrawHex64 (IN UINTN X, IN UINTN Y, IN UINT64 Value, IN UINT32 Color) {
  STATIC CONST CHAR8 Hex[] = "0123456789ABCDEF"; CHAR8 Text[17]; UINTN I;
  for (I = 0; I < 16; I++) Text[I] = Hex[(Value >> ((15U - I) * 4U)) & 0xFULL];
  Text[16] = '\0';
  DrawText(X, Y, Text, Color);
}

STATIC UINT64 ReadSctlr (VOID) { UINT64 V; __asm__ volatile("mrs %0, SCTLR_EL2" : "=r"(V)); return V; }
STATIC UINT64 ReadTcr (VOID) { UINT64 V; __asm__ volatile("mrs %0, TCR_EL2" : "=r"(V)); return V; }
STATIC UINT64 ReadTtbr0 (VOID) { UINT64 V; __asm__ volatile("mrs %0, TTBR0_EL2" : "=r"(V)); return V; }
STATIC UINT64 ReadMair (VOID) { UINT64 V; __asm__ volatile("mrs %0, MAIR_EL2" : "=r"(V)); return V; }
STATIC UINT64 ReadVbar (VOID) { UINT64 V; __asm__ volatile("mrs %0, VBAR_EL2" : "=r"(V)); return V; }
STATIC UINT64 ReadCurrentEl (VOID) { UINT64 V; __asm__ volatile("mrs %0, CurrentEL" : "=r"(V)); return V; }
STATIC UINT64 ReadHcr (VOID) { UINT64 V; __asm__ volatile("mrs %0, HCR_EL2" : "=r"(V)); return V; }

STATIC BOOLEAN WalkExecutableWb (IN UINT64 Va, OUT MOTO_G20_TRANSLATION_LOOKUP *Out) {
  UINT64 Attr;
  if (EFI_ERROR(MotoG20LookupTranslation((VOID *)(UINTN)MOTO_G20_TABLE_ROOT, Va, Out)) || !Out->Mapped || (Out->PhysicalAddress != Va)) return FALSE;
  Attr = Out->Descriptor;
  return (BOOLEAN)((Out->Level == 2) && (((Attr >> 2) & 7U) == 3U) && (((Attr >> 8) & 3U) == 3U) && ((Attr & (1ULL << 10)) != 0) && ((Attr & (1ULL << 53)) == 0) && ((Attr & (1ULL << 54)) == 0));
}

STATIC BOOLEAN WalkStack (IN UINT64 Sp) {
  MOTO_G20_TRANSLATION_LOOKUP A, B;
  if ((Sp < MOTO_G20_STACK_LOW) || (Sp > MOTO_G20_STACK_HIGH) || (Sp < 16)) return FALSE;
  return WalkExecutableWb(Sp - 16, &A) && WalkExecutableWb(Sp - 1, &B);
}

STATIC VOID Halt (VOID) { for (;;) __asm__ volatile("wfe"); }

VOID EFIAPI MotoG20Mmu3aVbarSpProbe (VOID) {
  UINT64 Vbar1, Vbar2, Sctlr, Tcr, Ttbr0, Mair, Sp, Spsel, Pc, CurrentEl, Hcr;
  MOTO_G20_TRANSLATION_LOOKUP V0, V1, P0, P1;
  BOOLEAN Stable, Align, Range, VaRange, VbarMap, VbarExec, SpMap, PcMap, FutureMap, Green;
  UINT64 End;

  Vbar1 = ReadVbar();
  Sctlr = ReadSctlr(); Tcr = ReadTcr(); Ttbr0 = ReadTtbr0(); Mair = ReadMair();
  CurrentEl = ReadCurrentEl(); Hcr = ReadHcr();
  MotoG20Mmu3aCapture(&Sp, &Spsel, &Pc);
  Vbar2 = ReadVbar();

  Stable = (BOOLEAN)(Vbar1 == Vbar2);
  Align = (BOOLEAN)((Vbar2 & 0x7FFULL) == 0);
  Range = (BOOLEAN)(Vbar2 <= (MAX_UINT64 - 0x7FFULL));
  End = Range ? Vbar2 + 0x7FFULL : 0;
  VaRange = (BOOLEAN)(Range && (Vbar2 < MOTO_G20_VA_LIMIT) && (End < MOTO_G20_VA_LIMIT));
  VbarMap = (BOOLEAN)(VaRange && !EFI_ERROR(MotoG20LookupTranslation((VOID *)(UINTN)MOTO_G20_TABLE_ROOT, Vbar2, &V0)) && !EFI_ERROR(MotoG20LookupTranslation((VOID *)(UINTN)MOTO_G20_TABLE_ROOT, End, &V1)) && V0.Mapped && V1.Mapped && (V0.PhysicalAddress == Vbar2) && (V1.PhysicalAddress == End));
  VbarExec = (BOOLEAN)(VbarMap && WalkExecutableWb(Vbar2, &V0) && WalkExecutableWb(End, &V1));
  SpMap = WalkStack(Sp);
  PcMap = (BOOLEAN)((Pc == (UINT64)(UINTN)&MotoG20Mmu3aPcAnchor) && (Pc >= MOTO_G20_PE_BASE) && (Pc < MOTO_G20_PE_TEXT_END) && WalkExecutableWb(Pc, &P0));
  FutureMap = WalkExecutableWb(0x81001044ULL, &P0) && WalkExecutableWb(0x81001070ULL, &P1);
  Green = (BOOLEAN)(Stable && Align && VbarExec && VaRange && SpMap && PcMap && FutureMap && ((Sctlr & (BIT0|BIT2|BIT12)) == 0) && (Spsel <= 1) && (CurrentEl == 8));

  FillRectangle(MOTO_G20_MARKER_X, MOTO_G20_MARKER_Y, MOTO_G20_FB_WIDTH, MOTO_G20_FB_HEIGHT, Green ? 0x0000FF00 : 0x00FF0000);
  DrawText(16, 1560, "PREENABLE", 0x00FFFFFF); DrawText(16, 1590, "VBAR", 0x00FFFFFF); DrawHex64(160, 1590, Vbar2, 0x00FFFFFF);
  DrawText(16, 1620, "SP", 0x00FFFFFF); DrawHex64(160, 1620, Sp, 0x00FFFFFF);
  DrawText(16, 1650, "SPSEL", 0x00FFFFFF); DrawHex64(160, 1650, Spsel, 0x00FFFFFF);
  DrawText(16, 1680, "PC", 0x00FFFFFF); DrawHex64(160, 1680, Pc, 0x00FFFFFF);
  DrawText(16, 1710, "SCTLR", 0x00FFFFFF); DrawHex64(160, 1710, Sctlr, 0x00FFFFFF);
  DrawText(16, 1740, "TCR", 0x00FFFFFF); DrawHex64(160, 1740, Tcr, 0x00FFFFFF);
  DrawText(16, 1770, "TTBR0", 0x00FFFFFF); DrawHex64(160, 1770, Ttbr0, 0x00FFFFFF);
  DrawText(16, 1800, "MAIR", 0x00FFFFFF); DrawHex64(160, 1800, Mair, 0x00FFFFFF);
  DrawText(16, 1830, "CURRENTEL", 0x00FFFFFF); DrawHex64(160, 1830, CurrentEl, 0x00FFFFFF);
  DrawText(16, 1860, "HCR", 0x00FFFFFF); DrawHex64(160, 1860, Hcr, 0x00FFFFFF);
  DrawText(16, 1890, "HEXTEST", 0x0000FFFF); DrawText(16, 1920, "0123456789ABCDEF", 0x0000FFFF); DrawText(16, 1950, "FEDCBA9876543210", 0x0000FFFF);
  Halt();
}
