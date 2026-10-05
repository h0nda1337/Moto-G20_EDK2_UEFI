#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MotoG20TranslationTableLib.h>

#define MOTO_G20_TABLE_ROOT 0x81981000ULL
#define MOTO_G20_PE_BASE 0x81000000ULL
#define MOTO_G20_FB_BASE 0x9E000000ULL
#define MOTO_G20_FB_STRIDE 2880U
#define MOTO_G20_MARKER_X 656U
#define MOTO_G20_MARKER_W 48U
#define MOTO_G20_MARKER_H 64U
#define MOTO_G20_MMU3B1_MARKER_Y 624U
#define MOTO_G20_MMU3B2_MARKER_Y 704U
#define MOTO_G20_TRUE_MMU3_MARKER_Y 784U
#define MOTO_G20_MMU_BITS (BIT0 | BIT2 | BIT12)
#define MOTO_G20_MMU3B2_SIGNATURE 0x4D4D553342434150ULL
#define MOTO_G20_MMU3B2_EXPECTED_ESR 0x00000000F20003B2ULL
#define MOTO_G20_MMU3B2_ENTERED_COOKIE 0x00000000B2E1B2E1ULL
#define MOTO_G20_MMU3B2_VALIDATED_COOKIE 0x00000000B2E3B2E3ULL
#define MOTO_G20_MMU3B2_POST_ERET_COOKIE 0x00000000B2E4B2E4ULL
#define MOTO_G20_TRUE_MMU3_SIGNATURE 0x54525545334D4F4EULL
#define MOTO_G20_TRUE_PRE_COOKIE 0x54525545334D5052ULL
#define MOTO_G20_TRUE_FIRST_COOKIE 0x54525545334D5054ULL
#define MOTO_G20_TRUE_DATA_COOKIE 0x5452554533444154ULL
#define MOTO_G20_TRUE_STACK_COOKIE 0x545255453353544BULL
#define MOTO_G20_TRUE_RENDER_COOKIE 0x545255453352454EULL
#define MOTO_G20_TRUE_FINAL_COOKIE 0x545255453346494EULL
#define MOTO_G20_PE_IMAGE_SIZE 0xE000ULL
#if defined(__clang__)
#define MMU3_TRUE_TEXT __attribute__((section(".mmu3true.text")))
#define MMU3C1_CHECKPOINT_TEXT __attribute__((section(".mmu3c1.checkpoint.text"), noinline))
#else
#define MMU3_TRUE_TEXT __attribute__((section(".mmu3true.text"), optimize("Os")))
#define MMU3C1_CHECKPOINT_TEXT __attribute__((section(".mmu3c1.checkpoint.text"), optimize("Os"), noinline))
#endif
#define MOTO_G20_TRUE_ENTRY_COLOR 0x00FF0000U
#define MOTO_G20_TRUE_CAPTURE_COLOR 0x00FF8000U
#define MOTO_G20_TRUE_WALK_COLOR 0x00FFFF00U
#define MOTO_G20_TRUE_PRE_MSR_COLOR 0x00FF00FFU
#define MOTO_G20_TRUE_POST_ISB_COLOR 0x0000FFFFU
#define MOTO_G20_TRUE_M1_STORE_COLOR 0x000080FFU
#define MOTO_G20_TRUE_RETURN_COLOR 0x008000FFU
#define MOTO_G20_TRUE_SUCCESS_COLOR 0x0000FF00U

typedef struct {
  UINT64 Signature;
  UINT64 Status;
  UINT64 VectorId;
  UINT64 PreBrkSpsel;
  UINT64 CurrentEl;
  UINT64 Esr;
  UINT64 ElrEntry;
  UINT64 ElrResume;
  UINT64 Spsr;
  UINT64 Far;
  UINT64 Vbar;
  UINT64 Sctlr;
  UINT64 EntrySp;
  UINT64 BrkRuntimeExpected;
  UINT64 PostBrkRuntimeExpected;
  UINT64 ExpectedEsr;
  UINT64 HandlerEnteredCookie;
  UINT64 HandlerValidatedCookie;
  UINT64 PostEretCookie;
} MMU3B1_CAPTURE;

typedef struct {
  UINT64 Signature;
  UINT64 Phase;
  UINT64 CurrentElBefore;
  UINT64 HcrEl2Before;
  UINT64 Mmfr0Before;
  UINT64 SctlrBefore;
  UINT64 TcrBefore;
  UINT64 Ttbr0Before;
  UINT64 MairBefore;
  UINT64 VbarBefore;
  UINT64 PreEnableCookie;
  UINT64 FirstPostEnableCookie;
  UINT64 SctlrAfter;
  UINT64 TcrAfter;
  UINT64 Ttbr0After;
  UINT64 MairAfter;
  UINT64 VbarAfter;
  UINT64 PostEnableDataCookie;
  UINT64 PostEnableStackCookie;
  UINT64 PostEnableRenderCookie;
  UINT64 FinalSuccessCookie;
  UINT64 FailureCode;
} TRUE_MMU3_CAPTURE;

extern UINT8 MotoG20Mmu3b1VectorTableStart[];
extern UINT8 MotoG20Mmu3b1VectorTableEnd[];
extern UINT8 MotoG20Mmu3b1ExceptionStackBase[];
extern UINT8 MotoG20Mmu3b1ExceptionStackTop[];
extern UINT8 MotoG20Mmu3b2ControlledException[];
extern UINT8 MotoG20Mmu3b2BrkInstruction[];
extern UINT8 MotoG20Mmu3b2PostBrk[];
extern volatile MMU3B1_CAPTURE MotoG20Mmu3b1CaptureBlock;
extern volatile TRUE_MMU3_CAPTURE MotoG20Mmu3TrueCaptureBlock;
extern VOID MotoG20Mmu3b1UnexpectedException (VOID);
extern VOID EFIAPI MotoG20Mmu3b2Trigger (VOID);
extern BOOLEAN MotoG20Mmu3b1WriteVbarReadback (IN UINT64 Address, OUT UINT64 *Readback);
extern VOID EFIAPI MotoG20DrawMmu3b1Diagnostics (IN UINT64 VbarBefore, IN UINT64 VbarAfter, IN UINT64 SctlrBefore, IN UINT64 SctlrAfter, IN UINT64 TcrDisplay, IN UINT64 Ttbr0Display, IN UINT64 MairDisplay);
extern VOID EFIAPI MotoG20DrawMmu3b2Diagnostics (IN UINT64 BrkImm, IN UINT64 PreSpsel, IN UINT64 VectorId, IN UINT64 Esr, IN UINT64 ExpectedEsr, IN UINT64 ElrEntry, IN UINT64 ElrResume, IN UINT64 Spsr, IN UINT64 Far, IN UINT64 EntrySp, IN UINT64 Vbar, IN UINT64 Sctlr, IN UINT64 PostEret);
extern VOID EFIAPI MotoG20DrawTrueMmu3MOnlyDiagnostics (IN UINT64 SctlrBefore, IN UINT64 SctlrAfter, IN UINT64 Tcr, IN UINT64 Ttbr0, IN UINT64 Mair, IN UINT64 Vbar, IN UINT64 CurrentEl, IN UINT64 Hcr, IN UINT64 CodeCookie, IN UINT64 DataCookie, IN UINT64 StackCookie, IN UINT64 RenderCookie, IN UINT64 FinalCookie, IN UINT64 Phase);
extern UINT64 EFIAPI MotoG20EnableMmuEl2MOnly (VOID);
extern UINT8 MotoG20Mmu3FirstPostEnableInstruction[];
extern volatile BOOLEAN gMotoG20Mmu3PreEnableOk;
extern volatile UINT64 gMotoG20Mmu3DataProbe;

STATIC UINT64 ReadSctlr (VOID) { UINT64 V; __asm__ volatile ("mrs %0, SCTLR_EL2" : "=r" (V)); return V; }
STATIC UINT64 ReadSctlrAfter (VOID) { UINT64 V; __asm__ volatile ("mrs %0, SCTLR_EL2" : "=r" (V)); return V; }
STATIC UINT64 ReadVbarBefore (VOID) { UINT64 V; __asm__ volatile ("mrs %0, VBAR_EL2" : "=r" (V)); return V; }
STATIC UINT64 ReadTcrDisplay (VOID) { UINT64 V; __asm__ volatile ("mrs %0, TCR_EL2" : "=r" (V)); return V; }
STATIC UINT64 ReadTtbr0Display (VOID) { UINT64 V; __asm__ volatile ("mrs %0, TTBR0_EL2" : "=r" (V)); return V; }
STATIC UINT64 ReadMairDisplay (VOID) { UINT64 V; __asm__ volatile ("mrs %0, MAIR_EL2" : "=r" (V)); return V; }
STATIC UINT64 ReadSpsel (VOID) { UINT64 V; __asm__ volatile ("mrs %0, SPSel" : "=r" (V)); return V & 1ULL; }
STATIC UINT64 ReadCurrentEl (VOID) { UINT64 V; __asm__ volatile ("mrs %0, CurrentEL" : "=r" (V)); return V; }

STATIC BOOLEAN WalkExecutableWb (IN UINT64 Va) {
  MOTO_G20_TRANSLATION_LOOKUP R;
  UINT64 A;
  if (EFI_ERROR (MotoG20LookupTranslation ((VOID *)(UINTN)MOTO_G20_TABLE_ROOT, Va, &R)) || !R.Mapped || R.PhysicalAddress != Va) return FALSE;
  A = R.Descriptor;
  return (BOOLEAN)(R.Level == 2 && (((A >> 2) & 7U) == 3U) && (((A >> 8) & 3U) == 3U) && (A & BIT10) != 0 && (A & BIT53) == 0 && (A & BIT54) == 0);
}

STATIC BOOLEAN WalkNormalWb (IN UINT64 Va) {
  MOTO_G20_TRANSLATION_LOOKUP R;
  UINT64 A;
  if (EFI_ERROR (MotoG20LookupTranslation ((VOID *)(UINTN)MOTO_G20_TABLE_ROOT, Va, &R)) || !R.Mapped || R.PhysicalAddress != Va) return FALSE;
  A = R.Descriptor;
  return (BOOLEAN)(R.Level == 2 && (((A >> 2) & 7U) == 3U) && (((A >> 8) & 3U) == 3U) && (A & BIT10) != 0);
}

STATIC BOOLEAN MMU3_TRUE_TEXT WalkWritableWb (IN UINT64 Va) {
  MOTO_G20_TRANSLATION_LOOKUP R;
  UINT64 A;
  if (EFI_ERROR (MotoG20LookupTranslation ((VOID *)(UINTN)MOTO_G20_TABLE_ROOT, Va, &R)) || !R.Mapped || R.PhysicalAddress != Va) return FALSE;
  A = R.Descriptor;
  return (BOOLEAN)(R.Level >= 2 && (((A >> 2) & 7U) == 3U) && (((A >> 8) & 3U) == 3U) && (A & BIT10) != 0 && (A & TT_AP_MASK) == TT_AP_RW_RW);
}

STATIC BOOLEAN MMU3_TRUE_TEXT WalkUncachedWritable (IN UINT64 Va) {
  MOTO_G20_TRANSLATION_LOOKUP R;
  UINT64 A;
  if (EFI_ERROR (MotoG20LookupTranslation ((VOID *)(UINTN)MOTO_G20_TABLE_ROOT, Va, &R)) || !R.Mapped || R.PhysicalAddress != Va) return FALSE;
  A = R.Descriptor;
  return (BOOLEAN)(R.Level >= 2 && ((A & TT_ATTR_INDX_MASK) == TT_ATTR_INDX_MEMORY_NON_CACHEABLE) && (A & TT_AF) != 0 && (A & TT_AP_MASK) == TT_AP_RW_RW && (A & TT_XN_MASK) != 0);
}

STATIC BOOLEAN MMU3_TRUE_TEXT TrueMmu3PageWalk (VOID) {
  UINT64 Va;
  STATIC CONST UINT64 DataPoints[] __attribute__((section(".mmu3true.rodata"))) = {
    0x80080000ULL, 0x80081000ULL, 0x81000000ULL, 0x8100DFFFULL,
    0x81882000ULL, 0x8197C000ULL, 0x81981000ULL, 0x81980000ULL,
    0x8197F000ULL, 0x8197E000ULL, 0x8197D000ULL, 0x8197C000ULL,
    0x811D0000ULL, 0x811E0000ULL - 1ULL, 0x811F0000ULL,
    0x81200000ULL - 1ULL, 0x82000000ULL, 0x82A0A000ULL,
    0x9E000000ULL, 0x9E000FFFULL, 0x9E9E3000ULL, 0x9E9E3FFFULL,
    0x81004000ULL, 0x810047FFULL, 0x81004860ULL, 0x810049A0ULL,
    0x810049A4ULL, 0x81008040ULL, 0x810080D7ULL,
    0x81008100ULL, 0x810081AFULL,
    (UINT64)(UINTN)MotoG20EnableMmuEl2MOnly,
    (UINT64)(UINTN)MotoG20Mmu3FirstPostEnableInstruction
  };

  for (Va = 0; Va < MOTO_G20_PE_IMAGE_SIZE; Va += 0x1000ULL) {
    if (!WalkExecutableWb (MOTO_G20_PE_BASE + Va)) return FALSE;
  }
  for (Va = 0; Va < sizeof (DataPoints) / sizeof (DataPoints[0]); Va++) {
    UINT64 Address = DataPoints[Va];
    if ((Address >= 0x9E000000ULL) && (Address < 0x9E9E4000ULL)) {
      if (!WalkUncachedWritable (Address)) return FALSE;
    } else if ((Address >= 0x811D0000ULL) && (Address < 0x81200000ULL)) {
      if (!WalkWritableWb (Address)) return FALSE;
    } else if ((Address >= 0x81008040ULL) && (Address < 0x81008200ULL)) {
      if (!WalkWritableWb (Address)) return FALSE;
    } else if ((Address >= 0x8197C000ULL) && (Address < 0x81982000ULL)) {
      if (!WalkWritableWb (Address)) return FALSE;
    } else if (!WalkExecutableWb (Address)) {
      return FALSE;
    }
  }
  return TRUE;
}

STATIC VOID MarkerAt (IN UINT32 Y, IN UINT32 Color) {
  volatile UINT32 *Fb = (volatile UINT32 *)(UINTN)MOTO_G20_FB_BASE;
  UINTN Row, Col;
  for (Row = 0; Row < MOTO_G20_MARKER_H; Row++) for (Col = 0; Col < MOTO_G20_MARKER_W; Col++) Fb[(Y + Row) * (MOTO_G20_FB_STRIDE / 4U) + MOTO_G20_MARKER_X + Col] = Color;
}

STATIC MMU3C1_CHECKPOINT_TEXT VOID FillStripe (
  IN UINT32 Y,
  IN UINT32 X,
  IN UINT32 Width,
  IN UINT32 Height,
  IN UINT32 Color
  )
{
  volatile UINT32 *Fb = (volatile UINT32 *)(UINTN)MOTO_G20_FB_BASE;
  UINTN Row, Col;
  for (Row = 0; Row < Height; Row++) {
    for (Col = 0; Col < Width; Col++) {
      Fb[(Y + Row) * (MOTO_G20_FB_STRIDE / 4U) + X + Col] = Color;
    }
  }
}

STATIC BOOLEAN ValidatePostEret (IN volatile MMU3B1_CAPTURE *Capture) {
  UINT64 CurrentSctlr = ReadSctlrAfter ();
  UINT64 CurrentVbar = ReadVbarBefore ();
  UINT64 CurrentEl = ReadCurrentEl ();
  UINT64 ExpectedMode = Capture->PreBrkSpsel == 0 ? 8ULL : 9ULL;
  UINT64 ExpectedVector = Capture->PreBrkSpsel == 0 ? 0ULL : 4ULL;
  return (BOOLEAN)(
    Capture->Signature == MOTO_G20_MMU3B2_SIGNATURE &&
    Capture->Status == 0xB2E2 &&
    Capture->VectorId == ExpectedVector &&
    (Capture->CurrentEl & 0xCULL) == 8ULL &&
    (CurrentEl & 0xCULL) == 8ULL &&
    Capture->Esr == MOTO_G20_MMU3B2_EXPECTED_ESR &&
    Capture->ElrEntry == Capture->BrkRuntimeExpected &&
    Capture->ElrResume == Capture->PostBrkRuntimeExpected &&
    Capture->ElrResume == Capture->ElrEntry + 4ULL &&
    (Capture->Spsr & 0xFULL) == ExpectedMode &&
    Capture->Vbar == 0x0000000081004000ULL &&
    CurrentVbar == 0x0000000081004000ULL &&
    (Capture->Sctlr & MOTO_G20_MMU_BITS) == 0 &&
    (CurrentSctlr & MOTO_G20_MMU_BITS) == 0 &&
    Capture->HandlerEnteredCookie == MOTO_G20_MMU3B2_ENTERED_COOKIE &&
    Capture->HandlerValidatedCookie == MOTO_G20_MMU3B2_VALIDATED_COOKIE &&
    Capture->PostEretCookie == MOTO_G20_MMU3B2_POST_ERET_COOKIE &&
    Capture->EntrySp != 0
  );
}

STATIC BOOLEAN MotoG20Mmu3b2SelfTest (VOID) {
  volatile MMU3B1_CAPTURE *Capture = &MotoG20Mmu3b1CaptureBlock;
  UINT64 CaptureAddress = (UINT64)(UINTN)Capture;
  UINT64 BrkAddress = (UINT64)(UINTN)MotoG20Mmu3b2BrkInstruction;
  UINT64 PostBrkAddress = (UINT64)(UINTN)MotoG20Mmu3b2PostBrk;
  UINT64 Spsel;
  BOOLEAN Mapped;

  Mapped = (BOOLEAN)(
    WalkExecutableWb ((UINT64)(UINTN)MotoG20Mmu3b2ControlledException) &&
    WalkExecutableWb ((UINT64)(UINTN)&MotoG20Mmu3b2Trigger) &&
    WalkExecutableWb (BrkAddress) &&
    WalkExecutableWb (PostBrkAddress) &&
    WalkNormalWb (CaptureAddress) &&
    WalkNormalWb (CaptureAddress + sizeof (*Capture) - 1)
  );
  if (!Mapped || PostBrkAddress != BrkAddress + 4ULL) return FALSE;

  SetMem ((VOID *)(UINTN)Capture, sizeof (*Capture), 0);
  Spsel = ReadSpsel ();
  Capture->Signature = MOTO_G20_MMU3B2_SIGNATURE;
  Capture->Status = 0xB200;
  Capture->PreBrkSpsel = Spsel;
  Capture->BrkRuntimeExpected = BrkAddress;
  Capture->PostBrkRuntimeExpected = PostBrkAddress;
  Capture->ExpectedEsr = MOTO_G20_MMU3B2_EXPECTED_ESR;
  MarkerAt (MOTO_G20_MMU3B2_MARKER_Y, 0x00FF0000U);
  MotoG20Mmu3b2Trigger ();
  Capture->PostEretCookie = MOTO_G20_MMU3B2_POST_ERET_COOKIE;
  if (!ValidatePostEret (Capture)) return FALSE;
  MotoG20DrawMmu3b2Diagnostics (0x03B2ULL, Capture->PreBrkSpsel, Capture->VectorId, Capture->Esr, Capture->ExpectedEsr, Capture->ElrEntry, Capture->ElrResume, Capture->Spsr, Capture->Far, Capture->EntrySp, Capture->Vbar, Capture->Sctlr, Capture->PostEretCookie);
  MarkerAt (MOTO_G20_MMU3B2_MARKER_Y, 0x0000FF00U);
  return TRUE;
}

STATIC BOOLEAN MMU3_TRUE_TEXT MotoG20TrueMmu3MOnly (VOID) {
  volatile TRUE_MMU3_CAPTURE *Capture = &MotoG20Mmu3TrueCaptureBlock;
  UINT64 StackPointer;
  UINT64 AssemblyDataCookie;
  UINT64 StackCookie;
  UINT64 DataCookie = MOTO_G20_TRUE_DATA_COOKIE;
  UINT64 RenderCookie = MOTO_G20_TRUE_RENDER_COOKIE;
  UINT64 FinalCookie = MOTO_G20_TRUE_FINAL_COOKIE;
  BOOLEAN Success;

  FillStripe (MOTO_G20_TRUE_MMU3_MARKER_Y, MOTO_G20_MARKER_X, MOTO_G20_MARKER_W, 8U, MOTO_G20_TRUE_ENTRY_COLOR);
  SetMem ((VOID *)(UINTN)Capture, sizeof (*Capture), 0);
  Capture->Signature = MOTO_G20_TRUE_MMU3_SIGNATURE;
  FillStripe (MOTO_G20_TRUE_MMU3_MARKER_Y + 8U, MOTO_G20_MARKER_X, MOTO_G20_MARKER_W, 8U, MOTO_G20_TRUE_CAPTURE_COLOR);

  // The page-table walk is independent of the transition assembly.  The
  // assembly repeats the live register checks immediately before MSR.
  if (!TrueMmu3PageWalk ()) { FillStripe (MOTO_G20_TRUE_MMU3_MARKER_Y + 16U, 624U, 24U, 8U, 0x00FF0000U); return FALSE; }
  gMotoG20Mmu3PreEnableOk = TRUE;
  FillStripe (MOTO_G20_TRUE_MMU3_MARKER_Y + 16U, MOTO_G20_MARKER_X, MOTO_G20_MARKER_W, 8U, MOTO_G20_TRUE_WALK_COLOR);

  Success = (BOOLEAN)(MotoG20EnableMmuEl2MOnly () == 1);
  if (!Success ||
      Capture->Signature != MOTO_G20_TRUE_MMU3_SIGNATURE ||
      Capture->PreEnableCookie != MOTO_G20_TRUE_PRE_COOKIE ||
      Capture->FirstPostEnableCookie != MOTO_G20_TRUE_FIRST_COOKIE ||
      Capture->PostEnableDataCookie != MOTO_G20_TRUE_DATA_COOKIE ||
      Capture->SctlrBefore != 0x0000000030C50830ULL ||
      Capture->SctlrAfter != 0x0000000030C50831ULL ||
      (Capture->SctlrBefore ^ Capture->SctlrAfter) != 1ULL ||
      Capture->TcrBefore != 0x0000000080823518ULL ||
      Capture->TcrAfter != 0x0000000080823518ULL ||
      Capture->Ttbr0Before != 0x0000000081981000ULL ||
      Capture->Ttbr0After != 0x0000000081981000ULL ||
      Capture->MairBefore != 0x00000000FFBB4400ULL ||
      Capture->MairAfter != 0x00000000FFBB4400ULL ||
      Capture->VbarBefore != 0x0000000081004000ULL ||
      Capture->VbarAfter != 0x0000000081004000ULL ||
      (Capture->SctlrAfter & MOTO_G20_MMU_BITS) != BIT0) {
    FillStripe (MOTO_G20_TRUE_MMU3_MARKER_Y + 24U, 624U, 24U, 8U, 0x00FF0000U); return FALSE;
  }

  AssemblyDataCookie = Capture->PostEnableDataCookie;
  gMotoG20Mmu3DataProbe = DataCookie;
  if (gMotoG20Mmu3DataProbe != DataCookie ||
      AssemblyDataCookie != MOTO_G20_TRUE_DATA_COOKIE) { FillStripe (MOTO_G20_TRUE_MMU3_MARKER_Y + 40U, 624U, 24U, 8U, 0x00FF0000U); return FALSE; }
  Capture->PostEnableDataCookie = gMotoG20Mmu3DataProbe;

  StackCookie = MOTO_G20_TRUE_STACK_COOKIE;
  if (StackCookie != MOTO_G20_TRUE_STACK_COOKIE) { FillStripe (MOTO_G20_TRUE_MMU3_MARKER_Y + 48U, 624U, 24U, 8U, 0x00FF0000U); return FALSE; }
  __asm__ volatile ("mov %0, sp" : "=r" (StackPointer));
  if ((StackPointer < 0x811F0000ULL) ||
      (StackPointer >= 0x81200000ULL)) { FillStripe (MOTO_G20_TRUE_MMU3_MARKER_Y + 48U, 624U, 24U, 8U, 0x00FF0000U); return FALSE; }
  Capture->PostEnableStackCookie = StackCookie;

  FillStripe (MOTO_G20_TRUE_MMU3_MARKER_Y + 48U, MOTO_G20_MARKER_X, MOTO_G20_MARKER_W, 8U, MOTO_G20_TRUE_RETURN_COLOR);
  MotoG20DrawTrueMmu3MOnlyDiagnostics (
    Capture->SctlrBefore, Capture->SctlrAfter, Capture->TcrAfter,
    Capture->Ttbr0After, Capture->MairAfter, Capture->VbarAfter,
    Capture->CurrentElBefore, Capture->HcrEl2Before,
    Capture->FirstPostEnableCookie, Capture->PostEnableDataCookie,
    Capture->PostEnableStackCookie, RenderCookie, FinalCookie,
    Capture->Phase
    );
  Capture->PostEnableRenderCookie = RenderCookie;
  if (Capture->PostEnableRenderCookie != MOTO_G20_TRUE_RENDER_COOKIE) { FillStripe (MOTO_G20_TRUE_MMU3_MARKER_Y + 56U, 624U, 24U, 8U, 0x00FF0000U); return FALSE; }
  Capture->FinalSuccessCookie = FinalCookie;
  if (Capture->FinalSuccessCookie != MOTO_G20_TRUE_FINAL_COOKIE) { FillStripe (MOTO_G20_TRUE_MMU3_MARKER_Y + 56U, 624U, 24U, 8U, 0x00FF0000U); return FALSE; }
  FillStripe (MOTO_G20_TRUE_MMU3_MARKER_Y + 56U, MOTO_G20_MARKER_X, MOTO_G20_MARKER_W, 8U, MOTO_G20_TRUE_SUCCESS_COLOR);
  MarkerAt (MOTO_G20_TRUE_MMU3_MARKER_Y, MOTO_G20_TRUE_SUCCESS_COLOR);
  return TRUE;
}

VOID EFIAPI MotoG20Mmu3b1Probe (VOID) {
  UINT64 Start = (UINT64)(UINTN)MotoG20Mmu3b1VectorTableStart;
  UINT64 End = (UINT64)(UINTN)MotoG20Mmu3b1VectorTableEnd;
  UINT64 StackBase = (UINT64)(UINTN)MotoG20Mmu3b1ExceptionStackBase;
  UINT64 StackTop = (UINT64)(UINTN)MotoG20Mmu3b1ExceptionStackTop;
  UINT64 Capture = (UINT64)(UINTN)&MotoG20Mmu3b1CaptureBlock;
  UINT64 Readback = 0;
  UINT64 VbarBefore = ReadVbarBefore ();
  UINT64 TcrDisplay = ReadTcrDisplay ();
  UINT64 Ttbr0Display = ReadTtbr0Display ();
  UINT64 MairDisplay = ReadMairDisplay ();
  UINT64 SctlrBefore = ReadSctlr ();
  UINT64 SctlrAfter = 0;
  BOOLEAN Range = (BOOLEAN)(End == Start + 0x800ULL);
  BOOLEAN Aligned = (BOOLEAN)((Start & 0x7FFULL) == 0);
  BOOLEAN VectorMap = (BOOLEAN)(Range && WalkExecutableWb (Start) && WalkExecutableWb (Start + 0x7FFULL));
  BOOLEAN HandlerMap = WalkExecutableWb ((UINT64)(UINTN)&MotoG20Mmu3b1UnexpectedException);
  BOOLEAN StackMap = (BOOLEAN)(StackTop == StackBase + 0x1000ULL && (StackBase & 0xFFFULL) == 0 && WalkNormalWb (StackBase) && WalkNormalWb (StackTop - 1));
  BOOLEAN CaptureMap = WalkNormalWb (Capture) && WalkNormalWb (Capture + sizeof (MMU3B1_CAPTURE) - 1);
  BOOLEAN BeforeMciOff = (BOOLEAN)((SctlrBefore & MOTO_G20_MMU_BITS) == 0);
  BOOLEAN VbarOk = FALSE;
  if (BeforeMciOff && Range && Aligned && VectorMap && HandlerMap && StackMap && CaptureMap) VbarOk = MotoG20Mmu3b1WriteVbarReadback (Start, &Readback);
  if (VbarOk && Readback == Start) SctlrAfter = ReadSctlrAfter ();
  BOOLEAN SctlrPreserved = (BOOLEAN)(VbarOk && Readback == Start && SctlrAfter == SctlrBefore && (SctlrAfter & MOTO_G20_MMU_BITS) == 0);
  BOOLEAN Green = (BOOLEAN)(Range && Aligned && VectorMap && HandlerMap && StackMap && CaptureMap && BeforeMciOff && VbarOk && Readback == Start && SctlrPreserved);
  MotoG20DrawMmu3b1Diagnostics (VbarBefore, Readback, SctlrBefore, SctlrAfter, TcrDisplay, Ttbr0Display, MairDisplay);
  MarkerAt (MOTO_G20_MMU3B1_MARKER_Y, Green ? 0x0000FF00U : 0x00FF0000U);
  MarkerAt (MOTO_G20_TRUE_MMU3_MARKER_Y, 0x00000000U);
  if (Green && MotoG20Mmu3b2SelfTest ()) {
    if (!MotoG20TrueMmu3MOnly ()) {
      for (;;) __asm__ volatile ("wfe");
    }
  }
  for (;;) __asm__ volatile ("wfe");
}
