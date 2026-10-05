/** @file
  Bounded DXE telemetry for the Moto G20 Foundation boot path.

  The interface is deliberately allocation-free so it remains usable before
  DXE memory services, from ASSERT paths, and from synchronous exceptions.
**/

#ifndef MOTO_G20_DXE_TELEMETRY_LIB_H_
#define MOTO_G20_DXE_TELEMETRY_LIB_H_

#include <PiDxe.h>
#include <Protocol/DebugSupport.h>

typedef enum {
  MotoG20DxeStageNone                 = 0x00,
  MotoG20DxeStageCoreEntry            = 0x01,
  MotoG20DxeStageMainEntry            = 0x02,
  MotoG20DxeStageExceptionsReady      = 0x03,
  MotoG20DxeStageMemoryServices       = 0x04,
  MotoG20DxeStageHandleServices       = 0x05,
  MotoG20DxeStageImageServices        = 0x06,
  MotoG20DxeStageGcdServices          = 0x07,
  MotoG20DxeStageSystemTables         = 0x08,
  MotoG20DxeStageConstructors         = 0x09,
  MotoG20DxeStageEvents               = 0x0A,
  MotoG20DxeStageFirmwareVolumes      = 0x0B,
  MotoG20DxeStageDispatcherReady      = 0x0C,
  MotoG20DxeStageDispatcherEnter      = 0x0D,
  MotoG20DxeStageDispatcherReturned   = 0x0E,
  MotoG20DxeStageArchitecturalReady   = 0x0F,
  MotoG20DxeStageBdsTransfer          = 0x10,
  MotoG20DxeStageProgressMonitor      = 0x20,
  MotoG20DxeStageCpuArch              = 0x21,
  MotoG20DxeStageInterruptArch        = 0x22,
  MotoG20DxeStageTimerArch            = 0x23,
  MotoG20DxeStageGraphicsOutput       = 0x24,
  MotoG20DxeStageTextOutput           = 0x25,
  MotoG20DxeStageBdsArch              = 0x26,
  MotoG20DxeStageBeforeConsole        = 0x30,
  MotoG20DxeStageConsoleReady         = 0x31,
  MotoG20DxeStageShellLoad            = 0x32,
  MotoG20DxeStageShellStart           = 0x33,
  MotoG20DxeStageBdsEntry             = 0x34,
  MotoG20DxeStageConOutConnectCall    = 0x35,
  MotoG20DxeStageConOutConnectReturn  = 0x36,
  MotoG20DxeStageErrOutConnectCall    = 0x37,
  MotoG20DxeStageErrOutConnectReturn  = 0x38,
  MotoG20DxeStagePhysicalConsole      = 0x39,
  MotoG20DxeStageConsoleWrite         = 0x3A,
  MotoG20DxeStageShellLoadReturn      = 0x3B,
  MotoG20DxeStageShellReturn          = 0x3C,
  MotoG20DxeStageCoreReturned         = 0xE0,
  MotoG20DxeStageException            = 0xE1,
  MotoG20DxeStageAssert               = 0xE2,
  MotoG20DxeStageFailure              = 0xE3,
  MotoG20DxeStageArchitecturalMissing = 0xE4
} MOTO_G20_DXE_STAGE;

VOID
EFIAPI
MotoG20DxeTelemetryCheckpoint (
  IN MOTO_G20_DXE_STAGE  Stage,
  IN EFI_STATUS          Status,
  IN UINT64              Detail0,
  IN UINT64              Detail1
  );

/**
  Capture the image entrypoint immediately before the final instruction-cache
  synchronization performed by DxeCore.

  The function is allocation-free.  Entry bytes are read only after proving
  that the complete 8-byte capture lies inside the supplied image range.

  @param[in] ImageBase     Loaded image base.
  @param[in] ImageSize     Loaded image size in bytes.
  @param[in] EntryPoint    Image entrypoint.
  @param[in] ImageName     Optional bounded ASCII identity (normally PDB path).

**/
VOID
EFIAPI
MotoG20DxeTelemetryImageEntryPrepare (
  IN CONST VOID   *ImageBase,
  IN UINT64       ImageSize,
  IN CONST VOID   *EntryPoint,
  IN CONST CHAR8  *ImageName OPTIONAL
  );

/**
  Arm the current image-entry snapshot after DxeCore has completed the final
  instruction-cache synchronization and immediately before control transfer.

**/
VOID
EFIAPI
MotoG20DxeTelemetryImageEntryArmed (
  VOID
  );

VOID
EFIAPI
MotoG20DxeTelemetryReleaseDisplay (
  VOID
  );

VOID
EFIAPI
MotoG20DxeTelemetryException (
  IN EFI_EXCEPTION_TYPE  ExceptionType,
  IN EFI_SYSTEM_CONTEXT  SystemContext
  );

VOID
EFIAPI
MotoG20DxeTelemetryAssert (
  IN CONST CHAR8  *FileName OPTIONAL,
  IN UINTN        LineNumber,
  IN CONST CHAR8  *Description OPTIONAL
  );

VOID
EFIAPI
MotoG20DxeTelemetryFailAndReset (
  IN MOTO_G20_DXE_STAGE  Stage,
  IN EFI_STATUS          Status,
  IN UINT64              Detail0,
  IN UINT64              Detail1
  );

#endif
