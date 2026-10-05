/** Resident status only: installing this protocol does not certify USB host readiness. */
#ifndef MOTO_G20_USB_DIAGNOSTIC_H
#define MOTO_G20_USB_DIAGNOSTIC_H
#include <Uefi.h>
#include <Library/MotoG20UsbPlatformLib.h>
#define MOTO_G20_USB_DIAGNOSTIC_REVISION 6U
#define MOTO_G20_USB_PEER_DEADLINE_US 1100000U
#define MOTO_G20_USB_PEER_INITIAL_VALID BIT0
#define MOTO_G20_USB_PEER_100MS_VALID BIT1
#define MOTO_G20_USB_PEER_FIRST_HOST_VALID BIT2
#define MOTO_G20_USB_PEER_FIRST_SPEED_VALID BIT3
// Zero means no lifetime attempt limit. Every retry still requires a stable
// physical detach, and each connection attempt retains its own deadline.
#define MOTO_G20_USB_SESSION_ATTEMPT_LIMIT 0U
#define MOTO_G20_USB_RECOVERY_DETACH_MS 500U
#define MOTO_G20_USB_SESSION_DEVCTL_BEFORE_VALID BIT0
#define MOTO_G20_USB_SESSION_DEVCTL_START_VALID BIT1
#define MOTO_G20_USB_SESSION_DEVCTL_END_VALID BIT2
#define MOTO_G20_USB_SESSION_POWER_VALID BIT3
#define MOTO_G20_USB_SESSION_OTG_VALID BIT4
#define MOTO_G20_USB_SESSION_ALL_VALID 0x1FU
#define MOTO_G20_USB_DIAGNOSTIC_PROTOCOL_GUID \
  { 0x5da83f91, 0xb67e, 0x4e18, { 0xa3, 0xc7, 0x41, 0x09, 0x33, 0xb8, 0x51, 0xa2 } }
typedef enum {
  MotoUsbHcStagePrepare = 1, MotoUsbHcStageWaitRole, MotoUsbHcStageController,
  MotoUsbHcStageInstall, MotoUsbHcStageConnect, MotoUsbHcStageControlError,
  MotoUsbHcStageInterruptError, MotoUsbHcStageInterruptReport, MotoUsbHcStageStopped
} MOTO_G20_USB_HC_STAGE;

#define MOTO_G20_USB_EP0_CSR_VALID BIT0
#define MOTO_G20_USB_EP0_INTRTX_VALID BIT1
#define MOTO_G20_USB_EP0_DEVCTL_VALID BIT2
#define MOTO_G20_USB_EP0_CAUSE_NONE 0U
#define MOTO_G20_USB_EP0_CAUSE_ROLE_LOST 1U
#define MOTO_G20_USB_EP0_CAUSE_CSR_ERROR 2U
#define MOTO_G20_USB_EP0_CAUSE_HOST_VBUS_LOST 3U
#define MOTO_G20_USB_EP0_CAUSE_STALL 4U
#define MOTO_G20_USB_EP0_CAUSE_NAK 5U
#define MOTO_G20_USB_EP0_CAUSE_TIMEOUT 6U
// Last WaitEp0 call in the failed control operation. Polls counts its loop
// iterations; DelayUs counts only its explicit 10us delays, not elapsed wall
// time or electrical timing. Register values come from existing reads, before
// cleanup. ValidMask refers to one polling iteration. When HardwareAccessLost
// is TRUE, that iteration preceded the failed role check and is historical;
// no HC registers are accessed after the role check gates the hardware.
// Cause NONE means no WaitEp0 failure (validation or another control check).
typedef struct {
  BOOLEAN Valid;
  UINT8 Cause;
  BOOLEAN HardwareAccessLost;
  UINT8 ValidMask;
  UINT16 Csr0;
  UINT16 IntrTx;
  UINT8 DevCtl;
  UINT32 Polls;
  UINT32 DelayUs;
} MOTO_G20_USB_EP0_TRACE;

// Most recent failed operation in this driver lifetime. Successful traffic
// never erases it. This is cached software evidence, not a live register read.
typedef struct {
  BOOLEAN Valid;
  UINT8 Kind; // 1=control, 2=async registration, 3=interrupt RX, 4=cancel
  UINT8 Phase; // control: 0=validation/readiness, 1=setup, 2=data, 3=status
  UINT8 Address;
  UINT8 Endpoint;
  UINT8 Speed;
  UINT8 HubAddress;
  UINT8 HubPort;
  BOOLEAN RequestValid;
  UINT8 RequestType;
  UINT8 Request;
  UINT16 Value;
  UINT16 Index;
  UINT16 Length;
  EFI_STATUS Status;
  UINT32 Result;
  BOOLEAN ResultValid;
  MOTO_G20_USB_EP0_TRACE Ep0;
} MOTO_G20_USB_TRANSFER_FAILURE;
// Metadata for the last completed interrupt transfer. No report payload or
// keyboard bytes are retained. Length is the length delivered to the callback
// (zero for a rejected/failed packet). A valid callback status means it ran and
// returned for this exact observation, even if it cancelled its own pipe.
typedef struct {
  BOOLEAN Valid;
  UINT8 Address;
  UINT8 Endpoint;
  UINTN Length;
  UINT32 Result;
  BOOLEAN CallbackStatusValid;
  EFI_STATUS CallbackStatus;
} MOTO_G20_USB_INTERRUPT_OBSERVATION;
typedef struct {
  UINT32 Revision;
  UINT32 Size;
  MOTO_G20_USB_HC_STAGE Stage;
  EFI_STATUS Status;
  UINT32 ControllerStep; // 1=identity/config; 2=inherited DMA; 3=takeover; 4=FIFO; 5=host session
  UINT32 LastTransferResult;
  UINT64 ControlCompleted;
  UINT64 InterruptReports;
  BOOLEAN HostReady;
  BOOLEAN HcPublished;
  BOOLEAN PortConnected;
  BOOLEAN PortEnabled;
  UINT32 PlatformStage;
  EFI_STATUS PlatformStatus;
  UINT16 TypecState;
  BOOLEAN VbusPresent;
  MOTO_G20_USB_ROLE_DIAGNOSTIC Role;
  // V3 session samples are historical, captured while HC clocks were accessible
  // and before rollback. GetStatus never reads HC registers. BEFORE is before
  // SESSION, START is after SESSION then HOST_FORCE, END is at qualification or
  // timeout. POWER/OTG accompany END. A bit in ValidMask qualifies each field.
  // New powered platform bring-ups reaching SESSION; normal USB HC Reset
  // while power remains on does not increment this count. Counters saturate;
  // SessionLimit zero means unlimited, never immediate exhaustion.
  UINT32 SessionAttempts;
  UINT32 SessionLimit;
  UINT32 SessionSnapshotValidMask;
  UINT32 SessionWaitUs;
  UINT8 SessionDevCtlBefore;
  UINT8 SessionDevCtlStart;
  UINT8 SessionDevCtlEnd;
  UINT8 SessionPower;
  UINT8 SessionOtg;
  EFI_STATUS SessionStatus;
  EFI_STATUS CleanupStatus;
  EFI_STATUS RecoveryProbeStatus;
  BOOLEAN RecoveryWaiting;
  BOOLEAN RecoveryExhausted; // Reserved for ABI continuity; always FALSE in V5.
  BOOLEAN FatalLatched;
  // Recovery fields describe the latest fresh detector read while power is OFF.
  // They are invalid after a failed read or after leaving WAIT_STABLE_DETACH.
  BOOLEAN RecoverySampleValid;
  UINT16 RecoveryTypecState;
  BOOLEAN RecoveryVbus;
  UINT32 DetachStableMs;
  UINT32 Rearms;
  // V4: bounded connection wait. Samples are cached, never read by GetStatus.
  // FIRST times are observed poll times, not measured electrical edge times.
  BOOLEAN SessionPending;
  UINT32 SessionDeadlineUs;
  UINT32 PeerSnapshotValidMask;
  UINT8 SessionPowerInitial;
  UINT8 SessionTestModeInitial;
  UINT8 PeerDevCtl100Ms;
  UINT8 PeerPower100Ms;
  UINT8 PeerOtg100Ms;
  UINT32 FirstHostUs;
  UINT32 FirstSpeedUs;
  UINT64 ControlCalls;
  UINT64 ControlFailures;
  UINT64 AsyncFailures;
  UINT64 InterruptFailures;
  MOTO_G20_USB_TRANSFER_FAILURE LastFailure;
  UINT64 ZeroLengthInterruptReports;
  MOTO_G20_USB_INTERRUPT_OBSERVATION LastInterrupt;
  // Historical sample at the end of the last completed root-port reset.
  // Invalidated when the next actual root reset starts. Not current bus state.
  BOOLEAN RootResetValid;
  UINT8 RootResetDevCtl;
  UINT8 RootResetPower;
} MOTO_G20_USB_DIAGNOSTIC_STATUS;
typedef struct _MOTO_G20_USB_DIAGNOSTIC_PROTOCOL MOTO_G20_USB_DIAGNOSTIC_PROTOCOL;
/* Call at TPL <= TPL_NOTIFY. GetStatus copies all caches under TPL_NOTIFY.
   A retrieval error must not be interpreted
   as USB success; consumers must validate protocol/status Revision and Size. */
typedef EFI_STATUS (EFIAPI *MOTO_G20_USB_GET_DIAGNOSTIC)(
  IN MOTO_G20_USB_DIAGNOSTIC_PROTOCOL *This,
  OUT MOTO_G20_USB_DIAGNOSTIC_STATUS *Status);
struct _MOTO_G20_USB_DIAGNOSTIC_PROTOCOL {
  UINT64 Revision;
  MOTO_G20_USB_GET_DIAGNOSTIC GetStatus;
};
#endif
