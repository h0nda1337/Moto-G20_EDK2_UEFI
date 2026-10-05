/** Moto G20 board USB host power and PHY contract. Host bring-up is experimental. */
#ifndef MOTO_G20_USB_PLATFORM_LIB_H_
#define MOTO_G20_USB_PLATFORM_LIB_H_
#include <Uefi.h>

typedef enum {
  MotoUsbStageNone, MotoUsbStageMapping, MotoUsbStageAdi,
  MotoUsbStageEfuse, MotoUsbStageTypec, MotoUsbStageExternalVbus,
  MotoUsbStageI2c, MotoUsbStageVdd, MotoUsbStagePhy,
  MotoUsbStageBoost, MotoUsbStageVbusSense, MotoUsbStageRunning,
  MotoUsbStageRollback, MotoUsbStageStopped
} MOTO_G20_USB_PLATFORM_STAGE;

#define MOTO_G20_USB_ROLE_DIAGNOSTIC_REVISION 2U
#define MOTO_G20_USB_ROLE_EN_VALID           BIT0
#define MOTO_G20_USB_ROLE_MODE_VALID         BIT1
#define MOTO_G20_USB_ROLE_INT_EN_VALID       BIT2
#define MOTO_G20_USB_ROLE_INT_RAW_VALID      BIT3
#define MOTO_G20_USB_ROLE_INT_MASK_VALID     BIT4
#define MOTO_G20_USB_ROLE_STATUS_VALID       BIT5
#define MOTO_G20_USB_ROLE_TRIM_VALID         BIT6
#define MOTO_G20_USB_ROLE_ALL_VALID          0x7FU

/* Cached diagnostic snapshot only; getters never access MMIO.
   Samples counts ReadRole attempts. SeenStates/SourceSeen use only successful
   TypeC STATUS reads inside ReadRole (excluding the extra snapshot read).
   Snapshot masks describe the latest complete read sweep,
   not continuous measurements; failed fields are invalid even when zero.
   SnapshotStatus is the first ADI error from that sweep. CalibrationStatus,
   EfuseValid and TrimApplied must qualify their corresponding values.
   TrimApplied records that the last calibration write/readback succeeded;
   a later Stop may restore the original trim. Register snapshots are historical.
   DriverActive/BoostRequested are software ownership, never measured voltage.
   BoostRequested remains TRUE after an uncertain OFF. StopAttempts counts
   DisablePower entries; LastStopStatus includes a full Stop's final restore. */
typedef struct {
  UINT32 Revision;
  UINT32 Size;
  UINT64 Samples;
  UINT64 SourceSeen;
  UINT64 BoostAttempts;
  UINT64 SnapshotAttempts;
  UINT64 SnapshotFailures;
  UINT32 SeenStates;
  UINT32 ValidMask;
  UINT32 ErrorMask;
  EFI_STATUS RoleReadStatus;
  EFI_STATUS SnapshotStatus;
  EFI_STATUS CalibrationStatus;
  UINT16 En;
  UINT16 Mode;
  UINT16 IntEn;
  UINT16 IntRaw;
  UINT16 IntMask;
  UINT16 TypecStatus;
  UINT16 Trim;
  UINT16 EfuseData;
  UINT16 RequestedTrim;
  BOOLEAN EfuseValid;
  BOOLEAN TrimApplied;
  BOOLEAN DriverActive;
  BOOLEAN BoostRequested;
  EFI_STATUS LastStopStatus;
  UINT64 StopAttempts;
} MOTO_G20_USB_ROLE_DIAGNOSTIC;

/* Call at TPL_APPLICATION before any host controller register access. */
EFI_STATUS EFIAPI MotoG20UsbPlatformPrepare (VOID);
/* Init may be retried on EFI_NOT_READY when the user changes PC cable to OTG.
   SUCCESS means calibrated ATTACHED_SRC, EIC VBUS present, PHY configured.
   It does not mean MUSB HOSTMODE, enumeration or keyboard input works. */
EFI_STATUS EFIAPI MotoG20UsbPlatformInit (OUT UINTN *ControllerBase);
/* Poll before HC accesses. No HC MMIO after FALSE or an error. */
EFI_STATUS EFIAPI MotoG20UsbPlatformPollRole (OUT BOOLEAN *HostActive);
/* Stop the HC first. Remove source power/PHY, retaining the TypeC detector.
   Never enables power; any rollback fault stays latched against rearming. */
EFI_STATUS EFIAPI MotoG20UsbPlatformQuiesceForRetry (VOID);
/* Fresh TypeC/EIC reads only, while inactive with a prepared detector and no
   fault/pending boost. Does not energize or access HC. On failure OUT values
   are untouched. Getter APIs remain cache-only. */
EFI_STATUS EFIAPI MotoG20UsbPlatformDetectRole (
  OUT UINT16 *TypecState, OUT BOOLEAN *VbusPresent
  );
/* Stop the HC before Stop. Idempotent; failed rollback latches a fault. */
EFI_STATUS EFIAPI MotoG20UsbPlatformStop (VOID);
/* Final unload only: Stop plus CloseEvent. Propagate error and refuse unload. */
EFI_STATUS EFIAPI MotoG20UsbPlatformShutdown (VOID);
VOID EFIAPI MotoG20UsbPlatformGetDiagnostic (
  OUT MOTO_G20_USB_PLATFORM_STAGE *Stage, OUT EFI_STATUS *Status,
  OUT UINT16 *TypecState, OUT BOOLEAN *VbusPresent
  );
EFI_STATUS EFIAPI MotoG20UsbPlatformGetRoleDiagnostic (
  OUT MOTO_G20_USB_ROLE_DIAGNOSTIC *Role
  );
#endif
