#include <Base.h>
#include <Library/MotoG20PreOsTelemetryLib.h>

#define MOTO_G20_SCTLR_M  0x0000000000000001ULL
#define MOTO_G20_SCTLR_C  0x0000000000000004ULL

#define M3_OFFSET_MAGIC             0U
#define M3_OFFSET_VERSION           8U
#define M3_OFFSET_HEADER_SIZE       10U
#define M3_OFFSET_FRAME_SIZE        12U
#define M3_OFFSET_CANONICAL_SHA256  16U
#define M3_OFFSET_BUILD_ID          48U
#define M3_OFFSET_SEQUENCE          64U
#define M3_OFFSET_ATTEMPT           72U
#define M3_OFFSET_STAGE             76U
#define M3_OFFSET_PHASE             78U
#define M3_OFFSET_FAILURE_CODE      80U
#define M3_OFFSET_CHECKPOINTS       84U
#define M3_OFFSET_CURRENT_EL        92U
#define M3_OFFSET_SCTLR_EL2         100U
#define M3_OFFSET_TCR_EL2           108U
#define M3_OFFSET_TTBR0_EL2         116U
#define M3_OFFSET_MAIR_EL2          124U
#define M3_OFFSET_VBAR_EL2          132U
#define M3_OFFSET_ESR_EL2           140U
#define M3_OFFSET_ELR_EL2           148U
#define M3_OFFSET_FAR_EL2           156U
#define M3_OFFSET_SPSR_EL2          164U
#define M3_OFFSET_VECTOR_SLOT       172U
#define M3_OFFSET_STACK_POINTER     180U
#define M3_OFFSET_MONOTONIC_TICKS   188U
#define M3_OFFSET_CRC32             196U

STATIC CONST UINT8  mMotoG20TelemetryV2Magic[MOTO_G20_TELEMETRY_V2_MAGIC_SIZE] = {
  'M', '3', 'T', 'E', 'L', 'M', '0', '2'
};

STATIC CONST UINT8  mMotoG20RamoopsKmsgHeader[8] = {
  '=', '=', '=', '=', '0', '.', '0', '\n'
};

STATIC
VOID
MotoG20PutUint16 (
  OUT UINT8   *Buffer,
  IN  UINT16  Value
  )
{
  Buffer[0] = (UINT8)Value;
  Buffer[1] = (UINT8)(Value >> 8);
}

STATIC
VOID
MotoG20PutUint32 (
  OUT UINT8   *Buffer,
  IN  UINT32  Value
  )
{
  UINTN  Index;

  for (Index = 0; Index < 4; Index++) {
    Buffer[Index] = (UINT8)(Value >> (Index * 8));
  }
}

STATIC
VOID
MotoG20PutUint64 (
  OUT UINT8   *Buffer,
  IN  UINT64  Value
  )
{
  UINTN  Index;

  for (Index = 0; Index < 8; Index++) {
    Buffer[Index] = (UINT8)(Value >> (Index * 8));
  }
}

STATIC
UINT16
MotoG20GetUint16 (
  IN CONST UINT8  *Buffer
  )
{
  return (UINT16)(Buffer[0] | ((UINT16)Buffer[1] << 8));
}

STATIC
UINT32
MotoG20GetUint32 (
  IN CONST UINT8  *Buffer
  )
{
  UINTN   Index;
  UINT32  Value;

  Value = 0;
  for (Index = 0; Index < 4; Index++) {
    Value |= (UINT32)Buffer[Index] << (Index * 8);
  }

  return Value;
}

STATIC
VOID
MotoG20CopyBytes (
  OUT UINT8        *Destination,
  IN  CONST UINT8  *Source,
  IN  UINTN        Size
  )
{
  UINTN  Index;

  for (Index = 0; Index < Size; Index++) {
    Destination[Index] = Source[Index];
  }
}

STATIC
VOID
MotoG20CopyBytesVolatile (
  OUT volatile UINT8  *Destination,
  IN  CONST UINT8     *Source,
  IN  UINTN           Size
  )
{
  UINTN  Index;

  for (Index = 0; Index < Size; Index++) {
    Destination[Index] = Source[Index];
  }
}

STATIC
VOID
MotoG20ZeroBytes (
  OUT UINT8  *Destination,
  IN  UINTN  Size
  )
{
  UINTN  Index;

  for (Index = 0; Index < Size; Index++) {
    Destination[Index] = 0;
  }
}

STATIC
VOID
MotoG20WriteUint32Volatile (
  OUT volatile UINT8  *Address,
  IN  UINT32          Value
  )
{
  *(volatile UINT32 *)(VOID *)Address = Value;
}

STATIC
VOID
MotoG20DataSynchronizationBarrier (
  VOID
  )
{
#if defined (MOTO_G20_TELEMETRY_HOST_TEST)
  __asm__ __volatile__ ("" ::: "memory");
#else
  __asm__ __volatile__ ("dsb sy" ::: "memory");
#endif
}

#if !defined (MOTO_G20_TELEMETRY_HOST_TEST)
STATIC
UINT64
MotoG20ReadSctlrEl2 (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, SCTLR_EL2" : "=r" (Value));
  return Value;
}
#endif

STATIC
BOOLEAN
MotoG20CanonicalIdentityIsBound (
  IN CONST UINT8  Identity[MOTO_G20_TELEMETRY_V2_SHA256_SIZE]
  )
{
  UINTN  Index;
  UINT8  Combined;

  Combined = 0;
  for (Index = 0; Index < MOTO_G20_TELEMETRY_V2_SHA256_SIZE; Index++) {
    Combined |= Identity[Index];
  }

  return (BOOLEAN)(Combined != 0);
}

STATIC
BOOLEAN
MotoG20BuildIdIsValid (
  IN CONST UINT8  BuildId[MOTO_G20_TELEMETRY_V2_BUILD_ID_SIZE],
  IN UINTN        BuildIdLength
  )
{
  UINTN  Index;

  if ((BuildIdLength == 0) ||
      (BuildIdLength > MOTO_G20_TELEMETRY_V2_BUILD_ID_SIZE)) {
    return FALSE;
  }

  for (Index = 0; Index < BuildIdLength; Index++) {
    if ((BuildId[Index] == 0) || (BuildId[Index] >= 0x80)) {
      return FALSE;
    }
  }

  return TRUE;
}

UINT32
EFIAPI
MotoG20TelemetryV2Crc32 (
  IN CONST UINT8  *Data,
  IN UINTN        DataSize
  )
{
  UINT32  Crc;
  UINTN   ByteIndex;
  UINTN   BitIndex;

  if ((Data == NULL) && (DataSize != 0)) {
    return 0;
  }

  Crc = 0xFFFFFFFFU;
  for (ByteIndex = 0; ByteIndex < DataSize; ByteIndex++) {
    Crc ^= Data[ByteIndex];
    for (BitIndex = 0; BitIndex < 8; BitIndex++) {
      Crc = (Crc >> 1) ^ ((0U - (Crc & 1U)) & 0xEDB88320U);
    }
  }

  return ~Crc;
}

BOOLEAN
EFIAPI
MotoG20TelemetryV2Encode (
  IN  CONST MOTO_G20_TELEMETRY_V2_INPUT  *Input,
  OUT UINT8                              Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE]
  )
{
  UINT32  Crc;

  if ((Input == NULL) || (Frame == NULL) ||
      !MotoG20CanonicalIdentityIsBound (Input->CanonicalCandidateSha256) ||
      !MotoG20BuildIdIsValid (Input->BuildId, Input->BuildIdLength)) {
    return FALSE;
  }

  MotoG20ZeroBytes (Frame, MOTO_G20_TELEMETRY_V2_FRAME_SIZE);
  MotoG20CopyBytes (
    Frame + M3_OFFSET_MAGIC,
    mMotoG20TelemetryV2Magic,
    sizeof (mMotoG20TelemetryV2Magic)
    );
  MotoG20PutUint16 (Frame + M3_OFFSET_VERSION, 2U);
  MotoG20PutUint16 (
    Frame + M3_OFFSET_HEADER_SIZE,
    MOTO_G20_TELEMETRY_V2_BODY_SIZE
    );
  MotoG20PutUint32 (
    Frame + M3_OFFSET_FRAME_SIZE,
    MOTO_G20_TELEMETRY_V2_FRAME_SIZE
    );
  MotoG20CopyBytes (
    Frame + M3_OFFSET_CANONICAL_SHA256,
    Input->CanonicalCandidateSha256,
    MOTO_G20_TELEMETRY_V2_SHA256_SIZE
    );
  MotoG20CopyBytes (
    Frame + M3_OFFSET_BUILD_ID,
    Input->BuildId,
    Input->BuildIdLength
    );
  MotoG20PutUint64 (Frame + M3_OFFSET_SEQUENCE, Input->Sequence);
  MotoG20PutUint32 (Frame + M3_OFFSET_ATTEMPT, Input->AttemptNumber);
  MotoG20PutUint16 (Frame + M3_OFFSET_STAGE, Input->Stage);
  MotoG20PutUint16 (Frame + M3_OFFSET_PHASE, Input->Phase);
  MotoG20PutUint32 (Frame + M3_OFFSET_FAILURE_CODE, Input->FailureCode);
  MotoG20PutUint64 (Frame + M3_OFFSET_CHECKPOINTS, Input->CheckpointBitmap);
  MotoG20PutUint64 (Frame + M3_OFFSET_CURRENT_EL, Input->CurrentEl);
  MotoG20PutUint64 (Frame + M3_OFFSET_SCTLR_EL2, Input->SctlrEl2);
  MotoG20PutUint64 (Frame + M3_OFFSET_TCR_EL2, Input->TcrEl2);
  MotoG20PutUint64 (Frame + M3_OFFSET_TTBR0_EL2, Input->Ttbr0El2);
  MotoG20PutUint64 (Frame + M3_OFFSET_MAIR_EL2, Input->MairEl2);
  MotoG20PutUint64 (Frame + M3_OFFSET_VBAR_EL2, Input->VbarEl2);
  MotoG20PutUint64 (Frame + M3_OFFSET_ESR_EL2, Input->EsrEl2);
  MotoG20PutUint64 (Frame + M3_OFFSET_ELR_EL2, Input->ElrEl2);
  MotoG20PutUint64 (Frame + M3_OFFSET_FAR_EL2, Input->FarEl2);
  MotoG20PutUint64 (Frame + M3_OFFSET_SPSR_EL2, Input->SpsrEl2);
  MotoG20PutUint64 (Frame + M3_OFFSET_VECTOR_SLOT, Input->VectorSlot);
  MotoG20PutUint64 (Frame + M3_OFFSET_STACK_POINTER, Input->StackPointer);
  MotoG20PutUint64 (Frame + M3_OFFSET_MONOTONIC_TICKS, Input->MonotonicTicks);
  Crc = MotoG20TelemetryV2Crc32 (Frame, MOTO_G20_TELEMETRY_V2_BODY_SIZE);
  MotoG20PutUint32 (Frame + M3_OFFSET_CRC32, Crc);
  return TRUE;
}

BOOLEAN
EFIAPI
MotoG20TelemetryV2ValidateFrame (
  IN CONST UINT8  Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE]
  )
{
  UINTN    Index;
  BOOLEAN  FoundBuildByte;
  BOOLEAN  FoundBuildTerminator;

  if (Frame == NULL) {
    return FALSE;
  }

  for (Index = 0; Index < MOTO_G20_TELEMETRY_V2_MAGIC_SIZE; Index++) {
    if (Frame[M3_OFFSET_MAGIC + Index] != mMotoG20TelemetryV2Magic[Index]) {
      return FALSE;
    }
  }

  if ((MotoG20GetUint16 (Frame + M3_OFFSET_VERSION) != 2U) ||
      (MotoG20GetUint16 (Frame + M3_OFFSET_HEADER_SIZE) !=
       MOTO_G20_TELEMETRY_V2_BODY_SIZE) ||
      (MotoG20GetUint32 (Frame + M3_OFFSET_FRAME_SIZE) !=
       MOTO_G20_TELEMETRY_V2_FRAME_SIZE) ||
      !MotoG20CanonicalIdentityIsBound (
         Frame + M3_OFFSET_CANONICAL_SHA256
         )) {
    return FALSE;
  }

  FoundBuildByte       = FALSE;
  FoundBuildTerminator = FALSE;
  for (Index = 0; Index < MOTO_G20_TELEMETRY_V2_BUILD_ID_SIZE; Index++) {
    UINT8  Value;

    Value = Frame[M3_OFFSET_BUILD_ID + Index];
    if (Value == 0) {
      FoundBuildTerminator = TRUE;
    } else {
      if (FoundBuildTerminator || (Value >= 0x80)) {
        return FALSE;
      }

      FoundBuildByte = TRUE;
    }
  }

  return (BOOLEAN)(
           FoundBuildByte &&
           (MotoG20GetUint32 (Frame + M3_OFFSET_CRC32) ==
            MotoG20TelemetryV2Crc32 (
              Frame,
              MOTO_G20_TELEMETRY_V2_BODY_SIZE
              ))
           );
}

STATIC
BOOLEAN
MotoG20TelemetryV2CommitToZone (
  IN OUT volatile UINT8  *Zone,
  IN     UINTN           ZoneSize,
  IN     CONST UINT8     Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE],
  IN     UINT64          SctlrEl2
#if defined (MOTO_G20_TELEMETRY_HOST_TEST)
  ,
  IN     UINT32          StopAfterStage
#endif
  )
{
  if ((Zone == NULL) ||
      (((UINTN)Zone & 3U) != 0) ||
      (ZoneSize != MOTO_G20_RAMOOPS_RECORD_SIZE) ||
      ((SctlrEl2 & (MOTO_G20_SCTLR_M | MOTO_G20_SCTLR_C)) != 0) ||
      !MotoG20TelemetryV2ValidateFrame (Frame)) {
    return FALSE;
  }

  // Invalidate the old record before changing any payload bytes.
  MotoG20WriteUint32Volatile (Zone, 0U);
  MotoG20DataSynchronizationBarrier ();
#if defined (MOTO_G20_TELEMETRY_HOST_TEST)
  if (StopAfterStage == MOTO_G20_TELEMETRY_COMMIT_AFTER_INVALID) {
    return TRUE;
  }
#endif

  MotoG20CopyBytesVolatile (
    Zone + 12U,
    mMotoG20RamoopsKmsgHeader,
    sizeof (mMotoG20RamoopsKmsgHeader)
    );
  MotoG20CopyBytesVolatile (
    Zone + 20U,
    Frame,
    MOTO_G20_TELEMETRY_V2_FRAME_SIZE
    );
  MotoG20DataSynchronizationBarrier ();
#if defined (MOTO_G20_TELEMETRY_HOST_TEST)
  if (StopAfterStage == MOTO_G20_TELEMETRY_COMMIT_AFTER_PAYLOAD) {
    return TRUE;
  }
#endif

  MotoG20WriteUint32Volatile (
    Zone + 4U,
    MOTO_G20_RAMOOPS_RECORD_PAYLOAD_SIZE
    );
  MotoG20WriteUint32Volatile (
    Zone + 8U,
    MOTO_G20_RAMOOPS_RECORD_PAYLOAD_SIZE
    );
  MotoG20DataSynchronizationBarrier ();
#if defined (MOTO_G20_TELEMETRY_HOST_TEST)
  if (StopAfterStage == MOTO_G20_TELEMETRY_COMMIT_AFTER_GEOMETRY) {
    return TRUE;
  }
#endif

  // The persistent signature is the sole commit marker and is stored last.
  MotoG20WriteUint32Volatile (
    Zone,
    MOTO_G20_RAMOOPS_PERSISTENT_SIGNATURE
    );
  MotoG20DataSynchronizationBarrier ();
  return TRUE;
}

BOOLEAN
EFIAPI
MotoG20TelemetryV2CommitPreMmuFrame (
  IN CONST UINT8  Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE]
  )
{
#if defined (MOTO_G20_TELEMETRY_HOST_TEST)
  // A host build must never dereference the physical ramoops address.
  (VOID)Frame;
  return FALSE;
#else
  return MotoG20TelemetryV2CommitToZone (
           (volatile UINT8 *)(UINTN)MOTO_G20_RAMOOPS_TELEMETRY_ZONE_BASE,
           MOTO_G20_RAMOOPS_RECORD_SIZE,
           Frame,
           MotoG20ReadSctlrEl2 ()
           );
#endif
}

BOOLEAN
EFIAPI
MotoG20TelemetryV2PublishPreMmu (
  IN  CONST MOTO_G20_TELEMETRY_V2_INPUT  *Input,
  OUT UINT8                              FrameScratch[MOTO_G20_TELEMETRY_V2_FRAME_SIZE]
  )
{
  if (!MotoG20TelemetryV2Encode (Input, FrameScratch)) {
    return FALSE;
  }

  return MotoG20TelemetryV2CommitPreMmuFrame (FrameScratch);
}

#if defined (MOTO_G20_TELEMETRY_HOST_TEST)
BOOLEAN
EFIAPI
MotoG20TelemetryV2HostCommitToZone (
  IN OUT volatile UINT8  *Zone,
  IN     UINTN           ZoneSize,
  IN     CONST UINT8     Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE],
  IN     UINT64          SimulatedSctlrEl2,
  IN     UINT32          StopAfterStage
  )
{
  if (StopAfterStage > MOTO_G20_TELEMETRY_COMMIT_AFTER_SIGNATURE) {
    return FALSE;
  }

  return MotoG20TelemetryV2CommitToZone (
           Zone,
           ZoneSize,
           Frame,
           SimulatedSctlrEl2,
           StopAfterStage
           );
}
#endif
