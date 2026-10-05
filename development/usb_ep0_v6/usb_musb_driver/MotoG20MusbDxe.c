/** @file
  Non-PCI MUSB USB2 host: one root port, control EP0 and four independent
  periodic interrupt-IN pipes. PIO only, with single-TT hub routing as used
  by UsbBusDxe's default hub interface setting. Bulk, isochronous, interrupt
  OUT and interrupt packets above 64 bytes are unsupported.

  Register semantics checked against Motorola kernel-sprd commit
  261390cdfff66efef9082bd70c1af0abd21d77f2 (musb_regs.h, musb_host.c).
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Protocol/Usb2HostController.h>
#include <Protocol/DevicePath.h>
#include <Guid/EventGroup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/MotoG20UsbPlatformLib.h>
#include "MotoG20MusbRegs.h"
#include "MotoG20UsbDiagnostic.h"

#define INTERRUPT_PIPE_COUNT 4
#define INTERRUPT_FIFO_SIZE 64
#define ROLE_POLL_TICKS 100
#define CONTROL_MAX_MS 5000
#define SESSION_WAIT_MS 100U

typedef struct {
  BOOLEAN Active;
  BOOLEAN Pending;
  UINT64 Generation;
  UINT8 Address;
  UINT8 Endpoint;
  UINT8 Speed;
  UINT8 Toggle;
  UINT8 HubAddress;
  UINT8 HubPort;
  UINTN MaxPacket;
  UINTN Length;
  UINTN Interval;
  UINTN Elapsed;
  EFI_ASYNC_USB_TRANSFER_CALLBACK Callback;
  VOID *CallbackContext;
  UINT8 Buffer[INTERRUPT_FIFO_SIZE];
} MUSB_INTERRUPT_PIPE;

typedef struct {
  EFI_USB2_HC_PROTOCOL Hc;
  EFI_HANDLE Handle;
  EFI_EVENT Timer;
  EFI_EVENT ExitEvent;
  UINTN Base;
  EFI_USB_HC_STATE State;
  BOOLEAN Ready;
  BOOLEAN Published;
  BOOLEAN Busy;
  BOOLEAN Ended;
  BOOLEAN Fatal;
  BOOLEAN ConnectPending;
  BOOLEAN PowerOffRequested;
  BOOLEAN Connected;
  BOOLEAN Enabled;
  BOOLEAN Suspended;
  BOOLEAN Resetting;
  BOOLEAN WaitingDetach;
  BOOLEAN DetachTracking;
  BOOLEAN DetachedStateObserved;
  UINT64 DetachSince;
  BOOLEAN SessionPending;
  BOOLEAN SessionAccessLost;
  UINT64 SessionSince;
  UINT64 SessionLast;
  UINT64 InterruptObservationGeneration;
  UINT16 Changes;
  UINTN RoleTicks;
  MUSB_INTERRUPT_PIPE Pipes[INTERRUPT_PIPE_COUNT];
} MUSB_HOST;

STATIC MUSB_HOST mUsb;
STATIC EFI_HANDLE mDiagnosticHandle;
STATIC EFI_GUID mDiagnosticGuid = MOTO_G20_USB_DIAGNOSTIC_PROTOCOL_GUID;
STATIC MOTO_G20_USB_DIAGNOSTIC_STATUS mDiagnostic;
STATIC EFI_STATUS EFIAPI ReadDiagnostic (MOTO_G20_USB_DIAGNOSTIC_PROTOCOL *This, MOTO_G20_USB_DIAGNOSTIC_STATUS *Status);
STATIC MOTO_G20_USB_DIAGNOSTIC_PROTOCOL mDiagnosticProtocol = { MOTO_G20_USB_DIAGNOSTIC_REVISION, ReadDiagnostic };

STATIC VOID Record (MOTO_G20_USB_HC_STAGE Stage, EFI_STATUS Status) {
  mDiagnostic.Stage = Stage;
  mDiagnostic.Status = Status;
}
STATIC VOID Increment32 (UINT32 *Counter) {
  if (*Counter != 0xFFFFFFFFU) { ++*Counter; }
}
STATIC VOID Increment64 (UINT64 *Counter) {
  if (*Counter != 0xFFFFFFFFFFFFFFFFULL) { ++*Counter; }
}
STATIC EFI_STATUS EFIAPI ReadDiagnostic (MOTO_G20_USB_DIAGNOSTIC_PROTOCOL *This, MOTO_G20_USB_DIAGNOSTIC_STATUS *Status) {
  MOTO_G20_USB_PLATFORM_STAGE PlatformStage;
  EFI_STATUS RoleStatus;
  EFI_TPL OldTpl;
  if ((This != &mDiagnosticProtocol) || (Status == NULL)) { return EFI_INVALID_PARAMETER; }
  // Keep HC and both platform caches coherent against role/timer notifications.
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  CopyMem (Status, &mDiagnostic, sizeof (*Status));
  Status->Revision = MOTO_G20_USB_DIAGNOSTIC_REVISION;
  Status->Size = sizeof (*Status);
  Status->HostReady = mUsb.Ready;
  Status->HcPublished = mUsb.Published;
  Status->PortConnected = mUsb.Connected;
  Status->PortEnabled = mUsb.Enabled;
  Status->SessionLimit = MOTO_G20_USB_SESSION_ATTEMPT_LIMIT;
  Status->RecoveryWaiting = mUsb.WaitingDetach;
  Status->FatalLatched = mUsb.Fatal;
  Status->SessionPending = mUsb.SessionPending;
  Status->SessionDeadlineUs = MOTO_G20_USB_PEER_DEADLINE_US;
  MotoG20UsbPlatformGetDiagnostic (&PlatformStage, &Status->PlatformStatus, &Status->TypecState, &Status->VbusPresent);
  Status->PlatformStage = (UINT32)PlatformStage;
  RoleStatus = MotoG20UsbPlatformGetRoleDiagnostic (&Status->Role);
  if (!EFI_ERROR (RoleStatus) &&
      ((Status->Role.Revision != MOTO_G20_USB_ROLE_DIAGNOSTIC_REVISION) ||
       (Status->Role.Size != sizeof (Status->Role)))) {
    RoleStatus = EFI_INCOMPATIBLE_VERSION;
  }
  if (EFI_ERROR (RoleStatus)) {
    // Do not leave partially returned fields marked valid after a getter failure.
    ZeroMem (&Status->Role, sizeof (Status->Role));
    Status->Role.Revision = MOTO_G20_USB_ROLE_DIAGNOSTIC_REVISION;
    Status->Role.Size = sizeof (Status->Role);
    Status->Role.RoleReadStatus = RoleStatus;
    Status->Role.SnapshotStatus = RoleStatus;
    Status->Role.CalibrationStatus = RoleStatus;
  }
  gBS->RestoreTPL (OldTpl);
  return RoleStatus;
}
typedef struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} MUSB_DEVICE_PATH;

STATIC MUSB_DEVICE_PATH mPath = {
  { { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0xd72b139a, 0x50b1, 0x45cf, { 0x94, 0xb9, 0x7e, 0x6e, 0x5c, 0xa1, 0xb2, 0xd3 } } },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC UINT8 R8 (UINTN Offset) { return MmioRead8 (mUsb.Base + Offset); }
STATIC UINT16 R16 (UINTN Offset) { return MmioRead16 (mUsb.Base + Offset); }
STATIC VOID W8 (UINTN Offset, UINT8 Value) { MmioWrite8 (mUsb.Base + Offset, Value); }
STATIC VOID W16 (UINTN Offset, UINT16 Value) { MmioWrite16 (mUsb.Base + Offset, Value); }
STATIC VOID Select (UINT8 Ep) { W8 (MU_INDEX, Ep); }

STATIC VOID FifoWrite (UINT8 Ep, CONST UINT8 *Data, UINTN Length) {
  UINTN Index;
  for (Index = 0; Index < Length; ++Index) { W8 (MU_FIFO (Ep), Data[Index]); }
}
STATIC VOID FifoRead (UINT8 Ep, UINT8 *Data, UINTN Length) {
  UINTN Index;
  for (Index = 0; Index < Length; ++Index) { Data[Index] = R8 (MU_FIFO (Ep)); }
}

STATIC UINT8 TypeSpeed (UINT8 Speed) {
  return (Speed == EFI_USB_SPEED_HIGH) ? 0x40 : ((Speed == EFI_USB_SPEED_LOW) ? 0xC0 : 0x80);
}

STATIC EFI_STATUS GetRoute (
  UINT8 Speed, EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator,
  UINT8 *HubAddress, UINT8 *HubPort)
{
  *HubAddress = 0;
  *HubPort = 0;
  // UsbBusDxe can supply {0, 1} for a root-connected full/low-speed device.
  // A high-speed endpoint needs no split transaction, irrespective of TT.
  if ((Speed == EFI_USB_SPEED_HIGH) || (Translator == NULL) ||
      (Translator->TranslatorHubAddress == 0)) { return EFI_SUCCESS; }
  if ((Translator->TranslatorHubAddress > 127) ||
      (Translator->TranslatorPortNumber == 0) ||
      (Translator->TranslatorPortNumber > 127)) { return EFI_INVALID_PARAMETER; }
  // MUSB HUBADDR bit 7 means multi-TT, NOT "enable split". UEFI's translator
  // has no multi-TT flag. The paired UsbBusDxe keeps hubs at default alt 0,
  // single-TT, so use the seven-bit hub address without inventing that flag.
  *HubAddress = Translator->TranslatorHubAddress;
  *HubPort = Translator->TranslatorPortNumber;
  return EFI_SUCCESS;
}

STATIC BOOLEAN ValidThis (EFI_USB2_HC_PROTOCOL *This) { return This == &mUsb.Hc; }

STATIC VOID ForgetPipe (MUSB_INTERRUPT_PIPE *Pipe) {
  ++Pipe->Generation;
  Pipe->Active = FALSE;
  Pipe->Pending = FALSE;
  Pipe->Callback = NULL;
  Pipe->CallbackContext = NULL;
}

STATIC VOID ForgetAllPipes (VOID) {
  UINTN Index;
  for (Index = 0; Index < INTERRUPT_PIPE_COUNT; ++Index) { ForgetPipe (&mUsb.Pipes[Index]); }
}

STATIC VOID StopPipeHardware (UINTN Index) {
  if (mUsb.Ready) {
    Select ((UINT8)(Index + 1));
    W16 (MU_RXCSR, 0);
    W16 (MU_RXCSR, MU_RX_FLUSH | MU_RX_CLRTOG);
    W16 (MU_RXCSR, MU_RX_FLUSH | MU_RX_CLRTOG);
    W16 (MU_RXCSR, 0);
    (VOID)R16 (MU_INTRRX);
  }
  mUsb.Pipes[Index].Pending = FALSE;
}

STATIC VOID StopAllPipeHardware (VOID) {
  UINTN Index;
  for (Index = 0; Index < INTERRUPT_PIPE_COUNT; ++Index) { StopPipeHardware (Index); }
}

// Caller has just verified platform access and holds TPL_NOTIFY. Disable the
// host session before the platform restores its PHY and source-power state.
STATIC VOID StopControllerHardware (VOID) {
  StopAllPipeHardware ();
  Select (0);
  W16 (MU_CSR0, 0);
  W16 (MU_CSR0, MU_C0_FLUSH);
  W16 (MU_CSR0, 0);
  W8 (MU_INTRUSBE, 0);
  W16 (MU_INTRTXE, 0);
  W16 (MU_INTRRXE, 0);
  W8 (MU_DEVCTL, (UINT8)(R8 (MU_DEVCTL) & ~MU_DEV_SESSION));
  W8 (MU_OTG_EXT_CSR, (UINT8)(R8 (MU_OTG_EXT_CSR) & ~MU_HOST_FORCE));
}

// PollRole can turn off the platform clocks. No MMIO follows a negative result.
STATIC EFI_STATUS CheckRole (VOID) {
  BOOLEAN HostActive;
  EFI_STATUS Status;
  if (!mUsb.Ready || mUsb.Ended) { return EFI_NOT_READY; }
  HostActive = FALSE;
  Status = MotoG20UsbPlatformPollRole (&HostActive);
  if (EFI_ERROR (Status) || !HostActive) {
    mUsb.Ready = FALSE;
    mUsb.State = EfiUsbHcStateHalt;
    if (mUsb.Connected) { mUsb.Changes |= USB_PORT_STAT_C_CONNECTION; }
    if (mUsb.Enabled) { mUsb.Changes |= USB_PORT_STAT_C_ENABLE; }
    mUsb.Connected = FALSE;
    mUsb.Enabled = FALSE;
    ForgetAllPipes ();
    return EFI_NOT_READY;
  }
  return EFI_SUCCESS;
}

// Public MMIO methods hold TPL_NOTIFY across the role check and final access.
// The independent platform monitor can therefore never gate our clocks in
// between. Control polling explicitly rechecks the role during long waits.
STATIC EFI_STATUS BeginIo (EFI_TPL *OldTpl) {
  EFI_STATUS Status;
  *OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mUsb.Busy) { gBS->RestoreTPL (*OldTpl); return EFI_NOT_READY; }
  mUsb.Busy = TRUE;
  Status = CheckRole ();
  if (EFI_ERROR (Status)) { mUsb.Busy = FALSE; gBS->RestoreTPL (*OldTpl); }
  return Status;
}
STATIC VOID EndIo (EFI_TPL OldTpl) {
  mUsb.Busy = FALSE;
  gBS->RestoreTPL (OldTpl);
}

STATIC VOID RefreshPort (VOID) {
  UINT8 Interrupt;
  UINT8 Dev;
  BOOLEAN Connected;
  Interrupt = R8 (MU_INTRUSB);
  Dev = R8 (MU_DEVCTL);
  Connected = mUsb.Connected || ((Dev & (MU_DEV_FS | MU_DEV_LS)) != 0);
  if ((Interrupt & MU_INTR_CONNECT) != 0) { Connected = TRUE; }
  if ((Interrupt & MU_INTR_DISCONNECT) != 0) { Connected = FALSE; }
  if ((Interrupt & (MU_INTR_VBUSERR | MU_INTR_BABBLE)) != 0) {
    Connected = FALSE;
    mUsb.Enabled = FALSE;
    mUsb.Changes |= USB_PORT_STAT_C_ENABLE;
    if ((Interrupt & MU_INTR_VBUSERR) != 0) { mUsb.Changes |= USB_PORT_STAT_C_OVERCURRENT; }
  }
  if (Connected != mUsb.Connected) {
    mUsb.Connected = Connected;
    mUsb.Changes |= USB_PORT_STAT_C_CONNECTION;
    if (!Connected) {
      if (mUsb.Enabled) { mUsb.Changes |= USB_PORT_STAT_C_ENABLE; }
      mUsb.Enabled = FALSE;
      StopAllPipeHardware ();
      ForgetAllPipes ();
    }
  }
}

STATIC VOID CaptureSessionEnd (VOID) {
  // Do not read INTRUSB here: it is clear-on-read.
  mDiagnostic.SessionDevCtlEnd = R8 (MU_DEVCTL);
  mDiagnostic.SessionPower = R8 (MU_POWER);
  mDiagnostic.SessionOtg = R8 (MU_OTG_EXT_CSR);
  mDiagnostic.SessionSnapshotValidMask |= MOTO_G20_USB_SESSION_DEVCTL_END_VALID |
    MOTO_G20_USB_SESSION_POWER_VALID | MOTO_G20_USB_SESSION_OTG_VALID;
}

STATIC EFI_STATUS SessionElapsed (VOID) {
  UINT64 Now, Us;
  Now = GetPerformanceCounter ();
  if (Now < mUsb.SessionLast) { return EFI_DEVICE_ERROR; }
  mUsb.SessionLast = Now;
  Us = GetTimeInNanoSecond (Now - mUsb.SessionSince) / 1000ULL;
  mDiagnostic.SessionWaitUs = (Us > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (UINT32)Us;
  return EFI_SUCCESS;
}

STATIC VOID ObserveSessionBits (VOID) {
  if ((mDiagnostic.SessionDevCtlEnd & MU_DEV_HOST) &&
      !(mDiagnostic.PeerSnapshotValidMask & MOTO_G20_USB_PEER_FIRST_HOST_VALID)) {
    mDiagnostic.FirstHostUs = mDiagnostic.SessionWaitUs;
    mDiagnostic.PeerSnapshotValidMask |= MOTO_G20_USB_PEER_FIRST_HOST_VALID;
  }
  if ((mDiagnostic.SessionDevCtlEnd & (MU_DEV_FS | MU_DEV_LS)) &&
      !(mDiagnostic.PeerSnapshotValidMask & MOTO_G20_USB_PEER_FIRST_SPEED_VALID)) {
    mDiagnostic.FirstSpeedUs = mDiagnostic.SessionWaitUs;
    mDiagnostic.PeerSnapshotValidMask |= MOTO_G20_USB_PEER_FIRST_SPEED_VALID;
  }
}

STATIC BOOLEAN SessionQualified (VOID) {
  return (mDiagnostic.SessionDevCtlEnd & (MU_DEV_SESSION | MU_DEV_BDEVICE | MU_DEV_HOST | MU_DEV_VBUS)) ==
    (MU_DEV_SESSION | MU_DEV_HOST | MU_DEV_VBUS);
}

STATIC BOOLEAN SessionCanWait (VOID) {
  return ((mDiagnostic.SessionDevCtlEnd & (MU_DEV_SESSION | MU_DEV_BDEVICE | MU_DEV_VBUS)) ==
          (MU_DEV_SESSION | MU_DEV_VBUS)) && ((mDiagnostic.SessionOtg & MU_HOST_FORCE) != 0);
}

STATIC VOID SessionReady (VOID) {
  mUsb.SessionPending = FALSE;
  mDiagnostic.SessionStatus = EFI_SUCCESS;
  mUsb.Ready = TRUE;
  mUsb.State = EfiUsbHcStateOperational;
  mUsb.Enabled = FALSE;
  mUsb.Suspended = FALSE;
  mUsb.Resetting = FALSE;
  RefreshPort ();
}

STATIC EFI_STATUS ConfigureController (BOOLEAN CountBringUp) {
  UINT8 Config;
  UINT8 Dev;
  UINT16 Version;
  UINTN Wait;
  UINTN Channel;
  UINT16 Csr;
  EFI_STATUS Status;
  BOOLEAN Active;
  mDiagnostic.SessionSnapshotValidMask = 0;
  mDiagnostic.SessionStatus = EFI_NOT_READY;
  mDiagnostic.SessionWaitUs = 0;
  mDiagnostic.PeerSnapshotValidMask = 0;
  mDiagnostic.FirstHostUs = mDiagnostic.FirstSpeedUs = 0;
  mUsb.SessionPending = mUsb.SessionAccessLost = FALSE;
  Version = R16 (MU_HWVERS);
  mDiagnostic.ControllerStep = 1;
  Select (0);
  Config = R8 (MU_CONFIGDATA);
  if ((Version == 0) || (Version == 0xFFFF) ||
      ((Config & MU_CFG_DYNFIFO) == 0) || ((Config & MU_CFG_BIGENDIAN) != 0)) {
    return EFI_UNSUPPORTED;
  }
  // sprd_musbhsdma.h exposes 30 channels. A live inherited DMA queue is not
  // safe to take over without its ownership metadata. Refuse it explicitly.
  mDiagnostic.ControllerStep = 2;
  if (MmioRead32 (mUsb.Base + MU_DMA_EN_STATUS) != 0) { return EFI_ACCESS_DENIED; }
  for (Channel = 1; Channel <= 30; ++Channel) {
    if ((MmioRead32 (mUsb.Base + MU_DMA_CH_CFG (Channel)) & 1) != 0) { return EFI_ACCESS_DENIED; }
  }
  // Stopping a session does not reset endpoint CSR state. Do not reassign
  // FIFO RAM while an unused endpoint still owns a pending request or data.
  // Motorola musb_host.c: musb_h_tx_flush_fifo / musb_h_flush_rxfifo;
  // musb_sprd.c: sprd_musb_reset_context separately clears endpoint state.
  mDiagnostic.ControllerStep = 3;
  W8 (MU_DEVCTL, (UINT8)(R8 (MU_DEVCTL) & ~MU_DEV_SESSION));
  W8 (MU_OTG_EXT_CSR, (UINT8)(R8 (MU_OTG_EXT_CSR) & ~MU_HOST_FORCE));
  if (((R8 (MU_DEVCTL) & MU_DEV_SESSION) != 0) ||
      ((R8 (MU_OTG_EXT_CSR) & MU_HOST_FORCE) != 0)) { return EFI_DEVICE_ERROR; }
  for (Channel = 1; Channel <= 15; ++Channel) {
    Select ((UINT8)Channel);
    if ((R16 (MU_TXCSR) & (MU_TX_READY | MU_TX_FIFO_NOT_EMPTY | MU_TX_AUTOSET)) != 0) {
      return EFI_ACCESS_DENIED;
    }
    if ((Channel > INTERRUPT_PIPE_COUNT) &&
        ((R16 (MU_RXCSR) & (MU_RX_READY | MU_RX_REQ | MU_RX_AUTOREQ)) != 0)) {
      return EFI_ACCESS_DENIED;
    }
  }
  for (Channel = 1; Channel <= 30; ++Channel) {
    MmioWrite32 (mUsb.Base + MU_DMA_CH_INTR (Channel), 0);
  }
  // DMA enable must be cleared before the TX DMA mode bit (vendor driver).
  for (Channel = 1; Channel <= 15; ++Channel) {
    Select ((UINT8)Channel);
    Csr = R16 (MU_TXCSR);
    W16 (MU_TXCSR, (UINT16)(Csr & ~MU_TX_DMA_ENABLE));
    W16 (MU_TXCSR, (UINT16)(Csr & ~(MU_TX_DMA_ENABLE | MU_TX_DMA_MODE)));
    Csr = R16 (MU_RXCSR);
    W16 (MU_RXCSR, (UINT16)(Csr & ~(MU_RX_DMA_ENABLE | MU_RX_DMA_MODE)));
  }
  W8 (MU_INTRUSBE, 0);
  W16 (MU_INTRTXE, 0);
  W16 (MU_INTRRXE, 0);
  (VOID)R8 (MU_INTRUSB);
  (VOID)R16 (MU_INTRTX);
  (VOID)R16 (MU_INTRRX);
  // EP0 uses bytes 0..63. RX EP1..EP4 each own the next 64 bytes,
  // single buffered. Hub status and keyboard reports must not share a FIFO.
  mDiagnostic.ControllerStep = 4;
  Select (0);
  W8 (MU_TXFIFOSZ, 3);
  W8 (MU_RXFIFOSZ, 3);
  W16 (MU_TXFIFOADD, 0);
  W16 (MU_RXFIFOADD, 0);
  W16 (MU_CSR0, MU_C0_FLUSH);
  W16 (MU_CSR0, 0);
  for (Channel = 1; Channel <= INTERRUPT_PIPE_COUNT; ++Channel) {
    Select ((UINT8)Channel);
    W16 (MU_RXCSR, 0);
    W16 (MU_RXCSR, MU_RX_FLUSH | MU_RX_CLRTOG);
    W16 (MU_RXCSR, MU_RX_FLUSH | MU_RX_CLRTOG);
    W16 (MU_RXCSR, 0);
    W16 (MU_TXCSR, 0);
    W8 (MU_RXFIFOSZ, 3);
    W16 (MU_RXFIFOADD, (UINT16)(Channel * INTERRUPT_FIFO_SIZE / 8));
    if ((R8 (MU_RXFIFOSZ) != 3) ||
        (R16 (MU_RXFIFOADD) != Channel * INTERRUPT_FIFO_SIZE / 8)) { return EFI_DEVICE_ERROR; }
  }
  W8 (MU_POWER, MU_POWER_HSEN);
  mDiagnostic.SessionPowerInitial = R8 (MU_POWER);
  mDiagnostic.SessionTestModeInitial = R8 (MU_TESTMODE);
  mDiagnostic.PeerSnapshotValidMask |= MOTO_G20_USB_PEER_INITIAL_VALID;
  mDiagnostic.ControllerStep = 5;
  // UsbBusDxe can reset an already powered controller during normal binding.
  // Only a new platform bring-up increments the saturating attempt counter.
  if (CountBringUp) { Increment32 (&mDiagnostic.SessionAttempts); }
  mDiagnostic.SessionDevCtlBefore = R8 (MU_DEVCTL);
  mDiagnostic.SessionSnapshotValidMask |= MOTO_G20_USB_SESSION_DEVCTL_BEFORE_VALID;
  // Match sprd_musb_enable(): SESSION first, then HOST_FORCE. This aligns the
  // vendor sequence; it is not evidence that ordering caused the V2 timeout.
  mUsb.SessionSince = mUsb.SessionLast = GetPerformanceCounter ();
  W8 (MU_DEVCTL, (UINT8)(mDiagnostic.SessionDevCtlBefore | MU_DEV_SESSION));
  W8 (MU_OTG_EXT_CSR, (UINT8)(R8 (MU_OTG_EXT_CSR) | MU_HOST_FORCE));
  mDiagnostic.SessionDevCtlStart = R8 (MU_DEVCTL);
  mDiagnostic.SessionSnapshotValidMask |= MOTO_G20_USB_SESSION_DEVCTL_START_VALID;
  for (Wait = 0; Wait < SESSION_WAIT_MS; ++Wait) {
    Dev = R8 (MU_DEVCTL);
    if ((Dev & (MU_DEV_HOST | MU_DEV_VBUS)) == (MU_DEV_HOST | MU_DEV_VBUS)) { break; }
    MicroSecondDelay (1000);
  }
  Status = SessionElapsed ();
  CaptureSessionEnd ();
  if (EFI_ERROR (Status)) { return Status; }
  ObserveSessionBits ();
  if (Wait == SESSION_WAIT_MS) {
    mDiagnostic.PeerDevCtl100Ms = mDiagnostic.SessionDevCtlEnd;
    mDiagnostic.PeerPower100Ms = mDiagnostic.SessionPower;
    mDiagnostic.PeerOtg100Ms = mDiagnostic.SessionOtg;
    mDiagnostic.PeerSnapshotValidMask |= MOTO_G20_USB_PEER_100MS_VALID;
  }
  // Fresh TypeC=AttachedSRC and EIC=1, with no HC MMIO after a negative poll.
  // Caller holds TPL_NOTIFY, so the independent monitor cannot gate in between.
  Active = FALSE;
  Status = MotoG20UsbPlatformPollRole (&Active);
  if (EFI_ERROR (Status) || !Active) {
    mUsb.SessionAccessLost = TRUE;
    return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
  }
  if (!SessionQualified ()) {
    // Motorola distinguishes A_WAIT_VRISE (100ms) from A_WAIT_BCON (1100ms).
    // Keep protocol/traffic gated on HM; permit only a powered new bring-up
    // to wait asynchronously. UEFI Reset retains its synchronous contract.
    if (CountBringUp && SessionCanWait () &&
        (mDiagnostic.SessionWaitUs < MOTO_G20_USB_PEER_DEADLINE_US)) {
      mUsb.SessionPending = TRUE;
      mUsb.Ready = FALSE;
      mUsb.State = EfiUsbHcStateHalt;
    }
    return EFI_NOT_READY;
  }
  SessionReady ();
  return EFI_SUCCESS;
}

STATIC VOID ResetDetachDebounce (VOID) {
  mUsb.DetachTracking = FALSE;
  mUsb.DetachedStateObserved = FALSE;
  mUsb.DetachSince = 0;
  mDiagnostic.DetachStableMs = 0;
}

// Only a qualified step5 timeout may wait for a physically new connection.
// Identity/DMA/FIFO/transport/rollback errors remain fatal for this lifetime.
// Caller owns TPL_NOTIFY. HardwareAccessible is FALSE after a negative role
// poll because that poll (or the platform monitor) may already have gated HC.
STATIC VOID ControllerFailed (EFI_STATUS Status, BOOLEAN HardwareAccessible) {
  BOOLEAN Retryable;
  EFI_STATUS Cleanup;
  Retryable = (mDiagnostic.ControllerStep == 5) && (Status == EFI_NOT_READY);
  if (HardwareAccessible && (mDiagnostic.ControllerStep >= 4)) { StopControllerHardware (); }
  mUsb.SessionPending = FALSE;
  mDiagnostic.SessionStatus = Status;
  if (mUsb.Connected) { mUsb.Changes |= USB_PORT_STAT_C_CONNECTION; }
  if (mUsb.Enabled) { mUsb.Changes |= USB_PORT_STAT_C_ENABLE; }
  mUsb.Ready = FALSE;
  mUsb.State = EfiUsbHcStateHalt;
  mUsb.Connected = mUsb.Enabled = FALSE;
  ForgetAllPipes ();
  ResetDetachDebounce ();
  mDiagnostic.RecoverySampleValid = FALSE;
  mDiagnostic.RecoveryProbeStatus = EFI_NOT_READY;
  mDiagnostic.RecoveryExhausted = FALSE;
  if (Retryable) {
    Cleanup = MotoG20UsbPlatformQuiesceForRetry ();
  } else {
    Cleanup = MotoG20UsbPlatformStop ();
  }
  mDiagnostic.CleanupStatus = Cleanup;
  mUsb.WaitingDetach = Retryable && !EFI_ERROR (Cleanup);
  mUsb.Fatal = !mUsb.WaitingDetach;
}

STATIC EFI_STATUS PollPendingSession (VOID) {
  EFI_STATUS Status;
  BOOLEAN Active;
  // Invoked at TPL_NOTIFY. PollRole refreshes the actual detector first; its
  // watchdog and stop policy remain authoritative during this bounded wait.
  Active = FALSE;
  Status = MotoG20UsbPlatformPollRole (&Active);
  if (EFI_ERROR (Status) || !Active) {
    if (!EFI_ERROR (Status)) { Status = EFI_NOT_READY; }
    ControllerFailed (Status, FALSE);
    return Status;
  }
  Status = SessionElapsed ();
  CaptureSessionEnd ();
  if (EFI_ERROR (Status)) {
    ControllerFailed (Status, TRUE);
    return Status;
  }
  ObserveSessionBits ();
  // A late callback cannot accept a connection beyond the wall-clock deadline.
  if ((mDiagnostic.SessionWaitUs >= MOTO_G20_USB_PEER_DEADLINE_US) || !SessionCanWait ()) {
    ControllerFailed (EFI_NOT_READY, TRUE);
    return EFI_NOT_READY;
  }
  if (SessionQualified ()) { SessionReady (); return EFI_SUCCESS; }
  return EFI_NOT_READY;
}

// Detector-only path. Never calls Init, and never accesses powered-off HC MMIO.
// 0/3 are detached sink/source; 9 is the DRP toggle sleep transition. Requiring
// an observed 0/3 avoids treating an unchanging sleep state alone as unplugged.
STATIC VOID ProbeRecoveryDetach (VOID) {
  EFI_STATUS Status;
  UINT16 State;
  BOOLEAN Vbus;
  UINT64 Now, Elapsed;
  EFI_TPL OldTpl;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = MotoG20UsbPlatformDetectRole (&State, &Vbus);
  mDiagnostic.RecoveryProbeStatus = Status;
  mDiagnostic.RecoverySampleValid = !EFI_ERROR (Status);
  if (EFI_ERROR (Status)) {
    ResetDetachDebounce ();
    if (Status != EFI_NOT_READY) {
      mDiagnostic.CleanupStatus = MotoG20UsbPlatformStop ();
      mUsb.WaitingDetach = FALSE;
      mUsb.Fatal = TRUE;
      Record (MotoUsbHcStageWaitRole, Status);
    }
    gBS->RestoreTPL (OldTpl);
    return;
  }
  mDiagnostic.RecoveryTypecState = State;
  mDiagnostic.RecoveryVbus = Vbus;
  if (Vbus || ((State != 0) && (State != 3) && (State != 9))) {
    ResetDetachDebounce ();
    gBS->RestoreTPL (OldTpl);
    return;
  }
  Now = GetPerformanceCounter ();
  if (!mUsb.DetachTracking) {
    mUsb.DetachSince = Now;
    mUsb.DetachTracking = TRUE;
  }
  if ((State == 0) || (State == 3)) { mUsb.DetachedStateObserved = TRUE; }
  // This platform TimerLib counter is ascending. A reversal cannot certify time.
  if (Now < mUsb.DetachSince) {
    ResetDetachDebounce ();
    mDiagnostic.CleanupStatus = MotoG20UsbPlatformStop ();
    mDiagnostic.RecoveryProbeStatus = EFI_DEVICE_ERROR;
    mDiagnostic.RecoverySampleValid = FALSE;
    mUsb.WaitingDetach = FALSE;
    mUsb.Fatal = TRUE;
    Record (MotoUsbHcStageWaitRole, EFI_DEVICE_ERROR);
    gBS->RestoreTPL (OldTpl);
    return;
  }
  Elapsed = GetTimeInNanoSecond (Now - mUsb.DetachSince);
  mDiagnostic.DetachStableMs = (UINT32)MIN (Elapsed / 1000000ULL, MOTO_G20_USB_RECOVERY_DETACH_MS);
  if ((Elapsed >= (UINT64)MOTO_G20_USB_RECOVERY_DETACH_MS * 1000000ULL) && mUsb.DetachedStateObserved) {
    mUsb.WaitingDetach = FALSE;
    mUsb.DetachTracking = FALSE;
    mDiagnostic.RecoverySampleValid = FALSE;
    Increment32 (&mDiagnostic.Rearms);
    Record (MotoUsbHcStageWaitRole, EFI_NOT_READY);
  }
  gBS->RestoreTPL (OldTpl);
}

STATIC EFI_STATUS EFIAPI GetCapability (EFI_USB2_HC_PROTOCOL *This, UINT8 *Speed, UINT8 *Ports, UINT8 *Is64Bit) {
  if (!ValidThis (This) || (Speed == NULL) || (Ports == NULL) || (Is64Bit == NULL)) { return EFI_INVALID_PARAMETER; }
  *Speed = EFI_USB_SPEED_HIGH;
  *Ports = 1;
  *Is64Bit = FALSE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI GetState (EFI_USB2_HC_PROTOCOL *This, EFI_USB_HC_STATE *State) {
  if (!ValidThis (This) || (State == NULL)) { return EFI_INVALID_PARAMETER; }
  *State = mUsb.State;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI SetState (EFI_USB2_HC_PROTOCOL *This, EFI_USB_HC_STATE State) {
  EFI_TPL OldTpl;
  EFI_STATUS Status;
  if (!ValidThis (This) || (State >= EfiUsbHcStateMaximum)) { return EFI_INVALID_PARAMETER; }
  if (State == EfiUsbHcStateSuspend) { return EFI_UNSUPPORTED; }
  Status = BeginIo (&OldTpl);
  if (EFI_ERROR (Status)) { return Status; }
  if (State == EfiUsbHcStateHalt) { StopAllPipeHardware (); ForgetAllPipes (); }
  mUsb.State = State;
  EndIo (OldTpl);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Reset (EFI_USB2_HC_PROTOCOL *This, UINT16 Attributes) {
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  if (!ValidThis (This)) { return EFI_INVALID_PARAMETER; }
  if ((Attributes != EFI_USB_HC_RESET_GLOBAL) && (Attributes != EFI_USB_HC_RESET_HOST_CONTROLLER)) { return EFI_UNSUPPORTED; }
  Status = BeginIo (&OldTpl);
  if (EFI_ERROR (Status)) { return Status; }
  StopAllPipeHardware ();
  ForgetAllPipes ();
  Status = ConfigureController (FALSE);
  Record (MotoUsbHcStageController, Status);
  if (EFI_ERROR (Status)) { ControllerFailed (Status, !mUsb.SessionAccessLost); }
  mUsb.State = EfiUsbHcStateHalt;
  EndIo (OldTpl);
  return Status;
}

STATIC EFI_STATUS EFIAPI GetPort (EFI_USB2_HC_PROTOCOL *This, UINT8 Port, EFI_USB_PORT_STATUS *Status) {
  UINT8 Dev;
  EFI_TPL OldTpl;
  if (!ValidThis (This) || (Port != 0) || (Status == NULL)) { return EFI_INVALID_PARAMETER; }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mUsb.Busy) { gBS->RestoreTPL (OldTpl); return EFI_NOT_READY; }
  Status->PortStatus = 0;
  if (!EFI_ERROR (CheckRole ())) { RefreshPort (); }
  if (mUsb.Ready) {
    Status->PortStatus = USB_PORT_STAT_POWER;
    if (mUsb.Connected) {
      Status->PortStatus |= USB_PORT_STAT_CONNECTION;
      Dev = R8 (MU_DEVCTL);
      if ((Dev & MU_DEV_LS) != 0) { Status->PortStatus |= USB_PORT_STAT_LOW_SPEED; }
      if ((R8 (MU_POWER) & MU_POWER_HSMODE) != 0) { Status->PortStatus |= USB_PORT_STAT_HIGH_SPEED; }
    }
    if (mUsb.Enabled) { Status->PortStatus |= USB_PORT_STAT_ENABLE; }
    if (mUsb.Suspended) { Status->PortStatus |= USB_PORT_STAT_SUSPEND; }
    if (mUsb.Resetting) { Status->PortStatus |= USB_PORT_STAT_RESET; }
  }
  Status->PortChangeStatus = mUsb.Changes;
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI SetPort (EFI_USB2_HC_PROTOCOL *This, UINT8 Port, EFI_USB_PORT_FEATURE Feature) {
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  if (!ValidThis (This) || (Port != 0)) { return EFI_INVALID_PARAMETER; }
  if (Feature == EfiUsbPortPower) { mUsb.PowerOffRequested = FALSE; }
  Status = BeginIo (&OldTpl);
  if (EFI_ERROR (Status)) { return Status; }
  if (Feature == EfiUsbPortPower) { EndIo (OldTpl); return EFI_SUCCESS; }
  if (Feature == EfiUsbPortReset) {
    RefreshPort ();
    if (!mUsb.Connected) { EndIo (OldTpl); return EFI_NOT_READY; }
    mDiagnostic.RootResetValid = FALSE;
    mDiagnostic.RootResetDevCtl = mDiagnostic.RootResetPower = 0;
    StopAllPipeHardware ();
    ForgetAllPipes ();
    mUsb.Resetting = TRUE;
    mUsb.Enabled = FALSE;
    W8 (MU_POWER, (UINT8)((R8 (MU_POWER) & ~MU_POWER_SUSPEND) | MU_POWER_RESET));
    MicroSecondDelay (50000);
    // Hardware remains reset until ClearPort(Reset), as required by UsbBusDxe.
    EndIo (OldTpl);
    return EFI_SUCCESS;
  }
  EndIo (OldTpl);
  if (Feature == EfiUsbPortSuspend) { return EFI_UNSUPPORTED; }
  return EFI_INVALID_PARAMETER;
}

STATIC EFI_STATUS EFIAPI ClearPort (EFI_USB2_HC_PROTOCOL *This, UINT8 Port, EFI_USB_PORT_FEATURE Feature) {
  EFI_TPL OldTpl;
  EFI_STATUS Status;
  if (!ValidThis (This) || (Port != 0)) { return EFI_INVALID_PARAMETER; }
  switch (Feature) {
    case EfiUsbPortConnectChange: mUsb.Changes &= ~USB_PORT_STAT_C_CONNECTION; return EFI_SUCCESS;
    case EfiUsbPortEnableChange: mUsb.Changes &= ~USB_PORT_STAT_C_ENABLE; return EFI_SUCCESS;
    case EfiUsbPortSuspendChange: mUsb.Changes &= ~USB_PORT_STAT_C_SUSPEND; return EFI_SUCCESS;
    case EfiUsbPortOverCurrentChange: mUsb.Changes &= ~USB_PORT_STAT_C_OVERCURRENT; return EFI_SUCCESS;
    case EfiUsbPortResetChange: mUsb.Changes &= ~USB_PORT_STAT_C_RESET; return EFI_SUCCESS;
    default: break;
  }
  Status = BeginIo (&OldTpl);
  if (EFI_ERROR (Status)) { return Status; }
  if (Feature == EfiUsbPortReset) {
    if (!mUsb.Resetting) { EndIo (OldTpl); return EFI_SUCCESS; }
    W8 (MU_POWER, (UINT8)(R8 (MU_POWER) & ~MU_POWER_RESET));
    MicroSecondDelay (20000);
    mUsb.Resetting = FALSE;
    RefreshPort ();
    mUsb.Enabled = mUsb.Connected;
    mUsb.Changes |= USB_PORT_STAT_C_RESET;
    // BeginIo still holds TPL_NOTIFY. RefreshPort does not call the platform
    // monitor or gate HC clocks. Do not sample if access has been invalidated.
    if (mUsb.Ready) {
      mDiagnostic.RootResetDevCtl = R8 (MU_DEVCTL);
      mDiagnostic.RootResetPower = R8 (MU_POWER);
      mDiagnostic.RootResetValid = TRUE;
    }
    Status = mUsb.Enabled ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    EndIo (OldTpl);
    return Status;
  }
  if (Feature == EfiUsbPortEnable) {
    StopAllPipeHardware ();
    ForgetAllPipes ();
    mUsb.Enabled = FALSE;
    EndIo (OldTpl);
    return EFI_SUCCESS;
  }
  if (Feature == EfiUsbPortPower) {
    StopControllerHardware ();
    ForgetAllPipes ();
    mUsb.Ready = FALSE;
    mUsb.Enabled = FALSE;
    mUsb.Connected = FALSE;
    mUsb.State = EfiUsbHcStateHalt;
    mUsb.PowerOffRequested = TRUE;
    Status = MotoG20UsbPlatformStop ();
    EndIo (OldTpl);
    return Status;
  }
  EndIo (OldTpl);
  return EFI_UNSUPPORTED;
}

// All stages share the caller's bounded deadline, including error recovery.
STATIC EFI_STATUS WaitEp0 (UINTN *BudgetUs, UINT32 *Result, MOTO_G20_USB_EP0_TRACE *Trace) {
  UINT16 Csr;
  UINT16 Interrupt;
  UINT8 DevCtl;
  UINTN RoleCountdown;
  ZeroMem (Trace, sizeof (*Trace));
  Trace->Valid = TRUE;
  RoleCountdown = 10000;
  while (*BudgetUs >= 10) {
    Increment32 (&Trace->Polls);
    if (--RoleCountdown == 0) {
      if (EFI_ERROR (CheckRole ())) {
        // Preserve only the preceding poll's historical samples. CheckRole
        // may have stopped platform clocks: absolutely no further HC reads.
        Trace->Cause = MOTO_G20_USB_EP0_CAUSE_ROLE_LOST;
        Trace->HardwareAccessLost = TRUE;
        *Result = EFI_USB_ERR_SYSTEM; return EFI_DEVICE_ERROR;
      }
      RoleCountdown = 10000;
    }
    Trace->ValidMask = 0;
    Trace->DevCtl = 0;
    Csr = R16 (MU_CSR0);
    Interrupt = R16 (MU_INTRTX);
    Trace->Csr0 = Csr;
    Trace->IntrTx = Interrupt;
    Trace->ValidMask = MOTO_G20_USB_EP0_CSR_VALID | MOTO_G20_USB_EP0_INTRTX_VALID;
    if ((Csr & MU_C0_STALL) != 0) {
      Trace->Cause = MOTO_G20_USB_EP0_CAUSE_STALL;
      *Result = EFI_USB_ERR_STALL; return EFI_DEVICE_ERROR;
    }
    if ((Csr & MU_C0_ERROR) != 0) {
      Trace->Cause = MOTO_G20_USB_EP0_CAUSE_CSR_ERROR;
      *Result = EFI_USB_ERR_SYSTEM; return EFI_DEVICE_ERROR;
    }
    if ((Csr & MU_C0_NAK) != 0) {
      Trace->Cause = MOTO_G20_USB_EP0_CAUSE_NAK;
      *Result = EFI_USB_ERR_TIMEOUT | EFI_USB_ERR_NAK; return EFI_TIMEOUT;
    }
    if ((Interrupt & 1) != 0) { return EFI_SUCCESS; }
    DevCtl = R8 (MU_DEVCTL);
    Trace->DevCtl = DevCtl;
    Trace->ValidMask |= MOTO_G20_USB_EP0_DEVCTL_VALID;
    if ((DevCtl & (MU_DEV_HOST | MU_DEV_VBUS)) != (MU_DEV_HOST | MU_DEV_VBUS)) {
      Trace->Cause = MOTO_G20_USB_EP0_CAUSE_HOST_VBUS_LOST;
      *Result = EFI_USB_ERR_SYSTEM; return EFI_DEVICE_ERROR;
    }
    MicroSecondDelay (10);
    *BudgetUs -= 10;
    Trace->DelayUs += 10; // Bounded by CONTROL_MAX_MS; same delays as V5.
  }
  Trace->Cause = MOTO_G20_USB_EP0_CAUSE_TIMEOUT;
  *Result = EFI_USB_ERR_TIMEOUT;
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS ControlInternal (
  EFI_USB2_HC_PROTOCOL *This, UINT8 Address, UINT8 Speed, UINTN MaxPacket,
  EFI_USB_DEVICE_REQUEST *Request, EFI_USB_DATA_DIRECTION Direction,
  VOID *Data, UINTN *DataLength, UINTN Timeout,
  EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator, UINT32 *Result, UINT8 *Phase,
  MOTO_G20_USB_EP0_TRACE *Trace)
{
  EFI_STATUS Status;
  UINTN Budget;
  UINTN Done;
  UINTN Count;
  UINTN Target;
  UINT16 Csr;
  UINT8 HubAddress;
  UINT8 HubPort;
  EFI_TPL OldTpl;
  *Phase = 0;
  ZeroMem (Trace, sizeof (*Trace));
  if (Result == NULL) { return EFI_INVALID_PARAMETER; }
  *Result = EFI_USB_ERR_NOTEXECUTE;
  if (!ValidThis (This) || (Request == NULL) || (Address > 127) || (Speed > EFI_USB_SPEED_HIGH) ||
      (Direction > EfiUsbNoData) || ((MaxPacket != 8) && (MaxPacket != 16) && (MaxPacket != 32) && (MaxPacket != 64)) ||
      ((Speed == EFI_USB_SPEED_LOW) && (MaxPacket != 8)) || ((Speed == EFI_USB_SPEED_HIGH) && (MaxPacket != 64))) {
    return EFI_INVALID_PARAMETER;
  }
  Status = GetRoute (Speed, Translator, &HubAddress, &HubPort);
  if (EFI_ERROR (Status)) { return Status; }
  Target = Request->Length;
  if ((Direction == EfiUsbNoData) ? (Target != 0) :
      ((Data == NULL) || (DataLength == NULL) || (*DataLength < Target) || (Target == 0) ||
       (((Request->RequestType & 0x80) != 0) != (Direction == EfiUsbDataIn)))) { return EFI_INVALID_PARAMETER; }
  if (mUsb.Busy || (mUsb.State != EfiUsbHcStateOperational) || !mUsb.Enabled) { return EFI_NOT_READY; }
  Status = BeginIo (&OldTpl);
  if (EFI_ERROR (Status)) { return Status; }
  Done = 0;
  Budget = ((Timeout == 0) || (Timeout > CONTROL_MAX_MS) ? CONTROL_MAX_MS : Timeout) * 1000;
  Select (0);
  W16 (MU_CSR0, 0);
  W16 (MU_CSR0, MU_C0_FLUSH);
  W16 (MU_CSR0, 0);
  (VOID)R16 (MU_INTRTX);
  W8 (MU_FADDR, Address);
  W8 (MU_TXFUNC (0), Address);
  W8 (MU_RXFUNC (0), Address);
  W8 (MU_TXHUB (0), HubAddress); W8 (MU_TXPORT (0), HubPort);
  W8 (MU_RXHUB (0), HubAddress); W8 (MU_RXPORT (0), HubPort);
  W8 (MU_TYPE0, TypeSpeed (Speed));
  W8 (MU_NAKLIMIT0, 16);
  FifoWrite (0, (UINT8 *)Request, sizeof (*Request));
  *Phase = 1;
  W16 (MU_CSR0, MU_C0_SETUP | MU_C0_TXREADY);
  Status = WaitEp0 (&Budget, Result, Trace);
  if (EFI_ERROR (Status)) { goto Finish; }
  while (Done < Target) {
    *Phase = 2;
    if (Direction == EfiUsbDataIn) {
      W16 (MU_CSR0, MU_C0_REQ);
      Status = WaitEp0 (&Budget, Result, Trace);
      if (EFI_ERROR (Status)) { goto Finish; }
      if ((R16 (MU_CSR0) & MU_C0_RXREADY) == 0) { *Result = EFI_USB_ERR_SYSTEM; Status = EFI_DEVICE_ERROR; goto Finish; }
      Count = R16 (MU_RXCOUNT);
      if ((Count > MaxPacket) || (Count > Target - Done)) {
        *Result = EFI_USB_ERR_BABBLE | EFI_USB_ERR_BUFFER;
        Status = EFI_DEVICE_ERROR;
        goto Finish;
      }
      FifoRead (0, (UINT8 *)Data + Done, Count);
      Done += Count;
      W16 (MU_CSR0, 0);
      if (Count < MaxPacket) { break; }
    } else {
      Count = MIN (MaxPacket, Target - Done);
      FifoWrite (0, (UINT8 *)Data + Done, Count);
      W16 (MU_CSR0, MU_C0_TXREADY);
      Status = WaitEp0 (&Budget, Result, Trace);
      if (EFI_ERROR (Status)) { goto Finish; }
      Done += Count;
    }
  }
  Csr = MU_C0_STATUS | MU_C0_NOPING;
  *Phase = 3;
  Csr |= (Direction == EfiUsbDataIn) ? MU_C0_TXREADY : MU_C0_REQ;
  W16 (MU_CSR0, Csr);
  Status = WaitEp0 (&Budget, Result, Trace);
  if (!EFI_ERROR (Status) && (Direction != EfiUsbDataIn) &&
      ((R16 (MU_CSR0) & MU_C0_RXREADY) != 0) && (R16 (MU_RXCOUNT) != 0)) {
    *Result = EFI_USB_ERR_BABBLE | EFI_USB_ERR_BUFFER;
    Status = EFI_DEVICE_ERROR;
  }
  if (!EFI_ERROR (Status)) { *Result = EFI_USB_NOERROR; }
Finish:
  if (mUsb.Ready) {
    W16 (MU_CSR0, 0);
    if (EFI_ERROR (Status)) { W16 (MU_CSR0, MU_C0_FLUSH); W16 (MU_CSR0, 0); }
  }
  if (DataLength != NULL) { *DataLength = Done; }
  mDiagnostic.LastTransferResult = *Result;
  if (EFI_ERROR (Status)) { Record (MotoUsbHcStageControlError, Status); }
  else { Increment64 (&mDiagnostic.ControlCompleted); }
  EndIo (OldTpl);
  return Status;
}

STATIC VOID CacheTransferFailure (
  UINT8 Kind, UINT8 Phase, UINT8 Address, UINT8 Endpoint, UINT8 Speed,
  EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator, EFI_USB_DEVICE_REQUEST *Request,
  EFI_STATUS Status, UINT32 *Result, CONST MOTO_G20_USB_EP0_TRACE *Trace)
{
  MOTO_G20_USB_TRANSFER_FAILURE *Failure;
  // Caller holds TPL_NOTIFY. Do not perform any bus access for diagnostics.
  Failure = &mDiagnostic.LastFailure;
  ZeroMem (Failure, sizeof (*Failure));
  Failure->Valid = TRUE;
  Failure->Kind = Kind;
  Failure->Phase = Phase;
  Failure->Address = Address;
  Failure->Endpoint = Endpoint;
  Failure->Speed = Speed;
  if (Translator != NULL) {
    Failure->HubAddress = Translator->TranslatorHubAddress;
    Failure->HubPort = Translator->TranslatorPortNumber;
  }
  if (Request != NULL) {
    Failure->RequestValid = TRUE;
    Failure->RequestType = Request->RequestType;
    Failure->Request = Request->Request;
    Failure->Value = Request->Value;
    Failure->Index = Request->Index;
    Failure->Length = Request->Length;
  }
  Failure->Status = Status;
  Failure->ResultValid = Result != NULL;
  if (Result != NULL) { Failure->Result = *Result; }
  if (Trace != NULL) { CopyMem (&Failure->Ep0, Trace, sizeof (*Trace)); }
}

STATIC EFI_STATUS EFIAPI Control (
  EFI_USB2_HC_PROTOCOL *This, UINT8 Address, UINT8 Speed, UINTN MaxPacket,
  EFI_USB_DEVICE_REQUEST *Request, EFI_USB_DATA_DIRECTION Direction,
  VOID *Data, UINTN *DataLength, UINTN Timeout,
  EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator, UINT32 *Result)
{
  EFI_TPL OldTpl;
  EFI_STATUS Status;
  UINT8 Phase;
  MOTO_G20_USB_EP0_TRACE Trace;
  // Serialize the evidence with the operation, including early rejections.
  // Existing BeginIo nests the guard without lowering TPL during traffic.
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = ControlInternal (This, Address, Speed, MaxPacket, Request, Direction,
                            Data, DataLength, Timeout, Translator, Result, &Phase, &Trace);
  if (ValidThis (This)) {
    Increment64 (&mDiagnostic.ControlCalls);
    if (EFI_ERROR (Status)) {
      Increment64 (&mDiagnostic.ControlFailures);
      CacheTransferFailure (1, Phase, Address, 0, Speed, Translator, Request, Status, Result, &Trace);
    }
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

STATIC UINT8 InterruptInterval (UINT8 Speed, UINTN Milliseconds) {
  UINT8 Exponent;
  if (Speed != EFI_USB_SPEED_HIGH) { return (UINT8)Milliseconds; }
  // High speed register uses 2^(bInterval-1) microframes; round up.
  for (Exponent = 1; Exponent < 16; ++Exponent) {
    if (((UINTN)1 << (Exponent - 1)) >= Milliseconds * 8) { break; }
  }
  return Exponent;
}

STATIC VOID StartInterruptPacket (UINTN Index) {
  MUSB_INTERRUPT_PIPE *Pipe;
  UINT8 HardwareEndpoint;
  Pipe = &mUsb.Pipes[Index];
  HardwareEndpoint = (UINT8)(Index + 1);
  Select (HardwareEndpoint);
  W8 (MU_RXFUNC (HardwareEndpoint), Pipe->Address);
  W8 (MU_RXHUB (HardwareEndpoint), Pipe->HubAddress);
  W8 (MU_RXPORT (HardwareEndpoint), Pipe->HubPort);
  W16 (MU_RXMAXP, (UINT16)Pipe->MaxPacket);
  W8 (MU_RXTYPE, (UINT8)(TypeSpeed (Pipe->Speed) | 0x30 | (Pipe->Endpoint & 15)));
  W8 (MU_RXINTERVAL, InterruptInterval (Pipe->Speed, Pipe->Interval));
  W16 (MU_RXCSR, (UINT16)(MU_RX_DISNYET | MU_RX_WRTOG | (Pipe->Toggle ? MU_RX_TOGGLE : 0)));
  W16 (MU_RXCSR, (UINT16)(MU_RX_DISNYET | MU_RX_REQ));
  Pipe->Pending = TRUE;
}

STATIC EFI_STATUS AsyncInterruptInternal (
  EFI_USB2_HC_PROTOCOL *This, UINT8 Address, UINT8 Endpoint, UINT8 Speed, UINTN MaxPacket,
  BOOLEAN NewTransfer, UINT8 *Toggle, UINTN Interval, UINTN Length,
  EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator, EFI_ASYNC_USB_TRANSFER_CALLBACK Callback, VOID *Context)
{
  EFI_TPL OldTpl;
  EFI_STATUS Status;
  MUSB_INTERRUPT_PIPE *Pipe;
  UINTN Index;
  UINTN FreeIndex;
  UINT8 HubAddress;
  UINT8 HubPort;
  if (!ValidThis (This) || (Toggle == NULL) || (*Toggle > 1) || (Address > 127) ||
      ((Endpoint & 0x70) != 0) || ((Endpoint & 15) == 0)) { return EFI_INVALID_PARAMETER; }
  // Match and allocation are serialized too: cancellation can originate from
  // a UsbBusDxe notification at TPL_NOTIFY while our timer runs at CALLBACK.
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mUsb.Busy) { gBS->RestoreTPL (OldTpl); return EFI_NOT_READY; }
  FreeIndex = INTERRUPT_PIPE_COUNT;
  for (Index = 0; Index < INTERRUPT_PIPE_COUNT; ++Index) {
    Pipe = &mUsb.Pipes[Index];
    if (Pipe->Active && (Address == Pipe->Address) && (Endpoint == Pipe->Endpoint)) { break; }
    if (!Pipe->Active && (FreeIndex == INTERRUPT_PIPE_COUNT)) { FreeIndex = Index; }
  }
  if (!NewTransfer) {
    if (Index == INTERRUPT_PIPE_COUNT) { gBS->RestoreTPL (OldTpl); return EFI_INVALID_PARAMETER; }
    Pipe = &mUsb.Pipes[Index];
    if (EFI_ERROR (CheckRole ())) {
      *Toggle = Pipe->Toggle;
      ForgetPipe (Pipe);
      gBS->RestoreTPL (OldTpl);
      return EFI_SUCCESS;
    }
    if (mUsb.Ready && Pipe->Pending) {
      Select ((UINT8)(Index + 1));
      if ((R16 (MU_RXCSR) & MU_RX_READY) != 0) { Pipe->Toggle ^= 1; }
    }
    StopPipeHardware (Index);
    *Toggle = Pipe->Toggle;
    ForgetPipe (Pipe);
    gBS->RestoreTPL (OldTpl);
    return EFI_SUCCESS;
  }
  if (Index != INTERRUPT_PIPE_COUNT) { gBS->RestoreTPL (OldTpl); return EFI_INVALID_PARAMETER; }
  if ((Endpoint & 0x80) == 0) { gBS->RestoreTPL (OldTpl); return EFI_UNSUPPORTED; }
  if ((Speed > EFI_USB_SPEED_HIGH) || (MaxPacket == 0) || (MaxPacket > 1024) ||
      ((Speed == EFI_USB_SPEED_LOW) && (MaxPacket > 8)) || ((Speed == EFI_USB_SPEED_FULL) && (MaxPacket > 64)) ||
      (Length == 0) || (Length > MaxPacket) || (Interval == 0) || (Interval > 255) || (Callback == NULL)) {
    gBS->RestoreTPL (OldTpl); return EFI_INVALID_PARAMETER;
  }
  if (MaxPacket > INTERRUPT_FIFO_SIZE) { gBS->RestoreTPL (OldTpl); return EFI_UNSUPPORTED; }
  Status = GetRoute (Speed, Translator, &HubAddress, &HubPort);
  if (EFI_ERROR (Status)) { gBS->RestoreTPL (OldTpl); return Status; }
  if (FreeIndex == INTERRUPT_PIPE_COUNT) { gBS->RestoreTPL (OldTpl); return EFI_OUT_OF_RESOURCES; }
  if (!mUsb.Enabled || (mUsb.State != EfiUsbHcStateOperational)) { gBS->RestoreTPL (OldTpl); return EFI_NOT_READY; }
  Status = CheckRole ();
  if (EFI_ERROR (Status)) { gBS->RestoreTPL (OldTpl); return Status; }
  Pipe = &mUsb.Pipes[FreeIndex];
  Pipe->Address = Address; Pipe->Endpoint = Endpoint; Pipe->Speed = Speed;
  Pipe->HubAddress = HubAddress; Pipe->HubPort = HubPort;
  Pipe->MaxPacket = MaxPacket; Pipe->Length = Length; Pipe->Interval = Interval;
  Pipe->Elapsed = 0; Pipe->Toggle = *Toggle;
  Pipe->Callback = Callback; Pipe->CallbackContext = Context;
  StopPipeHardware (FreeIndex);
  ++Pipe->Generation;
  Pipe->Active = TRUE;
  StartInterruptPacket (FreeIndex);
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI AsyncInterrupt (
  EFI_USB2_HC_PROTOCOL *This, UINT8 Address, UINT8 Endpoint, UINT8 Speed, UINTN MaxPacket,
  BOOLEAN NewTransfer, UINT8 *Toggle, UINTN Interval, UINTN Length,
  EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator, EFI_ASYNC_USB_TRANSFER_CALLBACK Callback, VOID *Context)
{
  EFI_TPL OldTpl;
  EFI_STATUS Status;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = AsyncInterruptInternal (This, Address, Endpoint, Speed, MaxPacket,
                                   NewTransfer, Toggle, Interval, Length, Translator, Callback, Context);
  if (ValidThis (This) && EFI_ERROR (Status)) {
    Increment64 (&mDiagnostic.AsyncFailures);
    CacheTransferFailure (NewTransfer ? 2 : 4, 0, Address, Endpoint, Speed,
                           Translator, NULL, Status, NULL, NULL);
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

STATIC VOID PollInterruptPipe (UINTN Index) {
  UINT16 Csr;
  UINTN Count;
  UINT32 Result;
  EFI_ASYNC_USB_TRANSFER_CALLBACK Callback;
  VOID *Context;
  EFI_TPL OldTpl;
  UINT64 Generation;
  UINT64 ObservationGeneration;
  EFI_STATUS CallbackStatus;
  EFI_USB2_HC_TRANSACTION_TRANSLATOR Translator;
  MUSB_INTERRUPT_PIPE *Pipe;
  Pipe = &mUsb.Pipes[Index];
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mUsb.Busy || !Pipe->Active || !mUsb.Enabled ||
      (mUsb.State != EfiUsbHcStateOperational) || EFI_ERROR (CheckRole ())) { goto NoReport; }
  if (!Pipe->Pending) {
    if (++Pipe->Elapsed >= Pipe->Interval) { Pipe->Elapsed = 0; StartInterruptPacket (Index); }
    goto NoReport;
  }
  Select ((UINT8)(Index + 1));
  Csr = R16 (MU_RXCSR);
  Result = EFI_USB_NOERROR;
  Count = 0;
  if ((Csr & MU_RX_STALL) != 0) { Result = EFI_USB_ERR_STALL; }
  else if ((Csr & MU_RX_ERROR) != 0) { Result = EFI_USB_ERR_SYSTEM; }
  else if ((Csr & MU_RX_DATAERR) != 0) {
    // Host interrupt NAK timeout is not a keyboard report or a transfer success.
    W16 (MU_RXCSR, MU_RX_DISNYET);
    Pipe->Pending = FALSE;
    Pipe->Elapsed = 0;
    goto NoReport;
  } else if ((Csr & MU_RX_READY) != 0) {
    Count = R16 (MU_RXCOUNT);
    if ((Count > Pipe->MaxPacket) || (Count > Pipe->Length)) { Result = EFI_USB_ERR_BABBLE | EFI_USB_ERR_BUFFER; Count = 0; }
    else { FifoRead ((UINT8)(Index + 1), Pipe->Buffer, Count); Pipe->Toggle ^= 1; }
  } else { goto NoReport; }
  W16 (MU_RXCSR, MU_RX_DISNYET);
  if (Result != EFI_USB_NOERROR) { W16 (MU_RXCSR, MU_RX_FLUSH | MU_RX_DISNYET); W16 (MU_RXCSR, MU_RX_DISNYET); }
  (VOID)R16 (MU_INTRRX);
  Pipe->Pending = FALSE;
  Pipe->Elapsed = 0;
  Callback = Pipe->Callback;
  Context = Pipe->CallbackContext;
  Generation = Pipe->Generation;
  ObservationGeneration = ++mUsb.InterruptObservationGeneration;
  ZeroMem (&mDiagnostic.LastInterrupt, sizeof (mDiagnostic.LastInterrupt));
  mDiagnostic.LastInterrupt.Valid = TRUE;
  mDiagnostic.LastInterrupt.Address = Pipe->Address;
  mDiagnostic.LastInterrupt.Endpoint = Pipe->Endpoint;
  mDiagnostic.LastInterrupt.Length = Count;
  mDiagnostic.LastInterrupt.Result = Result;
  mDiagnostic.LastTransferResult = Result;
  if (Result != EFI_USB_NOERROR) {
    Record (MotoUsbHcStageInterruptError, EFI_DEVICE_ERROR);
    Increment64 (&mDiagnostic.InterruptFailures);
    Translator.TranslatorHubAddress = Pipe->HubAddress;
    Translator.TranslatorPortNumber = Pipe->HubPort;
    CacheTransferFailure (3, 0, Pipe->Address, Pipe->Endpoint, Pipe->Speed,
                           &Translator, NULL, EFI_DEVICE_ERROR, &Result, NULL);
  }
  else {
    Increment64 (&mDiagnostic.InterruptReports);
    if (Count == 0) { Increment64 (&mDiagnostic.ZeroLengthInterruptReports); }
    Record (MotoUsbHcStageInterruptReport, EFI_SUCCESS);
  }
  // Callback may cancel/recreate the pipe. Do not touch its state afterwards.
  gBS->RestoreTPL (OldTpl);
  if (Pipe->Active && (Pipe->Generation == Generation)) {
    CallbackStatus = Callback (Pipe->Buffer, Count, Context, Result);
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    // A nested report must retain its own callback result. This generation is
    // an ownership token, not a diagnostic count. Do not touch the old pipe.
    if (mUsb.InterruptObservationGeneration == ObservationGeneration) {
      mDiagnostic.LastInterrupt.CallbackStatus = CallbackStatus;
      mDiagnostic.LastInterrupt.CallbackStatusValid = TRUE;
    }
    gBS->RestoreTPL (OldTpl);
  }
  return;
NoReport:
  gBS->RestoreTPL (OldTpl);
}

STATIC VOID PollInterrupt (VOID) {
  UINTN Index;
  for (Index = 0; Index < INTERRUPT_PIPE_COUNT; ++Index) { PollInterruptPipe (Index); }
}

STATIC EFI_STATUS EFIAPI Bulk (
  EFI_USB2_HC_PROTOCOL *This, UINT8 Address, UINT8 Endpoint, UINT8 Speed, UINTN Packet,
  UINT8 Buffers, VOID *Data[EFI_USB_MAX_BULK_BUFFER_NUM], UINTN *Length, UINT8 *Toggle,
  UINTN Timeout, EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator, UINT32 *Result)
{
  if (Result != NULL) { *Result = EFI_USB_ERR_NOTEXECUTE; }
  return EFI_UNSUPPORTED;
}
STATIC EFI_STATUS EFIAPI SyncInterrupt (
  EFI_USB2_HC_PROTOCOL *This, UINT8 Address, UINT8 Endpoint, UINT8 Speed, UINTN Packet,
  VOID *Data, UINTN *Length, UINT8 *Toggle, UINTN Timeout,
  EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator, UINT32 *Result)
{
  if (Result != NULL) { *Result = EFI_USB_ERR_NOTEXECUTE; }
  return EFI_UNSUPPORTED;
}
STATIC EFI_STATUS EFIAPI Iso (
  EFI_USB2_HC_PROTOCOL *This, UINT8 Address, UINT8 Endpoint, UINT8 Speed, UINTN Packet,
  UINT8 Buffers, VOID *Data[EFI_USB_MAX_ISO_BUFFER_NUM], UINTN Length,
  EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator, UINT32 *Result)
{
  if (Result != NULL) { *Result = EFI_USB_ERR_NOTEXECUTE; }
  return EFI_UNSUPPORTED;
}
STATIC EFI_STATUS EFIAPI AsyncIso (
  EFI_USB2_HC_PROTOCOL *This, UINT8 Address, UINT8 Endpoint, UINT8 Speed, UINTN Packet,
  UINT8 Buffers, VOID *Data[EFI_USB_MAX_ISO_BUFFER_NUM], UINTN Length,
  EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator, EFI_ASYNC_USB_TRANSFER_CALLBACK Callback, VOID *Context)
{
  return EFI_UNSUPPORTED;
}

STATIC VOID EFIAPI TimerTick (EFI_EVENT Event, VOID *Context) {
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  if (mUsb.Ended || mUsb.Busy || mUsb.Fatal || mUsb.PowerOffRequested) { return; }
  if (mUsb.SessionPending) {
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    mUsb.Busy = TRUE;
    Status = PollPendingSession ();
    Record (MotoUsbHcStageController, Status);
    mUsb.Busy = FALSE;
    gBS->RestoreTPL (OldTpl);
    if (EFI_ERROR (Status)) { return; }
    mUsb.RoleTicks = ROLE_POLL_TICKS - 1;
  }
  if (++mUsb.RoleTicks >= ROLE_POLL_TICKS) {
    mUsb.RoleTicks = 0;
    if (mUsb.WaitingDetach) { ProbeRecoveryDetach (); return; }
    if (!mUsb.Ready) {
      // Keep the platform monitor deferred between Init's success and the
      // final HC access. Init nests this TPL guard without lowering it.
      OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
      mUsb.Busy = TRUE;
      Status = MotoG20UsbPlatformInit (&mUsb.Base);
      if (!EFI_ERROR (Status)) {
        Status = ConfigureController (TRUE);
        Record (MotoUsbHcStageController, Status);
        if (EFI_ERROR (Status) && !mUsb.SessionPending) {
          ControllerFailed (Status, !mUsb.SessionAccessLost);
        }
      } else {
        Record (MotoUsbHcStageWaitRole, Status);
        if (Status != EFI_NOT_READY) {
          mDiagnostic.CleanupStatus = MotoG20UsbPlatformStop ();
          mUsb.Fatal = TRUE;
        }
      }
      mUsb.Busy = FALSE;
      gBS->RestoreTPL (OldTpl);
      if (EFI_ERROR (Status)) { return; }
    } else if (EFI_ERROR (CheckRole ())) { return; }
    if (!mUsb.Published) {
      OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
      if (EFI_ERROR (CheckRole ())) { gBS->RestoreTPL (OldTpl); return; }
      Status = gBS->InstallMultipleProtocolInterfaces (&mUsb.Handle,
        &gEfiUsb2HcProtocolGuid, &mUsb.Hc, &gEfiDevicePathProtocolGuid, &mPath, NULL);
      Record (MotoUsbHcStageInstall, Status);
      if (EFI_ERROR (Status)) {
        mUsb.Fatal = TRUE;
        StopControllerHardware ();
        mUsb.Ready = FALSE;
        (VOID)MotoG20UsbPlatformStop ();
        gBS->RestoreTPL (OldTpl);
        return;
      }
      mUsb.Published = TRUE;
      mUsb.ConnectPending = TRUE;
      gBS->RestoreTPL (OldTpl);
    }
    if (mUsb.ConnectPending) {
      // Handles created after BDS ConnectAll still need bus/keyboard binding.
      Status = gBS->ConnectController (mUsb.Handle, NULL, NULL, TRUE);
      Record (MotoUsbHcStageConnect, Status);
      if (!EFI_ERROR (Status)) { mUsb.ConnectPending = FALSE; }
    }
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (!mUsb.Ready || EFI_ERROR (CheckRole ())) { gBS->RestoreTPL (OldTpl); return; }
  RefreshPort ();
  gBS->RestoreTPL (OldTpl);
  PollInterrupt ();
}

STATIC VOID EFIAPI ExitBoot (EFI_EVENT Event, VOID *Context) {
  BOOLEAN Active;
  // Platform may already have shut the PHY down from its own monitor event.
  if (mUsb.SessionPending) {
    Active = FALSE;
    if (!EFI_ERROR (MotoG20UsbPlatformPollRole (&Active)) && Active) { StopControllerHardware (); }
  } else if (!EFI_ERROR (CheckRole ())) { StopControllerHardware (); }
  mUsb.SessionPending = FALSE;
  mUsb.Ended = TRUE;
  Record (MotoUsbHcStageStopped, EFI_SUCCESS);
  (VOID)gBS->SetTimer (mUsb.Timer, TimerCancel, 0);
  ForgetAllPipes ();
  mUsb.Ready = FALSE;
  (VOID)MotoG20UsbPlatformShutdown ();
}

EFI_STATUS EFIAPI MotoG20MusbEntryPoint (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status;
  ZeroMem (&mUsb, sizeof (mUsb));
  ZeroMem (&mDiagnostic, sizeof (mDiagnostic));
  mDiagnostic.SessionLimit = MOTO_G20_USB_SESSION_ATTEMPT_LIMIT;
  mDiagnostic.SessionStatus = EFI_NOT_READY;
  mDiagnostic.CleanupStatus = EFI_NOT_READY;
  mDiagnostic.RecoveryProbeStatus = EFI_NOT_READY;
  Record (MotoUsbHcStagePrepare, EFI_NOT_READY);
  Status = gBS->InstallMultipleProtocolInterfaces (&mDiagnosticHandle, &mDiagnosticGuid, &mDiagnosticProtocol, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Status = MotoG20UsbPlatformPrepare ();
  Record (MotoUsbHcStagePrepare, Status);
  // The separate diagnostic handle keeps the loaded image resident on a
  // preparation failure. It exposes no USB host controller protocol.
  if (EFI_ERROR (Status)) { (VOID)MotoG20UsbPlatformShutdown (); return EFI_SUCCESS; }
  mUsb.Hc.GetCapability = GetCapability; mUsb.Hc.Reset = Reset;
  mUsb.Hc.GetState = GetState; mUsb.Hc.SetState = SetState;
  mUsb.Hc.ControlTransfer = Control; mUsb.Hc.BulkTransfer = Bulk;
  mUsb.Hc.AsyncInterruptTransfer = AsyncInterrupt; mUsb.Hc.SyncInterruptTransfer = SyncInterrupt;
  mUsb.Hc.IsochronousTransfer = Iso; mUsb.Hc.AsyncIsochronousTransfer = AsyncIso;
  mUsb.Hc.GetRootHubPortStatus = GetPort; mUsb.Hc.SetRootHubPortFeature = SetPort;
  mUsb.Hc.ClearRootHubPortFeature = ClearPort;
  mUsb.Hc.MajorRevision = 2; mUsb.Hc.MinorRevision = 0;
  mUsb.State = EfiUsbHcStateHalt;
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, TimerTick, NULL, &mUsb.Timer);
  if (EFI_ERROR (Status)) { (VOID)MotoG20UsbPlatformShutdown (); Record (MotoUsbHcStagePrepare, Status); return EFI_SUCCESS; }
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ExitBoot, NULL, &gEfiEventExitBootServicesGuid, &mUsb.ExitEvent);
  if (EFI_ERROR (Status)) { gBS->CloseEvent (mUsb.Timer); (VOID)MotoG20UsbPlatformShutdown (); Record (MotoUsbHcStagePrepare, Status); return EFI_SUCCESS; }
  Status = gBS->SetTimer (mUsb.Timer, TimerPeriodic, 10000);
  if (EFI_ERROR (Status)) { gBS->CloseEvent (mUsb.Timer); gBS->CloseEvent (mUsb.ExitEvent); (VOID)MotoG20UsbPlatformShutdown (); Record (MotoUsbHcStagePrepare, Status); return EFI_SUCCESS; }
  // Resident while the PC is attached. No HC protocol is published until safe
  // source role, PHY, VBUS and MUSB host session have actually been verified.
  return EFI_SUCCESS;
}
