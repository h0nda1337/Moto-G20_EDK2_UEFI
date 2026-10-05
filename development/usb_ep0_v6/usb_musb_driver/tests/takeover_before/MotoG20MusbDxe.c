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
  UINT16 Changes;
  UINTN RoleTicks;
  MUSB_INTERRUPT_PIPE Pipes[INTERRUPT_PIPE_COUNT];
} MUSB_HOST;

STATIC MUSB_HOST mUsb;
STATIC EFI_HANDLE mDiagnosticHandle;
STATIC EFI_GUID mDiagnosticGuid = MOTO_G20_USB_DIAGNOSTIC_PROTOCOL_GUID;
STATIC MOTO_G20_USB_DIAGNOSTIC_STATUS mDiagnostic;
STATIC EFI_STATUS EFIAPI ReadDiagnostic (MOTO_G20_USB_DIAGNOSTIC_PROTOCOL *This, MOTO_G20_USB_DIAGNOSTIC_STATUS *Status);
STATIC MOTO_G20_USB_DIAGNOSTIC_PROTOCOL mDiagnosticProtocol = { 1, ReadDiagnostic };

STATIC VOID Record (MOTO_G20_USB_HC_STAGE Stage, EFI_STATUS Status) {
  mDiagnostic.Stage = Stage;
  mDiagnostic.Status = Status;
}
STATIC EFI_STATUS EFIAPI ReadDiagnostic (MOTO_G20_USB_DIAGNOSTIC_PROTOCOL *This, MOTO_G20_USB_DIAGNOSTIC_STATUS *Status) {
  MOTO_G20_USB_PLATFORM_STAGE PlatformStage;
  if ((This != &mDiagnosticProtocol) || (Status == NULL)) { return EFI_INVALID_PARAMETER; }
  CopyMem (Status, &mDiagnostic, sizeof (*Status));
  Status->Revision = 1;
  Status->Size = sizeof (*Status);
  Status->HostReady = mUsb.Ready;
  Status->HcPublished = mUsb.Published;
  Status->PortConnected = mUsb.Connected;
  Status->PortEnabled = mUsb.Enabled;
  MotoG20UsbPlatformGetDiagnostic (&PlatformStage, &Status->PlatformStatus, &Status->TypecState, &Status->VbusPresent);
  Status->PlatformStage = (UINT32)PlatformStage;
  return EFI_SUCCESS;
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

STATIC EFI_STATUS ConfigureController (VOID) {
  UINT8 Config;
  UINT16 Version;
  UINTN Wait;
  UINTN Channel;
  UINT16 Csr;
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
  mDiagnostic.ControllerStep = 3;
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
  mDiagnostic.ControllerStep = 4;
  W8 (MU_OTG_EXT_CSR, (UINT8)(R8 (MU_OTG_EXT_CSR) | MU_HOST_FORCE));
  W8 (MU_DEVCTL, (UINT8)(R8 (MU_DEVCTL) | MU_DEV_SESSION));
  for (Wait = 0; Wait < 100; ++Wait) {
    if ((R8 (MU_DEVCTL) & (MU_DEV_HOST | MU_DEV_VBUS)) == (MU_DEV_HOST | MU_DEV_VBUS)) { break; }
    MicroSecondDelay (1000);
  }
  if (Wait == 100) { return EFI_NOT_READY; }
  mUsb.Ready = TRUE;
  mUsb.State = EfiUsbHcStateOperational;
  mUsb.Enabled = FALSE;
  mUsb.Suspended = FALSE;
  mUsb.Resetting = FALSE;
  RefreshPort ();
  return EFI_SUCCESS;
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
  Status = ConfigureController ();
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
STATIC EFI_STATUS WaitEp0 (UINTN *BudgetUs, UINT32 *Result) {
  UINT16 Csr;
  UINT16 Interrupt;
  UINTN RoleCountdown;
  RoleCountdown = 10000;
  while (*BudgetUs >= 10) {
    if (--RoleCountdown == 0) {
      if (EFI_ERROR (CheckRole ())) { *Result = EFI_USB_ERR_SYSTEM; return EFI_DEVICE_ERROR; }
      RoleCountdown = 10000;
    }
    Csr = R16 (MU_CSR0);
    Interrupt = R16 (MU_INTRTX);
    if ((Csr & MU_C0_STALL) != 0) { *Result = EFI_USB_ERR_STALL; return EFI_DEVICE_ERROR; }
    if ((Csr & MU_C0_ERROR) != 0) { *Result = EFI_USB_ERR_SYSTEM; return EFI_DEVICE_ERROR; }
    if ((Csr & MU_C0_NAK) != 0) { *Result = EFI_USB_ERR_TIMEOUT | EFI_USB_ERR_NAK; return EFI_TIMEOUT; }
    if ((Interrupt & 1) != 0) { return EFI_SUCCESS; }
    if ((R8 (MU_DEVCTL) & (MU_DEV_HOST | MU_DEV_VBUS)) != (MU_DEV_HOST | MU_DEV_VBUS)) {
      *Result = EFI_USB_ERR_SYSTEM; return EFI_DEVICE_ERROR;
    }
    MicroSecondDelay (10);
    *BudgetUs -= 10;
  }
  *Result = EFI_USB_ERR_TIMEOUT;
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS EFIAPI Control (
  EFI_USB2_HC_PROTOCOL *This, UINT8 Address, UINT8 Speed, UINTN MaxPacket,
  EFI_USB_DEVICE_REQUEST *Request, EFI_USB_DATA_DIRECTION Direction,
  VOID *Data, UINTN *DataLength, UINTN Timeout,
  EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator, UINT32 *Result)
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
  W16 (MU_CSR0, MU_C0_SETUP | MU_C0_TXREADY);
  Status = WaitEp0 (&Budget, Result);
  if (EFI_ERROR (Status)) { goto Finish; }
  while (Done < Target) {
    if (Direction == EfiUsbDataIn) {
      W16 (MU_CSR0, MU_C0_REQ);
      Status = WaitEp0 (&Budget, Result);
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
      Status = WaitEp0 (&Budget, Result);
      if (EFI_ERROR (Status)) { goto Finish; }
      Done += Count;
    }
  }
  Csr = MU_C0_STATUS | MU_C0_NOPING;
  Csr |= (Direction == EfiUsbDataIn) ? MU_C0_TXREADY : MU_C0_REQ;
  W16 (MU_CSR0, Csr);
  Status = WaitEp0 (&Budget, Result);
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
  else { ++mDiagnostic.ControlCompleted; }
  EndIo (OldTpl);
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

STATIC EFI_STATUS EFIAPI AsyncInterrupt (
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

STATIC VOID PollInterruptPipe (UINTN Index) {
  UINT16 Csr;
  UINTN Count;
  UINT32 Result;
  EFI_ASYNC_USB_TRANSFER_CALLBACK Callback;
  VOID *Context;
  EFI_TPL OldTpl;
  UINT64 Generation;
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
  mDiagnostic.LastTransferResult = Result;
  if (Result != EFI_USB_NOERROR) { Record (MotoUsbHcStageInterruptError, EFI_DEVICE_ERROR); }
  else { ++mDiagnostic.InterruptReports; Record (MotoUsbHcStageInterruptReport, EFI_SUCCESS); }
  // Callback may cancel/recreate the pipe. Do not touch its state afterwards.
  gBS->RestoreTPL (OldTpl);
  if (Pipe->Active && (Pipe->Generation == Generation)) {
    Callback (Pipe->Buffer, Count, Context, Result);
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
  if (++mUsb.RoleTicks >= ROLE_POLL_TICKS) {
    mUsb.RoleTicks = 0;
    if (!mUsb.Ready) {
      Status = MotoG20UsbPlatformInit (&mUsb.Base);
      if (!EFI_ERROR (Status)) {
        OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
        mUsb.Busy = TRUE;
        Status = ConfigureController ();
        Record (MotoUsbHcStageController, Status);
        if (EFI_ERROR (Status)) {
          mUsb.Fatal = TRUE;
          // Identity/DMA qualification failures do not authorize endpoint or
          // session writes. Only undo controller state we configured ourselves.
          if (mDiagnostic.ControllerStep >= 3) { StopControllerHardware (); }
          mUsb.Ready = FALSE;
          (VOID)MotoG20UsbPlatformStop ();
        }
        mUsb.Busy = FALSE;
        gBS->RestoreTPL (OldTpl);
        if (EFI_ERROR (Status)) { return; }
      } else { Record (MotoUsbHcStageWaitRole, Status); return; }
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
  // Platform may already have shut the PHY down from its own monitor event.
  if (!EFI_ERROR (CheckRole ())) { StopControllerHardware (); }
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
