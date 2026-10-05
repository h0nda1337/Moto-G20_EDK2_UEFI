/** @file
  Report USB bring-up transitions after the tested physical console is ready.
  Counters denote completed bus operations, never successful keyboard input.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Guid/ConsoleInDevice.h>
#include <Guid/EventGroup.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/UsbIo.h>
#include <Protocol/MotoG20UsbDiagnostic.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC EFI_GUID mDiagnosticGuid = MOTO_G20_USB_DIAGNOSTIC_PROTOCOL_GUID;
STATIC EFI_EVENT mStatusTimer;
STATIC EFI_EVENT mExitEvent;
STATIC BOOLEAN mEnded;
STATIC BOOLEAN mHaveStatus;
STATIC UINTN mFaultKind;
STATIC EFI_STATUS mFaultStatus;
STATIC UINTN mTicks;
STATIC UINTN mLastReportTick;
STATIC MOTO_G20_USB_DIAGNOSTIC_STATUS mPrevious;

#define USB_INVENTORY_DETAILS 4U
#define USB_QUERY_IO          BIT0
#define USB_QUERY_DEVICE      BIT1
#define USB_QUERY_INTERFACE   BIT2
#define USB_QUERY_TEXT        BIT3
#define USB_QUERY_ROUTE       BIT4

typedef struct {
  UINTN Handle;
  UINT16 Vendor, Product;
  UINT8 Number, Class, SubClass, Protocol;
  UINT8 Valid, Errors;
  BOOLEAN Text, Routed;
  EFI_STATUS Query[5];
} USB_INTERFACE_OBSERVATION;

typedef struct {
  EFI_STATUS LocateStatus, ErrorStatus;
  UINTN Interfaces, Hubs, Hid, BootKeyboards, TextInputs, RoutedInputs;
  UINTN ErrorCount, DetailCount;
  UINT32 ErrorMask;
  UINT64 Fingerprint;
  USB_INTERFACE_OBSERVATION Detail[USB_INVENTORY_DETAILS];
} USB_INVENTORY;

STATIC USB_INVENTORY mPreviousInventory;

#define USB_FIRST_REPORT_TICKS 5U
#define USB_WAIT_REPORT_TICKS  30U

STATIC VOID
InventoryError (USB_INVENTORY *Inventory, USB_INTERFACE_OBSERVATION *Item,
                UINTN Query, EFI_STATUS Status)
{
  Item->Errors |= (UINT8)(1U << Query);
  Item->Query[Query] = Status;
  Inventory->ErrorMask |= 1U << Query;
  // A deterministic representative status avoids false changes when handle
  // enumeration order changes. The fingerprint retains every query status.
  if ((Inventory->ErrorCount++ == 0) || (Status < Inventory->ErrorStatus)) {
    Inventory->ErrorStatus = Status;
  }
}

STATIC VOID
ReadUsbInventory (USB_INVENTORY *Inventory)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0, Index, Entry, EntryCount, Byte, Position;
  EFI_USB_IO_PROTOCOL *UsbIo;
  EFI_USB_DEVICE_DESCRIPTOR Device;
  EFI_USB_INTERFACE_DESCRIPTOR Interface;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *Info;
  VOID *Text;
  USB_INTERFACE_OBSERVATION Item;
  UINT64 Hash;
  EFI_STATUS Status;

  ZeroMem (Inventory, sizeof (*Inventory));
  Inventory->LocateStatus = gBS->LocateHandleBuffer (ByProtocol, &gEfiUsbIoProtocolGuid,
                                                     NULL, &Count, &Handles);
  // NOT_FOUND is a successful observation of no published interfaces. Other
  // failures remain explicit, never indistinguishable from an empty bus.
  if (EFI_ERROR (Inventory->LocateStatus)) { return; }
  Inventory->Interfaces = Count;
  for (Index = 0; Index < Count; ++Index) {
    ZeroMem (&Item, sizeof (Item));
    Item.Handle = (UINTN)Handles[Index];
    UsbIo = NULL;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiUsbIoProtocolGuid, (VOID **)&UsbIo);
    if (!EFI_ERROR (Status) && (UsbIo == NULL)) { Status = EFI_PROTOCOL_ERROR; }
    Item.Query[0] = Status;
    if (EFI_ERROR (Status)) { InventoryError (Inventory, &Item, 0, Status); }
    else {
      // In this image's UsbBusDxe these two getters only CopyMem cached
      // descriptors. Do not substitute a USB control request here.
      Status = (UsbIo->UsbGetDeviceDescriptor == NULL) ? EFI_UNSUPPORTED :
               UsbIo->UsbGetDeviceDescriptor (UsbIo, &Device);
      Item.Query[1] = Status;
      if (EFI_ERROR (Status)) { InventoryError (Inventory, &Item, 1, Status); }
      else { Item.Valid |= BIT0; Item.Vendor = Device.IdVendor; Item.Product = Device.IdProduct; }
      Status = (UsbIo->UsbGetInterfaceDescriptor == NULL) ? EFI_UNSUPPORTED :
               UsbIo->UsbGetInterfaceDescriptor (UsbIo, &Interface);
      Item.Query[2] = Status;
      if (EFI_ERROR (Status)) { InventoryError (Inventory, &Item, 2, Status); }
      else {
        Item.Valid |= BIT1;
        Item.Number = Interface.InterfaceNumber; Item.Class = Interface.InterfaceClass;
        Item.SubClass = Interface.InterfaceSubClass; Item.Protocol = Interface.InterfaceProtocol;
        if (Item.Class == 9) { ++Inventory->Hubs; }
        if (Item.Class == 3) { ++Inventory->Hid; }
        if ((Item.Class == 3) && (Item.SubClass == 1) && (Item.Protocol == 1)) {
          ++Inventory->BootKeyboards;
        }
      }
    }
    Text = NULL;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleTextInProtocolGuid, &Text);
    if (!EFI_ERROR (Status) && (Text == NULL)) { Status = EFI_PROTOCOL_ERROR; }
    Item.Query[3] = Status;
    if (!EFI_ERROR (Status)) {
      Item.Text = TRUE; ++Inventory->TextInputs;
      Info = NULL; EntryCount = 0;
      Status = gBS->OpenProtocolInformation (Handles[Index], &gEfiConsoleInDeviceGuid,
                                            &Info, &EntryCount);
      Item.Query[4] = Status;
      if (!EFI_ERROR (Status)) {
        if ((EntryCount != 0) && (Info == NULL)) {
          InventoryError (Inventory, &Item, 4, EFI_PROTOCOL_ERROR);
        } else {
          for (Entry = 0; Entry < EntryCount; ++Entry) {
            if ((gST->ConsoleInHandle != NULL) &&
                ((Info[Entry].Attributes & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) != 0) &&
                (Info[Entry].ControllerHandle == gST->ConsoleInHandle)) {
              Item.Routed = TRUE; ++Inventory->RoutedInputs; break;
            }
          }
        }
      } else if (Status != EFI_NOT_FOUND) { InventoryError (Inventory, &Item, 4, Status); }
      if (Info != NULL) { FreePool (Info); }
    } else if (Status != EFI_UNSUPPORTED) { InventoryError (Inventory, &Item, 3, Status); }

    // Include every interface in the change detector, even when the bounded
    // detail list is full. XOR of per-handle records is enumeration-order
    // independent; zero-initialized padding makes record bytes deterministic.
    Hash = 0xcbf29ce484222325ULL;
    for (Byte = 0; Byte < sizeof (Item); ++Byte) {
      Hash = (Hash ^ ((UINT8 *)&Item)[Byte]) * 0x100000001b3ULL;
    }
    Inventory->Fingerprint ^= Hash;
    for (Position = 0; Position < Inventory->DetailCount; ++Position) {
      if (Item.Handle < Inventory->Detail[Position].Handle) { break; }
    }
    if (Position < USB_INVENTORY_DETAILS) {
      if (Inventory->DetailCount < USB_INVENTORY_DETAILS) { ++Inventory->DetailCount; }
      for (Entry = Inventory->DetailCount - 1; Entry > Position; --Entry) {
        CopyMem (&Inventory->Detail[Entry], &Inventory->Detail[Entry - 1], sizeof (Item));
      }
      CopyMem (&Inventory->Detail[Position], &Item, sizeof (Item));
    }
  }
  if (Handles != NULL) { FreePool (Handles); }
}

STATIC VOID EFIAPI
ReportDiagnosticFault (UINTN Kind, EFI_STATUS Status)
{
  // Every fault breaks the snapshot epoch. Recovery must print even if it
  // returns the same good state observed before the failure.
  mHaveStatus = FALSE;
  if ((mFaultKind == Kind) && (mFaultStatus == Status)) { return; }
  mFaultKind = Kind;
  mFaultStatus = Status;
  Print (L"\r\nUSB: diagnostic unavailable kind=%u status=%r; no current USB result.\r\n",
         (UINT32)Kind, Status);
}

STATIC BOOLEAN
WaitingForRole (CONST MOTO_G20_USB_DIAGNOSTIC_STATUS *Status)
{
  return !Status->HostReady && (Status->RecoveryWaiting || Status->SessionPending ||
         ((Status->Stage == MotoUsbHcStageWaitRole) &&
          (Status->Status == EFI_NOT_READY)));
}

STATIC VOID
NormalizeStatus (MOTO_G20_USB_DIAGNOSTIC_STATUS *Status)
{
  // Record first activity, never print on every key or timer sample. History
  // remains monotonic in the producer and the full counters are still shown.
  Status->ControlCompleted = Status->ControlCompleted != 0;
  Status->InterruptReports = Status->InterruptReports != 0;
  Status->ControlCalls = Status->ControlCalls != 0;
  Status->ControlFailures = Status->ControlFailures != 0;
  Status->AsyncFailures = Status->AsyncFailures != 0;
  Status->InterruptFailures = Status->InterruptFailures != 0;
  Status->ZeroLengthInterruptReports = Status->ZeroLengthInterruptReports != 0;
  if (!Status->LastFailure.Valid) { ZeroMem (&Status->LastFailure, sizeof (Status->LastFailure)); }
  else {
    if (!Status->LastFailure.RequestValid) {
      Status->LastFailure.RequestType = Status->LastFailure.Request = 0;
      Status->LastFailure.Value = Status->LastFailure.Index = Status->LastFailure.Length = 0;
    }
    if (!Status->LastFailure.ResultValid) { Status->LastFailure.Result = 0; }
    if (!Status->LastFailure.Ep0.Valid) {
      ZeroMem (&Status->LastFailure.Ep0, sizeof (Status->LastFailure.Ep0));
    } else {
      if (!(Status->LastFailure.Ep0.ValidMask & MOTO_G20_USB_EP0_CSR_VALID)) {
        Status->LastFailure.Ep0.Csr0 = 0;
      }
      if (!(Status->LastFailure.Ep0.ValidMask & MOTO_G20_USB_EP0_INTRTX_VALID)) {
        Status->LastFailure.Ep0.IntrTx = 0;
      }
      if (!(Status->LastFailure.Ep0.ValidMask & MOTO_G20_USB_EP0_DEVCTL_VALID)) {
        Status->LastFailure.Ep0.DevCtl = 0;
      }
      // These describe a completed failed operation, not live progress. Keep
      // exact values in the displayed snapshot, but timing-only variation in
      // otherwise identical repeated failures must not flood the console.
      Status->LastFailure.Ep0.Polls = Status->LastFailure.Ep0.DelayUs = 0;
    }
  }
  if (!Status->RootResetValid) { Status->RootResetDevCtl = Status->RootResetPower = 0; }
  if (!Status->LastInterrupt.Valid) { ZeroMem (&Status->LastInterrupt, sizeof (Status->LastInterrupt)); }
  else {
    // The last completion can alternate between devices and report sizes.
    // Those are displayed with the next real transition, not per key/report.
    Status->LastInterrupt.Address = Status->LastInterrupt.Endpoint = 0;
    Status->LastInterrupt.Length = Status->LastInterrupt.Length != 0;
    if (!Status->LastInterrupt.CallbackStatusValid) { Status->LastInterrupt.CallbackStatus = 0; }
  }
  Status->Role.Samples = 0;
  Status->Role.SourceSeen = Status->Role.SourceSeen != 0;
  Status->Role.BoostAttempts = Status->Role.BoostAttempts != 0;
  Status->Role.SnapshotAttempts = Status->Role.SnapshotAttempts != 0;
  Status->Role.SnapshotFailures = Status->Role.SnapshotFailures != 0;
  Status->Role.StopAttempts = Status->Role.StopAttempts != 0;
  if (Status->SessionPending) { Status->SessionWaitUs = 0; }
  if (!(Status->PeerSnapshotValidMask & MOTO_G20_USB_PEER_INITIAL_VALID)) {
    Status->SessionPowerInitial = Status->SessionTestModeInitial = 0;
  }
  if (!(Status->PeerSnapshotValidMask & MOTO_G20_USB_PEER_100MS_VALID)) {
    Status->PeerDevCtl100Ms = Status->PeerPower100Ms = Status->PeerOtg100Ms = 0;
  }
  if (!(Status->PeerSnapshotValidMask & MOTO_G20_USB_PEER_FIRST_HOST_VALID)) { Status->FirstHostUs = 0; }
  if (!(Status->PeerSnapshotValidMask & MOTO_G20_USB_PEER_FIRST_SPEED_VALID)) { Status->FirstSpeedUs = 0; }
  // SessionAttempts/Rearms are bounded event counters, not timer samples.
  Status->DetachStableMs = Status->DetachStableMs >= MOTO_G20_USB_RECOVERY_DETACH_MS;
  if (!Status->RecoverySampleValid) {
    Status->RecoveryTypecState = 0;
    Status->RecoveryVbus = FALSE;
  }
  if ((Status->SessionSnapshotValidMask & MOTO_G20_USB_SESSION_DEVCTL_BEFORE_VALID) == 0) {
    Status->SessionDevCtlBefore = 0;
  }
  if ((Status->SessionSnapshotValidMask & MOTO_G20_USB_SESSION_DEVCTL_START_VALID) == 0) {
    Status->SessionDevCtlStart = 0;
  }
  if ((Status->SessionSnapshotValidMask & MOTO_G20_USB_SESSION_DEVCTL_END_VALID) == 0) {
    Status->SessionDevCtlEnd = 0;
  }
  if ((Status->SessionSnapshotValidMask & MOTO_G20_USB_SESSION_POWER_VALID) == 0) {
    Status->SessionPower = 0;
  }
  if ((Status->SessionSnapshotValidMask & MOTO_G20_USB_SESSION_OTG_VALID) == 0) {
    Status->SessionOtg = 0;
  }
  if (WaitingForRole (Status)) {
    // DRP may cycle 9/3/0 forever. New SeenStates bits are useful history;
    // the instantaneous state and EIC input are not events. Keep RAW/MASK:
    // interrupt latch changes must remain reportable under the 30s throttle.
    Status->TypecState = 0;
    Status->VbusPresent = FALSE;
    Status->Role.TypecStatus = 0;
    Status->RecoveryTypecState = 0;
    Status->RecoveryVbus = FALSE;
    if ((Status->PlatformStage == MotoUsbStageTypec) ||
        (Status->PlatformStage == MotoUsbStageExternalVbus)) {
      Status->PlatformStage = MotoUsbStageTypec;
    }
  }
}

STATIC CONST CHAR16 *
CachedHex (UINT16 Value, BOOLEAN Valid, CHAR16 Buffer[5])
{
  STATIC CONST CHAR16 Digits[] = L"0123456789ABCDEF";
  UINTN Index;
  if (!Valid) { return L"----"; }
  for (Index = 0; Index < 4; ++Index) {
    Buffer[Index] = Digits[(Value >> (12 - 4 * Index)) & 15];
  }
  Buffer[4] = 0;
  return Buffer;
}

STATIC BOOLEAN
ImmediateChange (CONST MOTO_G20_USB_DIAGNOSTIC_STATUS *Current, CONST USB_INVENTORY *Inventory)
{
  return !mHaveStatus || !WaitingForRole (Current) ||
         (Current->Stage != mPrevious.Stage) || (Current->Status != mPrevious.Status) ||
         (Current->ControllerStep != mPrevious.ControllerStep) ||
         (Current->HostReady != mPrevious.HostReady) ||
         (Current->HcPublished != mPrevious.HcPublished) ||
         (Current->PortConnected != mPrevious.PortConnected) ||
         (Current->PortEnabled != mPrevious.PortEnabled) ||
         (Current->ControlCompleted != mPrevious.ControlCompleted) ||
         (Current->InterruptReports != mPrevious.InterruptReports) ||
         (Current->ControlCalls != mPrevious.ControlCalls) ||
         (Current->ControlFailures != mPrevious.ControlFailures) ||
         (Current->AsyncFailures != mPrevious.AsyncFailures) ||
         (Current->InterruptFailures != mPrevious.InterruptFailures) ||
         (Current->ZeroLengthInterruptReports != mPrevious.ZeroLengthInterruptReports) ||
         (Current->RootResetValid != mPrevious.RootResetValid) ||
         (Current->RootResetDevCtl != mPrevious.RootResetDevCtl) ||
         (Current->RootResetPower != mPrevious.RootResetPower) ||
         (CompareMem (&Current->LastFailure, &mPrevious.LastFailure, sizeof (Current->LastFailure)) != 0) ||
         (CompareMem (&Current->LastInterrupt, &mPrevious.LastInterrupt, sizeof (Current->LastInterrupt)) != 0) ||
         (Current->LastTransferResult != mPrevious.LastTransferResult) ||
         (Current->PlatformStatus != mPrevious.PlatformStatus) ||
         (Current->Role.RoleReadStatus != mPrevious.Role.RoleReadStatus) ||
         (Current->Role.SnapshotStatus != mPrevious.Role.SnapshotStatus) ||
         (Current->Role.CalibrationStatus != mPrevious.Role.CalibrationStatus) ||
         (Current->Role.ValidMask != mPrevious.Role.ValidMask) ||
         (Current->Role.ErrorMask != mPrevious.Role.ErrorMask) ||
         (Current->Role.DriverActive != mPrevious.Role.DriverActive) ||
         (Current->Role.BoostRequested != mPrevious.Role.BoostRequested) ||
         (Current->Role.LastStopStatus != mPrevious.Role.LastStopStatus) ||
         (Current->SessionAttempts != mPrevious.SessionAttempts) ||
         (Current->SessionLimit != mPrevious.SessionLimit) ||
         (Current->SessionSnapshotValidMask != mPrevious.SessionSnapshotValidMask) ||
         (Current->SessionStatus != mPrevious.SessionStatus) ||
         (Current->SessionPending != mPrevious.SessionPending) ||
         (Current->PeerSnapshotValidMask != mPrevious.PeerSnapshotValidMask) ||
         (Current->CleanupStatus != mPrevious.CleanupStatus) ||
         (Current->RecoveryProbeStatus != mPrevious.RecoveryProbeStatus) ||
         (Current->RecoveryWaiting != mPrevious.RecoveryWaiting) ||
         (Current->RecoveryExhausted != mPrevious.RecoveryExhausted) ||
         (Current->FatalLatched != mPrevious.FatalLatched) ||
         (Current->RecoverySampleValid != mPrevious.RecoverySampleValid) ||
         (Current->Rearms != mPrevious.Rearms) ||
         (CompareMem (Inventory, &mPreviousInventory, sizeof (*Inventory)) != 0);
}

STATIC VOID
PrintUsbInventory (CONST USB_INVENTORY *Inventory)
{
  UINTN Index;
  CONST USB_INTERFACE_OBSERVATION *Item;
  CHAR16 Hex[6][5];
  Print (L"USB: inventory locate=%r interfaces=%u hubs=%u HID=%u boot-kbd=%u\r\n",
         Inventory->LocateStatus, (UINT32)Inventory->Interfaces, (UINT32)Inventory->Hubs,
         (UINT32)Inventory->Hid, (UINT32)Inventory->BootKeyboards);
  Print (L"USB: inventory text=%u routed=%u errors=%u mask=%02x status=%r details=%u/%u\r\n",
         (UINT32)Inventory->TextInputs, (UINT32)Inventory->RoutedInputs,
         (UINT32)Inventory->ErrorCount, Inventory->ErrorMask, Inventory->ErrorStatus,
         (UINT32)Inventory->DetailCount, (UINT32)Inventory->Interfaces);
  for (Index = 0; Index < Inventory->DetailCount; ++Index) {
    Item = &Inventory->Detail[Index];
    Print (L"USB: if[%u] id=%s:%s n=%s cls/sub/proto=%s/%s/%s text=%u routed=%u err=%02x\r\n",
           (UINT32)Index, CachedHex (Item->Vendor, (Item->Valid & BIT0) != 0, Hex[0]),
           CachedHex (Item->Product, (Item->Valid & BIT0) != 0, Hex[1]),
           CachedHex (Item->Number, (Item->Valid & BIT1) != 0, Hex[2]),
           CachedHex (Item->Class, (Item->Valid & BIT1) != 0, Hex[3]),
           CachedHex (Item->SubClass, (Item->Valid & BIT1) != 0, Hex[4]),
           CachedHex (Item->Protocol, (Item->Valid & BIT1) != 0, Hex[5]),
           (UINT32)Item->Text, (UINT32)Item->Routed, (UINT32)Item->Errors);
  }
}

STATIC CONST CHAR16 *
CachedTime (UINT32 Value, BOOLEAN Valid, CHAR16 Buffer[11])
{
  UINTN Index = 10;
  if (!Valid) { return L"----"; }
  Buffer[Index] = 0;
  do { Buffer[--Index] = (CHAR16)(L'0' + Value % 10); Value /= 10; } while (Value != 0);
  return Buffer + Index;
}

STATIC CONST CHAR16 *
CachedResult (UINT32 Value, BOOLEAN Valid, CHAR16 Buffer[9])
{
  STATIC CONST CHAR16 Digits[] = L"0123456789ABCDEF";
  UINTN Index;
  if (!Valid) { return L"----"; }
  for (Index = 0; Index < 8; ++Index) { Buffer[Index] = Digits[(Value >> (28 - 4 * Index)) & 15]; }
  Buffer[8] = 0;
  return Buffer;
}

STATIC VOID
PrintEp0History (CONST MOTO_G20_USB_DIAGNOSTIC_STATUS *Current)
{
  CONST MOTO_G20_USB_EP0_TRACE *Ep0 = &Current->LastFailure.Ep0;
  CONST CHAR16 *Cause;
  CHAR16 Hex[3][5];
  if (!Current->LastFailure.Valid || !Ep0->Valid) {
    Print (L"USB: EP0 (failure history)=unavailable\r\n");
  } else {
    switch (Ep0->Cause) {
      case MOTO_G20_USB_EP0_CAUSE_NONE: Cause = L"no-Wait-failure"; break;
      case MOTO_G20_USB_EP0_CAUSE_ROLE_LOST: Cause = L"role-lost"; break;
      case MOTO_G20_USB_EP0_CAUSE_CSR_ERROR: Cause = L"CSR-error"; break;
      case MOTO_G20_USB_EP0_CAUSE_HOST_VBUS_LOST: Cause = L"host/VBUS-lost"; break;
      case MOTO_G20_USB_EP0_CAUSE_STALL: Cause = L"stall"; break;
      case MOTO_G20_USB_EP0_CAUSE_NAK: Cause = L"NAK"; break;
      case MOTO_G20_USB_EP0_CAUSE_TIMEOUT: Cause = L"poll-budget"; break;
      default: Cause = L"unknown"; break;
    }
    Print (L"USB: EP0 (failure history) cause=%u/%s access-lost=%u polls=%u delay-only-us=%u\r\n",
           (UINT32)Ep0->Cause, Cause, (UINT32)Ep0->HardwareAccessLost, Ep0->Polls, Ep0->DelayUs);
    Print (L"USB: EP0 cached valid=%02x CSR=%s IRQ=%s DEV=%s (historical)\r\n",
           (UINT32)Ep0->ValidMask,
           CachedHex (Ep0->Csr0, (Ep0->ValidMask & MOTO_G20_USB_EP0_CSR_VALID) != 0, Hex[0]),
           CachedHex (Ep0->IntrTx, (Ep0->ValidMask & MOTO_G20_USB_EP0_INTRTX_VALID) != 0, Hex[1]),
           CachedHex (Ep0->DevCtl, (Ep0->ValidMask & MOTO_G20_USB_EP0_DEVCTL_VALID) != 0, Hex[2]));
  }
  Print (L"USB: post-reset (history) valid=%u DEV=%s POWER=%s\r\n",
         (UINT32)Current->RootResetValid,
         CachedHex (Current->RootResetDevCtl, Current->RootResetValid, Hex[0]),
         CachedHex (Current->RootResetPower, Current->RootResetValid, Hex[1]));
}

STATIC VOID
PrintTransferHistory (CONST MOTO_G20_USB_DIAGNOSTIC_STATUS *Current)
{
  CONST MOTO_G20_USB_TRANSFER_FAILURE *Failure = &Current->LastFailure;
  CONST MOTO_G20_USB_INTERRUPT_OBSERVATION *Interrupt = &Current->LastInterrupt;
  CHAR16 Hex[5][5], Result[9];
  Print (L"USB: calls=%Lu control-fail=%Lu async-fail=%Lu IN-fail=%Lu zero-IN=%Lu\r\n",
         Current->ControlCalls, Current->ControlFailures, Current->AsyncFailures,
         Current->InterruptFailures, Current->ZeroLengthInterruptReports);
  if (!Failure->Valid) { Print (L"USB: last failure (history)=none\r\n"); }
  else {
    Print (L"USB: last failure (history) kind=%u phase=%u a=%u ep=%02x speed=%u via=%u/%u status=%r\r\n",
           (UINT32)Failure->Kind, (UINT32)Failure->Phase, (UINT32)Failure->Address,
           (UINT32)Failure->Endpoint, (UINT32)Failure->Speed,
           (UINT32)Failure->HubAddress, (UINT32)Failure->HubPort, Failure->Status);
    Print (L"USB: failed request valid=%u rt=%s req=%s value=%s index=%s len=%s result=%s\r\n",
           (UINT32)Failure->RequestValid,
           CachedHex (Failure->RequestType, Failure->RequestValid, Hex[0]),
           CachedHex (Failure->Request, Failure->RequestValid, Hex[1]),
           CachedHex (Failure->Value, Failure->RequestValid, Hex[2]),
           CachedHex (Failure->Index, Failure->RequestValid, Hex[3]),
           CachedHex (Failure->Length, Failure->RequestValid, Hex[4]),
           CachedResult (Failure->Result, Failure->ResultValid, Result));
  }
  if (!Interrupt->Valid) { Print (L"USB: last IN=none\r\n"); }
  else if (Interrupt->CallbackStatusValid) {
    Print (L"USB: last IN a=%u ep=%02x len=%Lu result=%08x callback=%r\r\n",
           (UINT32)Interrupt->Address, (UINT32)Interrupt->Endpoint, (UINT64)Interrupt->Length,
           Interrupt->Result, Interrupt->CallbackStatus);
  } else {
    Print (L"USB: last IN a=%u ep=%02x len=%Lu result=%08x callback=----\r\n",
           (UINT32)Interrupt->Address, (UINT32)Interrupt->Endpoint, (UINT64)Interrupt->Length,
           Interrupt->Result);
  }
}

STATIC VOID EFIAPI
ReportUsbStatus (EFI_EVENT Event, VOID *Context)
{
  MOTO_G20_USB_DIAGNOSTIC_PROTOCOL *Diagnostic;
  MOTO_G20_USB_DIAGNOSTIC_STATUS Current, Fingerprint;
  EFI_STATUS Status;
  USB_INVENTORY Inventory;
  CHAR16 Hex[20][5], Times[2][11];
  (VOID)Event; (VOID)Context;
  if (mEnded || (++mTicks < USB_FIRST_REPORT_TICKS) || (gST->ConOut == NULL)) { return; }
  Diagnostic = NULL;
  Status = gBS->LocateProtocol (&mDiagnosticGuid, NULL, (VOID **)&Diagnostic);
  if (EFI_ERROR (Status)) {
    ReportDiagnosticFault (1, Status);
    return;
  }
  if ((Diagnostic == NULL) || (Diagnostic->Revision != MOTO_G20_USB_DIAGNOSTIC_REVISION) ||
      (Diagnostic->GetStatus == NULL)) {
    ReportDiagnosticFault (2, EFI_INCOMPATIBLE_VERSION);
    return;
  }
  ZeroMem (&Current, sizeof (Current));
  Status = Diagnostic->GetStatus (Diagnostic, &Current);
  if (EFI_ERROR (Status)) {
    ReportDiagnosticFault (3, Status);
    return;
  }
  if ((Current.Revision != MOTO_G20_USB_DIAGNOSTIC_REVISION) || (Current.Size != sizeof (Current))) {
    ReportDiagnosticFault (4, EFI_INCOMPATIBLE_VERSION);
    return;
  }
  if ((Current.Role.Revision != MOTO_G20_USB_ROLE_DIAGNOSTIC_REVISION) ||
      (Current.Role.Size != sizeof (Current.Role))) {
    ReportDiagnosticFault (5, EFI_INCOMPATIBLE_VERSION);
    return;
  }
  // Full console rendering shares TPL_CALLBACK with the connection timer.
  // Preserve the producer's samples, but do not delay this short timing
  // experiment with a multi-line framebuffer report. Print its result later.
  if (Current.SessionPending) { return; }
  ReadUsbInventory (&Inventory);
  CopyMem (&Fingerprint, &Current, sizeof (Current));
  NormalizeStatus (&Fingerprint);
  if (mHaveStatus && (CompareMem (&Fingerprint, &mPrevious, sizeof (Fingerprint)) == 0) &&
      (CompareMem (&Inventory, &mPreviousInventory, sizeof (Inventory)) == 0)) { return; }
  if (!ImmediateChange (&Fingerprint, &Inventory) &&
      ((mTicks - mLastReportTick) < USB_WAIT_REPORT_TICKS)) { return; }
  CopyMem (&mPrevious, &Fingerprint, sizeof (mPrevious));
  CopyMem (&mPreviousInventory, &Inventory, sizeof (Inventory));
  mHaveStatus = TRUE;
  mFaultKind = 0;
  mLastReportTick = mTicks;
  Print (L"\r\nUSB: HC stage=%u step=%u status=%r ready=%u published=%u\r\n",
         (UINT32)Current.Stage, Current.ControllerStep, Current.Status,
         (UINT32)Current.HostReady, (UINT32)Current.HcPublished);
  Print (L"USB: platform stage=%u status=%r TypeC=%u EIC(last)=%u port=%u/%u\r\n",
         Current.PlatformStage, Current.PlatformStatus, (UINT32)Current.TypecState,
         (UINT32)Current.VbusPresent, (UINT32)Current.PortConnected, (UINT32)Current.PortEnabled);
  // Software ownership/status only; cached bits are not voltage measurements.
  Print (L"USB: driver active=%u boost-requested=%u stops=%Lu last-stop=%r\r\n",
         (UINT32)Current.Role.DriverActive, (UINT32)Current.Role.BoostRequested,
         Current.Role.StopAttempts, Current.Role.LastStopStatus);
  Print (L"USB: session attempt=%u retry=after-detach wait-us=%u status=%r cleanup=%r\r\n",
         Current.SessionAttempts, Current.SessionWaitUs,
         Current.SessionStatus, Current.CleanupStatus);
  Print (L"USB: HC cached-before-OFF valid=%02x DEVCTL=%s/%s/%s POWER=%s OTG=%s (---- invalid)\r\n",
         Current.SessionSnapshotValidMask,
         CachedHex (Current.SessionDevCtlBefore, (Current.SessionSnapshotValidMask & MOTO_G20_USB_SESSION_DEVCTL_BEFORE_VALID) != 0, Hex[8]),
         CachedHex (Current.SessionDevCtlStart, (Current.SessionSnapshotValidMask & MOTO_G20_USB_SESSION_DEVCTL_START_VALID) != 0, Hex[9]),
         CachedHex (Current.SessionDevCtlEnd, (Current.SessionSnapshotValidMask & MOTO_G20_USB_SESSION_DEVCTL_END_VALID) != 0, Hex[10]),
         CachedHex (Current.SessionPower, (Current.SessionSnapshotValidMask & MOTO_G20_USB_SESSION_POWER_VALID) != 0, Hex[11]),
         CachedHex (Current.SessionOtg, (Current.SessionSnapshotValidMask & MOTO_G20_USB_SESSION_OTG_VALID) != 0, Hex[12]));
  Print (L"USB: peer pending=%u deadline-us=%u valid=%02x first-HM-us=%s first-FSLS-us=%s\r\n",
         (UINT32)Current.SessionPending, Current.SessionDeadlineUs, Current.PeerSnapshotValidMask,
         CachedTime (Current.FirstHostUs, (Current.PeerSnapshotValidMask & MOTO_G20_USB_PEER_FIRST_HOST_VALID) != 0, Times[0]),
         CachedTime (Current.FirstSpeedUs, (Current.PeerSnapshotValidMask & MOTO_G20_USB_PEER_FIRST_SPEED_VALID) != 0, Times[1]));
  Print (L"USB: peer cached POWER0=%s TEST0=%s at100ms DEV=%s POWER=%s OTG=%s\r\n",
         CachedHex (Current.SessionPowerInitial, (Current.PeerSnapshotValidMask & MOTO_G20_USB_PEER_INITIAL_VALID) != 0, Hex[15]),
         CachedHex (Current.SessionTestModeInitial, (Current.PeerSnapshotValidMask & MOTO_G20_USB_PEER_INITIAL_VALID) != 0, Hex[16]),
         CachedHex (Current.PeerDevCtl100Ms, (Current.PeerSnapshotValidMask & MOTO_G20_USB_PEER_100MS_VALID) != 0, Hex[17]),
         CachedHex (Current.PeerPower100Ms, (Current.PeerSnapshotValidMask & MOTO_G20_USB_PEER_100MS_VALID) != 0, Hex[18]),
         CachedHex (Current.PeerOtg100Ms, (Current.PeerSnapshotValidMask & MOTO_G20_USB_PEER_100MS_VALID) != 0, Hex[19]));
  Print (L"USB: recovery waiting=%u fatal=%u rearms=%u detach-ms=%u probe=%r\r\n",
         (UINT32)Current.RecoveryWaiting,
         (UINT32)Current.FatalLatched, Current.Rearms, Current.DetachStableMs,
         Current.RecoveryProbeStatus);
  Print (L"USB: recovery sample valid=%u TypeC=%s EIC=%s (cached while OFF)\r\n",
         (UINT32)Current.RecoverySampleValid,
         CachedHex (Current.RecoveryTypecState, Current.RecoverySampleValid, Hex[13]),
         CachedHex ((UINT16)Current.RecoveryVbus, Current.RecoverySampleValid, Hex[14]));
  Print (L"USB: control=%Lu IN=%Lu result=%x\r\n",
         Current.ControlCompleted, Current.InterruptReports, Current.LastTransferResult);
  PrintTransferHistory (&Current);
  PrintEp0History (&Current);
  PrintUsbInventory (&Inventory);
  Print (L"USB: role samples=%Lu seen=%08x source=%Lu boost=%Lu read=%r\r\n",
         Current.Role.Samples, Current.Role.SeenStates, Current.Role.SourceSeen,
         Current.Role.BoostAttempts, Current.Role.RoleReadStatus);
  Print (L"USB: cached valid=%02x errors=%02x EN=%s MODE=%s IE=%s RAW=%s MASK=%s STATUS=%s (---- invalid)\r\n",
         Current.Role.ValidMask, Current.Role.ErrorMask,
         CachedHex (Current.Role.En, (Current.Role.ValidMask & MOTO_G20_USB_ROLE_EN_VALID) != 0, Hex[0]),
         CachedHex (Current.Role.Mode, (Current.Role.ValidMask & MOTO_G20_USB_ROLE_MODE_VALID) != 0, Hex[1]),
         CachedHex (Current.Role.IntEn, (Current.Role.ValidMask & MOTO_G20_USB_ROLE_INT_EN_VALID) != 0, Hex[2]),
         CachedHex (Current.Role.IntRaw, (Current.Role.ValidMask & MOTO_G20_USB_ROLE_INT_RAW_VALID) != 0, Hex[3]),
         CachedHex (Current.Role.IntMask, (Current.Role.ValidMask & MOTO_G20_USB_ROLE_INT_MASK_VALID) != 0, Hex[4]),
         CachedHex (Current.Role.TypecStatus, (Current.Role.ValidMask & MOTO_G20_USB_ROLE_STATUS_VALID) != 0, Hex[5]));
  Print (L"USB: snapshot=%Lu failed=%Lu status=%r; efuse=%s valid=%u trim=%s requested=%04x applied=%u cal=%r\r\n",
         Current.Role.SnapshotAttempts, Current.Role.SnapshotFailures, Current.Role.SnapshotStatus,
         CachedHex (Current.Role.EfuseData, Current.Role.EfuseValid, Hex[6]), (UINT32)Current.Role.EfuseValid,
         CachedHex (Current.Role.Trim, (Current.Role.ValidMask & MOTO_G20_USB_ROLE_TRIM_VALID) != 0, Hex[7]),
         (UINT32)Current.Role.RequestedTrim,
         (UINT32)Current.Role.TrimApplied, Current.Role.CalibrationStatus);
}

STATIC VOID EFIAPI
StopUsbStatus (EFI_EVENT Event, VOID *Context)
{
  (VOID)Event; (VOID)Context;
  mEnded = TRUE;
  if (mStatusTimer != NULL) {
    gBS->CloseEvent (mStatusTimer);
    mStatusTimer = NULL;
  }
}

EFI_STATUS EFIAPI
MotoG20StartUsbConsoleStatus (VOID)
{
  EFI_STATUS Status;
  if (mStatusTimer != NULL) { return EFI_ALREADY_STARTED; }
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, StopUsbStatus, NULL,
                              &gEfiEventExitBootServicesGuid, &mExitEvent);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                            ReportUsbStatus, NULL, &mStatusTimer);
  if (!EFI_ERROR (Status)) { Status = gBS->SetTimer (mStatusTimer, TimerPeriodic, 10000000); }
  if (EFI_ERROR (Status)) {
    if (mStatusTimer != NULL) { gBS->CloseEvent (mStatusTimer); mStatusTimer = NULL; }
    gBS->CloseEvent (mExitEvent);
    mExitEvent = NULL;
  }
  return Status;
}
