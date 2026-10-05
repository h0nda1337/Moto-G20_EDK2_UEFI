#ifndef MOTO_G20_POST_MMU_TELEMETRY_LIB_H_
#define MOTO_G20_POST_MMU_TELEMETRY_LIB_H_

#include <Base.h>
#include <Library/MotoG20PreOsTelemetryLib.h>

#define MOTO_G20_POST_MMU_EXPECTED_TCR_EL2         0x0000000080823518ULL
#define MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2       0x0000000081981000ULL
#define MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2        0x00000000FFBB4400ULL
#define MOTO_G20_POST_MMU_EXPECTED_SCTLR_M_ONLY    0x0000000030C50831ULL
#define MOTO_G20_POST_MMU_EXPECTED_SCTLR_M_AND_C   0x0000000030C50835ULL
#define MOTO_G20_POST_MMU_TABLE_BASE               0x8197A000ULL
#define MOTO_G20_POST_MMU_TABLE_LIMIT              0x81982000ULL
#define MOTO_G20_POST_MMU_TABLE_PAGE_COUNT         8U

// This validates the exact active EL2 translation regime required by the
// mapped writer.  It is read-only and checks every 4 KiB ramoops page plus
// both adjacent guard pages before any telemetry store is permitted.  Only
// the derived M-only and M+C SCTLR_EL2 states are accepted.
BOOLEAN
EFIAPI
MotoG20TelemetryV2ValidateActivePostMmuMap (
  IN CONST VOID  *TranslationTableBase,
  IN UINT64      SctlrEl2,
  IN UINT64      TcrEl2,
  IN UINT64      Ttbr0El2,
  IN UINT64      MairEl2
  );

// Production entry point.  It reads the live EL2 translation registers,
// validates the complete Device/XN identity mapping, and publishes the
// persistent signature only after payload/geometry readback and a second map
// validation.  The frozen pre-MMU writer remains a separate API.
BOOLEAN
EFIAPI
MotoG20TelemetryV2CommitPostMmuFrame (
  IN CONST UINT8  Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE]
  );

// FrameScratch is caller-owned; this library performs no allocation.
BOOLEAN
EFIAPI
MotoG20TelemetryV2PublishPostMmu (
  IN  CONST MOTO_G20_TELEMETRY_V2_INPUT  *Input,
  OUT UINT8                              FrameScratch[MOTO_G20_TELEMETRY_V2_FRAME_SIZE]
  );

#if defined (MOTO_G20_POST_MMU_TELEMETRY_HOST_TEST)
// Test-only entry point.  The production API never accepts simulated
// registers, an alternate zone, or a partial-commit boundary.
BOOLEAN
EFIAPI
MotoG20TelemetryV2HostCommitPostMmuToZone (
  IN OUT volatile UINT8  *Zone,
  IN     UINTN           ZoneSize,
  IN     CONST UINT8     Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE],
  IN     CONST VOID      *TranslationTableBase,
  IN     UINT64          SimulatedSctlrEl2,
  IN     UINT64          SimulatedTcrEl2,
  IN     UINT64          SimulatedTtbr0El2,
  IN     UINT64          SimulatedMairEl2,
  IN     UINT32          StopAfterStage
  );
#endif

#endif
