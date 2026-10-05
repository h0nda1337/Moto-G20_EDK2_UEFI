#include <Base.h>
#include <Library/MotoG20PreOsTelemetryLib.h>

#if defined(__clang__)
#define MMU3C2C_TELEMETRY_TEXT \
  __attribute__((section(".mmu3c2c.telemetry.text"), noinline))
#else
#define MMU3C2C_TELEMETRY_TEXT \
  __attribute__((section(".mmu3c2c.telemetry.text"), optimize("Os"), noinline))
#endif

#define MMU3C2C_TELEMETRY_IDENTITY \
  __attribute__((section(".mmu3c2c.telemetry.identity"), used, aligned(16)))

#define MOTO_G20_MMU3C2C_STAGE             0x3C2CU
#define MOTO_G20_MMU3C2C_FAILURE_UNEXPECTED 0x0000E001U

// This is the sole in-image telemetry identity field for the MMU3C2C
// template.  It must remain exactly zero until the host-side canonical
// identity binder creates a new, separately named candidate.
volatile CONST UINT8 gMotoG20Mmu3c2cCanonicalCandidateSha256[
  MOTO_G20_TELEMETRY_V2_SHA256_SIZE
  ] MMU3C2C_TELEMETRY_IDENTITY = { 0 };

STATIC
UINT64
MotoG20Mmu3c2cReadCurrentEl (
  VOID
  )
{
  UINT64 Value;
  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20Mmu3c2cReadSctlrEl2 (
  VOID
  )
{
  UINT64 Value;
  __asm__ volatile ("mrs %0, SCTLR_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20Mmu3c2cReadTcrEl2 (
  VOID
  )
{
  UINT64 Value;
  __asm__ volatile ("mrs %0, TCR_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20Mmu3c2cReadTtbr0El2 (
  VOID
  )
{
  UINT64 Value;
  __asm__ volatile ("mrs %0, TTBR0_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20Mmu3c2cReadMairEl2 (
  VOID
  )
{
  UINT64 Value;
  __asm__ volatile ("mrs %0, MAIR_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20Mmu3c2cReadVbarEl2 (
  VOID
  )
{
  UINT64 Value;
  __asm__ volatile ("mrs %0, VBAR_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20Mmu3c2cReadEsrEl2 (
  VOID
  )
{
  UINT64 Value;
  __asm__ volatile ("mrs %0, ESR_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20Mmu3c2cReadElrEl2 (
  VOID
  )
{
  UINT64 Value;
  __asm__ volatile ("mrs %0, ELR_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20Mmu3c2cReadFarEl2 (
  VOID
  )
{
  UINT64 Value;
  __asm__ volatile ("mrs %0, FAR_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20Mmu3c2cReadSpsrEl2 (
  VOID
  )
{
  UINT64 Value;
  __asm__ volatile ("mrs %0, SPSR_EL2" : "=r" (Value));
  return Value;
}

VOID
EFIAPI
MMU3C2C_TELEMETRY_TEXT
MotoG20Mmu3c2cPublishUnexpectedTelemetry (
  IN UINT64 VectorSlot,
  IN UINT64 InterruptedStackPointer
  )
{
  MOTO_G20_TELEMETRY_V2_INPUT Input;
  UINT8                         Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE];
  UINTN                         Index;

  for (Index = 0; Index < MOTO_G20_TELEMETRY_V2_SHA256_SIZE; Index++) {
    Input.CanonicalCandidateSha256[Index] =
      gMotoG20Mmu3c2cCanonicalCandidateSha256[Index];
  }

  for (Index = 0; Index < MOTO_G20_TELEMETRY_V2_BUILD_ID_SIZE; Index++) {
    Input.BuildId[Index] = 0;
  }

  Input.BuildId[0] = 'M';
  Input.BuildId[1] = 'M';
  Input.BuildId[2] = 'U';
  Input.BuildId[3] = '3';
  Input.BuildId[4] = 'C';
  Input.BuildId[5] = '2';
  Input.BuildId[6] = 'C';
  Input.BuildId[7] = '1';
  Input.BuildIdLength = 8;

  Input.Sequence         = 1;
  Input.AttemptNumber    = 1;
  Input.Stage            = MOTO_G20_MMU3C2C_STAGE;
  Input.Phase            = 0;
  Input.FailureCode      = MOTO_G20_MMU3C2C_FAILURE_UNEXPECTED;
  Input.CheckpointBitmap = 0;
  Input.CurrentEl        = MotoG20Mmu3c2cReadCurrentEl ();
  Input.SctlrEl2         = MotoG20Mmu3c2cReadSctlrEl2 ();
  Input.TcrEl2           = MotoG20Mmu3c2cReadTcrEl2 ();
  Input.Ttbr0El2         = MotoG20Mmu3c2cReadTtbr0El2 ();
  Input.MairEl2          = MotoG20Mmu3c2cReadMairEl2 ();
  Input.VbarEl2          = MotoG20Mmu3c2cReadVbarEl2 ();
  Input.EsrEl2           = MotoG20Mmu3c2cReadEsrEl2 ();
  Input.ElrEl2           = MotoG20Mmu3c2cReadElrEl2 ();
  Input.FarEl2           = MotoG20Mmu3c2cReadFarEl2 ();
  Input.SpsrEl2          = MotoG20Mmu3c2cReadSpsrEl2 ();
  Input.VectorSlot       = VectorSlot;
  Input.StackPointer     = InterruptedStackPointer;
  Input.MonotonicTicks   = 0;

  // An unbound all-zero template identity, an M=1/C=1 state, or any malformed
  // input fails closed inside the library.  The exception handler preserves
  // its visible legacy evidence regardless of this return value.
  (VOID)MotoG20TelemetryV2PublishPreMmu (&Input, Frame);
}
