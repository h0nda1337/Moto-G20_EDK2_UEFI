#ifndef MOTO_G20_PRE_OS_TELEMETRY_LIB_H_
#define MOTO_G20_PRE_OS_TELEMETRY_LIB_H_

#include <Base.h>

#define MOTO_G20_TELEMETRY_V2_MAGIC_SIZE          8U
#define MOTO_G20_TELEMETRY_V2_BUILD_ID_SIZE       16U
#define MOTO_G20_TELEMETRY_V2_SHA256_SIZE         32U
#define MOTO_G20_TELEMETRY_V2_BODY_SIZE           196U
#define MOTO_G20_TELEMETRY_V2_FRAME_SIZE          200U

#define MOTO_G20_RAMOOPS_BASE                     0xFFF80000ULL
#define MOTO_G20_RAMOOPS_SIZE                     0x00040000U
#define MOTO_G20_RAMOOPS_RECORD_SIZE              0x00008000U
#define MOTO_G20_RAMOOPS_TELEMETRY_ZONE_INDEX     5U
#define MOTO_G20_RAMOOPS_TELEMETRY_ZONE_BASE      0xFFFA8000ULL
#define MOTO_G20_RAMOOPS_PERSISTENT_SIGNATURE     0x43474244U
#define MOTO_G20_RAMOOPS_RECORD_PAYLOAD_SIZE      208U

#define MOTO_G20_TELEMETRY_COMMIT_COMPLETE        0U
#define MOTO_G20_TELEMETRY_COMMIT_AFTER_INVALID   1U
#define MOTO_G20_TELEMETRY_COMMIT_AFTER_PAYLOAD   2U
#define MOTO_G20_TELEMETRY_COMMIT_AFTER_GEOMETRY  3U
#define MOTO_G20_TELEMETRY_COMMIT_AFTER_SIGNATURE 4U

typedef struct {
  UINT8   CanonicalCandidateSha256[MOTO_G20_TELEMETRY_V2_SHA256_SIZE];
  UINT8   BuildId[MOTO_G20_TELEMETRY_V2_BUILD_ID_SIZE];
  UINTN   BuildIdLength;
  UINT64  Sequence;
  UINT32  AttemptNumber;
  UINT16  Stage;
  UINT16  Phase;
  UINT32  FailureCode;
  UINT64  CheckpointBitmap;
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
  UINT64  VectorSlot;
  UINT64  StackPointer;
  UINT64  MonotonicTicks;
} MOTO_G20_TELEMETRY_V2_INPUT;

UINT32
EFIAPI
MotoG20TelemetryV2Crc32 (
  IN CONST UINT8  *Data,
  IN UINTN        DataSize
  );

BOOLEAN
EFIAPI
MotoG20TelemetryV2Encode (
  IN  CONST MOTO_G20_TELEMETRY_V2_INPUT  *Input,
  OUT UINT8                              Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE]
  );

BOOLEAN
EFIAPI
MotoG20TelemetryV2ValidateFrame (
  IN CONST UINT8  Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE]
  );

// This writer is deliberately restricted to the currently proven pre-MMU
// state.  It refuses to touch ramoops if SCTLR_EL2.M or SCTLR_EL2.C is set.
// Post-MMU publication requires a separately audited identity mapping for the
// complete ramoops region and is not provided by this API.
BOOLEAN
EFIAPI
MotoG20TelemetryV2CommitPreMmuFrame (
  IN CONST UINT8  Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE]
  );

// FrameScratch is caller-owned so this path performs no pool/page allocation.
BOOLEAN
EFIAPI
MotoG20TelemetryV2PublishPreMmu (
  IN  CONST MOTO_G20_TELEMETRY_V2_INPUT  *Input,
  OUT UINT8                              FrameScratch[MOTO_G20_TELEMETRY_V2_FRAME_SIZE]
  );

#if defined (MOTO_G20_TELEMETRY_HOST_TEST)
// Test-only fault boundary.  The production library never exposes a partial
// commit control and always runs through the final signature publication.
BOOLEAN
EFIAPI
MotoG20TelemetryV2HostCommitToZone (
  IN OUT volatile UINT8  *Zone,
  IN     UINTN           ZoneSize,
  IN     CONST UINT8     Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE],
  IN     UINT64          SimulatedSctlrEl2,
  IN     UINT32          StopAfterStage
  );
#endif

#endif
