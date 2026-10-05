/* Host model of indexed registers and FIFO; includes the production driver. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#undef NULL
#include "../MotoG20MusbDxe.c"

EFI_GUID gEfiUsb2HcProtocolGuid = EFI_USB2_HC_PROTOCOL_GUID;
EFI_GUID gEfiDevicePathProtocolGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
EFI_GUID gEfiEventExitBootServicesGuid = EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS = &Services;
static unsigned char Reg[0x2000], EpReg[16][16], FifoReg[16][6], Fifo[16][2048];
static unsigned FifoPos[16], FifoLength[16], Sel, TxStage, PendingStage, RxSourcePos;
static unsigned char StageTxHub[32], StageRxHub[32], StageTxPort[32], StageRxPort[32];
static unsigned MmioCount, OffMmio, DelayUs, Callbacks, CallbackErrors, Installs, Connects;
static unsigned StallStage, TimeoutStage, OverflowStage;
static unsigned ErrorStage, NakStage;
static unsigned char Source[64], CallbackData[1024];
static unsigned SourceLength;
static BOOLEAN PowerActive, RoleAllowed, CancelInCallback;
static BOOLEAN CancelOnRestore, RecreateOnRestore;
static unsigned RoleLossAtUs;
static unsigned DevCtlLossAtUs;
static UINT8 DevCtlClearMask;
static EFI_STATUS PrepareStatus;
static EFI_STATUS ReportReturnStatus;
static EFI_TPL CurrentTpl;
static unsigned FifoConfigWrites, EndpointWrites;
static BOOLEAN CheckStoppedSession, BlockSessionStop;
static MOTO_G20_USB_ROLE_DIAGNOSTIC RoleDiagnostic;
static EFI_STATUS RoleDiagnosticStatus;
static unsigned RoleDiagnosticCalls;
static BOOLEAN DiagnosticNotifyPending;
static unsigned DiagnosticNotifyCalls;
static EFI_TPL PlatformGetterTpl, RoleGetterTpl;
static UINT64 CounterNs;
static EFI_STATUS DetectStatus, QuiesceStatus, StopStatus, InitStatus, PollStatus;
static UINT16 DetectState;
static BOOLEAN DetectVbus, GateOnInitRestore, GatePending;
static unsigned DetectCalls, InitCalls, QuiesceCalls, StopCalls;
static unsigned SessionWrites, SessionWriteSequence, ForceWriteSequence, WriteSequence;
static VOID EFIAPI Restore (EFI_TPL Old);

static unsigned char *RegPtr (UINTN Address) {
  unsigned Off = (unsigned)(Address - 0x5FFF0000U);
  assert (Off < sizeof (Reg));
  ++MmioCount;
  if (!PowerActive) { ++OffMmio; }
  if ((Off >= 0x10) && (Off < 0x20)) { return &EpReg[Sel][Off - 0x10]; }
  if ((Off >= MU_TXFIFOSZ) && (Off <= MU_RXFIFOADD + 1)) { return &FifoReg[Sel][Off - MU_TXFIFOSZ]; }
  return &Reg[Off];
}
UINT8 EFIAPI MmioRead8 (UINTN Address) {
  unsigned Off = (unsigned)(Address - 0x5FFF0000U);
  UINT8 Value;
  if ((Off >= MU_FIFO (0)) && (Off <= MU_FIFO (15)) && ((Off & 3) == 0)) {
    unsigned Ep = (Off - MU_FIFO (0)) / 4;
    (void)RegPtr (Address);
    assert (FifoPos[Ep] < FifoLength[Ep]);
    return Fifo[Ep][FifoPos[Ep]++];
  }
  Value = *RegPtr (Address);
  if (Off == MU_INTRUSB) { Reg[Off] = 0; }
  return Value;
}
static VOID AdvanceEp0 (VOID) {
  UINT16 Command = (UINT16)(EpReg[0][2] | (EpReg[0][3] << 8));
  unsigned Count;
  if (!PendingStage || (PendingStage == TimeoutStage)) { return; }
  if (PendingStage == StallStage) { EpReg[0][2] = MU_C0_STALL; EpReg[0][3] = 0; }
  else if (PendingStage == ErrorStage) { EpReg[0][2] = MU_C0_ERROR; EpReg[0][3] = 0; }
  else if (PendingStage == NakStage) { EpReg[0][2] = MU_C0_NAK; EpReg[0][3] = 0; }
  else if ((Command & MU_C0_REQ) && !(Command & MU_C0_STATUS)) {
    Count = SourceLength - RxSourcePos;
    if (Count > 8) { Count = 8; }
    if (PendingStage == OverflowStage) { Count = 65; }
    memset (Fifo[0], 0xEA, Count);
    if (Count <= 8) { memcpy (Fifo[0], Source + RxSourcePos, Count); RxSourcePos += Count; }
    FifoPos[0] = 0; FifoLength[0] = Count;
    EpReg[0][8] = (unsigned char)Count;
    EpReg[0][9] = 0;
    EpReg[0][2] = MU_C0_RXREADY;
    EpReg[0][3] = 0;
  } else {
    EpReg[0][2] = 0;
    EpReg[0][3] = 0;
    FifoLength[0] = FifoPos[0] = 0;
  }
  Reg[MU_INTRTX] |= 1;
  PendingStage = 0;
}
UINT16 EFIAPI MmioRead16 (UINTN Address) {
  UINT16 Value;
  unsigned Off = (unsigned)(Address - 0x5FFF0000U);
  unsigned char *Ptr;
  if (Off == MU_CSR0 && Sel == 0) { AdvanceEp0 (); }
  Ptr = RegPtr (Address);
  Value = (UINT16)(Ptr[0] | (Ptr[1] << 8));
  if ((Off == MU_INTRTX) || (Off == MU_INTRRX)) { Ptr[0] = Ptr[1] = 0; }
  return Value;
}
UINT32 EFIAPI MmioRead32 (UINTN Address) {
  UINT32 Value;
  memcpy (&Value, RegPtr (Address), 4);
  return Value;
}
UINT8 EFIAPI MmioWrite8 (UINTN Address, UINT8 Value) {
  unsigned Off = (unsigned)(Address - 0x5FFF0000U);
  ++WriteSequence;
  if (Off == MU_DEVCTL && (Value & MU_DEV_SESSION)) {
    ++SessionWrites; SessionWriteSequence = WriteSequence;
  }
  if (Off == MU_OTG_EXT_CSR && (Value & MU_HOST_FORCE)) {
    ForceWriteSequence = WriteSequence;
    assert (SessionWriteSequence && SessionWriteSequence < ForceWriteSequence);
  }
  if (BlockSessionStop && (Off == MU_DEVCTL || Off == MU_OTG_EXT_CSR)) { return Value; }
  if (Off >= MU_TXFIFOSZ && Off <= MU_RXFIFOADD + 1) {
    ++FifoConfigWrites;
    if (CheckStoppedSession) { assert (!(Reg[MU_DEVCTL] & MU_DEV_SESSION) && !(Reg[MU_OTG_EXT_CSR] & MU_HOST_FORCE)); }
  }
  if ((Off >= MU_FIFO (0)) && (Off <= MU_FIFO (15)) && ((Off & 3) == 0)) {
    unsigned Ep = (Off - MU_FIFO (0)) / 4;
    (void)RegPtr (Address);
    assert (FifoLength[Ep] < sizeof (Fifo[Ep]));
    Fifo[Ep][FifoLength[Ep]++] = Value;
  } else {
    *RegPtr (Address) = Value;
    if (Off == MU_INDEX) { assert (Value < 16); Sel = Value; }
  }
  return Value;
}
UINT16 EFIAPI MmioWrite16 (UINTN Address, UINT16 Value) {
  unsigned Off = (unsigned)(Address - 0x5FFF0000U);
  if (Off == MU_TXCSR || Off == MU_RXCSR) { ++EndpointWrites; }
  if (Off >= MU_TXFIFOSZ && Off <= MU_RXFIFOADD + 1) {
    ++FifoConfigWrites;
    if (CheckStoppedSession) { assert (!(Reg[MU_DEVCTL] & MU_DEV_SESSION) && !(Reg[MU_OTG_EXT_CSR] & MU_HOST_FORCE)); }
  }
  unsigned char *Ptr = RegPtr (Address);
  Ptr[0] = (unsigned char)Value; Ptr[1] = (unsigned char)(Value >> 8);
  if (Off == MU_CSR0 && Sel == 0) {
    if (Value & MU_C0_FLUSH) { FifoLength[0] = FifoPos[0] = 0; PendingStage = 0; }
    if (Value == 0) { FifoLength[0] = FifoPos[0] = 0; PendingStage = 0; }
    if (Value & (MU_C0_REQ | MU_C0_TXREADY)) {
      PendingStage = ++TxStage;
      assert (TxStage < sizeof (StageTxHub));
      StageTxHub[TxStage] = Reg[MU_TXHUB (0)]; StageRxHub[TxStage] = Reg[MU_RXHUB (0)];
      StageTxPort[TxStage] = Reg[MU_TXPORT (0)]; StageRxPort[TxStage] = Reg[MU_RXPORT (0)];
    }
  }
  if (Off == MU_RXCSR && Sel != 0 && (Value & MU_RX_FLUSH)) { FifoLength[Sel] = FifoPos[Sel] = 0; }
  return Value;
}
UINT32 EFIAPI MmioWrite32 (UINTN Address, UINT32 Value) {
  memcpy (RegPtr (Address), &Value, 4); return Value;
}
UINTN EFIAPI MicroSecondDelay (UINTN Us) {
  DelayUs += (unsigned)Us;
  CounterNs += (UINT64)Us * 1000ULL;
  if (RoleLossAtUs && DelayUs >= RoleLossAtUs) { RoleAllowed = FALSE; }
  if (DevCtlLossAtUs && DelayUs >= DevCtlLossAtUs) { Reg[MU_DEVCTL] &= (UINT8)~DevCtlClearMask; }
  return Us;
}
UINT64 EFIAPI GetPerformanceCounter (VOID) { return CounterNs; }
UINT64 EFIAPI GetTimeInNanoSecond (UINT64 Ticks) { return Ticks; }
VOID *EFIAPI ZeroMem (VOID *Buffer, UINTN Length) { return memset (Buffer, 0, Length); }
VOID *EFIAPI CopyMem (VOID *Dest, CONST VOID *SourceBuffer, UINTN Length) { return memcpy (Dest, SourceBuffer, Length); }

EFI_STATUS EFIAPI MotoG20UsbPlatformPrepare (VOID) { return PrepareStatus; }
EFI_STATUS EFIAPI MotoG20UsbPlatformInit (UINTN *Base) {
  EFI_TPL OldTpl;
  ++InitCalls;
  if (EFI_ERROR (InitStatus)) { *Base = 0; return InitStatus; }
  if (!RoleAllowed) { *Base = 0; return EFI_NOT_READY; }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  PowerActive = TRUE; *Base = 0x5FFF0000U;
  if (GateOnInitRestore) { GatePending = TRUE; }
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI MotoG20UsbPlatformPollRole (BOOLEAN *Active) {
  if (EFI_ERROR (PollStatus)) { *Active = FALSE; PowerActive = FALSE; return PollStatus; }
  *Active = PowerActive && RoleAllowed;
  if (!*Active) { PowerActive = FALSE; return EFI_NOT_READY; }
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI MotoG20UsbPlatformStop (VOID) { ++StopCalls; PowerActive = FALSE; return StopStatus; }
EFI_STATUS EFIAPI MotoG20UsbPlatformQuiesceForRetry (VOID) { ++QuiesceCalls; PowerActive = FALSE; return QuiesceStatus; }
EFI_STATUS EFIAPI MotoG20UsbPlatformDetectRole (UINT16 *State, BOOLEAN *Vbus) {
  assert (!PowerActive); ++DetectCalls;
  if (EFI_ERROR (DetectStatus)) { return DetectStatus; }
  *State = DetectState; *Vbus = DetectVbus; return EFI_SUCCESS;
}
EFI_STATUS EFIAPI MotoG20UsbPlatformShutdown (VOID) { PowerActive = FALSE; return EFI_SUCCESS; }
static VOID DispatchDiagnosticNotify (VOID) {
  if (DiagnosticNotifyPending && CurrentTpl < TPL_NOTIFY) {
    DiagnosticNotifyPending = FALSE;
    ++DiagnosticNotifyCalls;
    mDiagnostic.ControlCompleted = 82; mDiagnostic.Status = EFI_NOT_READY;
    mUsb.Ready = RoleAllowed = PowerActive = FALSE;
    RoleDiagnostic.Samples = 82; RoleDiagnostic.RoleReadStatus = EFI_NOT_READY;
    RoleDiagnostic.TypecStatus = 2;
  }
}
VOID EFIAPI MotoG20UsbPlatformGetDiagnostic (MOTO_G20_USB_PLATFORM_STAGE *Stage, EFI_STATUS *Status, UINT16 *State, BOOLEAN *Vbus) {
  PlatformGetterTpl = CurrentTpl;
  DispatchDiagnosticNotify ();
  *Stage = MotoUsbStageRunning; *Status = EFI_SUCCESS; *State = RoleAllowed ? 5 : 2; *Vbus = PowerActive;
}
EFI_STATUS EFIAPI MotoG20UsbPlatformGetRoleDiagnostic (MOTO_G20_USB_ROLE_DIAGNOSTIC *Role) {
  EFI_TPL OldTpl;
  RoleGetterTpl = CurrentTpl;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  ++RoleDiagnosticCalls;
  memcpy (Role, &RoleDiagnostic, sizeof (*Role));
  gBS->RestoreTPL (OldTpl);
  return RoleDiagnosticStatus;
}
static EFI_TPL EFIAPI Raise (EFI_TPL New) { EFI_TPL Old = CurrentTpl; assert (New >= Old); CurrentTpl = New; return Old; }
static EFI_STATUS EFIAPI OnReport (VOID *Data, UINTN Length, VOID *Context, UINT32 Result);
static VOID EFIAPI Restore (EFI_TPL Old) {
  CurrentTpl = Old;
  if (GatePending && CurrentTpl < TPL_NOTIFY) {
    GatePending = FALSE; PowerActive = RoleAllowed = FALSE;
  }
  DispatchDiagnosticNotify ();
  if (CancelOnRestore && Old == TPL_CALLBACK) {
    UINT8 Toggle = 0;
    CancelOnRestore = FALSE;
    CurrentTpl = TPL_NOTIFY;
    assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, FALSE, &Toggle, 0, 0, NULL, NULL, NULL) == EFI_SUCCESS);
    if (RecreateOnRestore) {
      Toggle = 0;
      assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, TRUE, &Toggle, 1, 8, NULL, OnReport, NULL) == EFI_SUCCESS);
    }
    CurrentTpl = Old;
  }
}
static EFI_STATUS EFIAPI Create (UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Notify, VOID *Context, EFI_EVENT *Event) {
  *Event = (EFI_EVENT)(UINTN)1; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CreateEx (UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Notify, CONST VOID *Context, CONST EFI_GUID *Group, EFI_EVENT *Event) {
  *Event = (EFI_EVENT)(UINTN)2; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close (EFI_EVENT Event) { return EFI_SUCCESS; }
static EFI_STATUS EFIAPI SetTimerMock (EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 Trigger) { return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Install (EFI_HANDLE *Handle, ...) { ++Installs; *Handle = (EFI_HANDLE)(UINTN)Installs; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Connect (EFI_HANDLE Handle, EFI_HANDLE *Image, EFI_DEVICE_PATH_PROTOCOL *Path, BOOLEAN Recursive) { ++Connects; return EFI_SUCCESS; }

static VOID ResetModel (VOID) {
  memset (&mUsb, 0, sizeof (mUsb)); memset (&mDiagnostic, 0, sizeof (mDiagnostic));
  memset (Reg, 0, sizeof (Reg)); memset (EpReg, 0, sizeof (EpReg));
  memset (FifoReg, 0, sizeof (FifoReg));
  memset (Fifo, 0, sizeof (Fifo)); memset (FifoPos, 0, sizeof (FifoPos)); memset (FifoLength, 0, sizeof (FifoLength));
  Sel = TxStage = PendingStage = RxSourcePos = 0;
  MmioCount = OffMmio = DelayUs = Callbacks = CallbackErrors = Installs = Connects = 0;
  StallStage = TimeoutStage = OverflowStage = 0;
  ErrorStage = NakStage = DevCtlLossAtUs = 0; DevCtlClearMask = 0;
  PowerActive = RoleAllowed = TRUE; CancelInCallback = FALSE; PrepareStatus = EFI_SUCCESS;
  ReportReturnStatus = EFI_SUCCESS;
  CancelOnRestore = RecreateOnRestore = FALSE; RoleLossAtUs = 0;
  CurrentTpl = TPL_APPLICATION;
  FifoConfigWrites = EndpointWrites = 0;
  CheckStoppedSession = BlockSessionStop = FALSE;
  memset (&RoleDiagnostic, 0, sizeof (RoleDiagnostic));
  RoleDiagnostic.Revision = MOTO_G20_USB_ROLE_DIAGNOSTIC_REVISION;
  RoleDiagnostic.Size = sizeof (RoleDiagnostic);
  RoleDiagnosticStatus = EFI_SUCCESS;
  RoleDiagnosticCalls = 0;
  DiagnosticNotifyPending = FALSE; DiagnosticNotifyCalls = 0;
  PlatformGetterTpl = RoleGetterTpl = 0;
  CounterNs = 0;
  DetectStatus = QuiesceStatus = StopStatus = InitStatus = PollStatus = EFI_SUCCESS;
  DetectState = 5; DetectVbus = FALSE; GateOnInitRestore = GatePending = FALSE;
  DetectCalls = InitCalls = QuiesceCalls = StopCalls = 0;
  SessionWrites = SessionWriteSequence = ForceWriteSequence = WriteSequence = 0;
  mUsb.Base = 0x5FFF0000U; mUsb.Ready = TRUE; mUsb.Enabled = TRUE; mUsb.Connected = TRUE;
  mUsb.State = EfiUsbHcStateOperational;
  Reg[MU_DEVCTL] = MU_DEV_SESSION | MU_DEV_HOST | MU_DEV_VBUS | MU_DEV_FS;
  Reg[MU_HWVERS] = 0x90; Reg[MU_HWVERS + 1] = 0x01;
  EpReg[0][15] = MU_CFG_DYNFIFO;
  SourceLength = 18;
  for (unsigned I = 0; I < SourceLength; ++I) { Source[I] = (unsigned char)(I + 1); }
  Services.RaiseTPL = Raise; Services.RestoreTPL = Restore;
  Services.CreateEvent = Create; Services.CreateEventEx = CreateEx;
  Services.CloseEvent = Close; Services.SetTimer = SetTimerMock;
  Services.InstallMultipleProtocolInterfaces = Install;
  Services.ConnectController = Connect;
}
// One scheduled 100 ms role poll, with wall time independent of callback count.
static VOID RoleCycle (VOID) {
  CounterNs += 100000000ULL;
  mUsb.RoleTicks = ROLE_POLL_TICKS - 1;
  TimerTick (NULL, NULL);
}
static VOID StartTimeout (UINT8 Devctl) {
  ResetModel (); CurrentTpl = TPL_CALLBACK; PowerActive = FALSE;
  Reg[MU_DEVCTL] = Devctl;
  assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS);
  RoleCycle ();
  if (mUsb.SessionPending) {
    CounterNs = mUsb.SessionSince + 1100000000ULL;
    TimerTick (NULL, NULL);
  }
  assert (mUsb.WaitingDetach && !mUsb.Fatal && !mUsb.Ready && !mUsb.Published);
  assert (!PowerActive && OffMmio == 0 && mDiagnostic.SessionAttempts == 1);
}
static EFI_STATUS EFIAPI OnReport (VOID *Data, UINTN Length, VOID *Context, UINT32 Result) {
  UINT8 Toggle = 0;
  ++Callbacks;
  if (Result) { ++CallbackErrors; }
  if (Length) { memcpy (CallbackData, Data, Length); }
  if (CancelInCallback) {
    assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, FALSE, &Toggle, 0, 0, NULL, NULL, NULL) == EFI_SUCCESS);
    assert (Toggle == 1);
  }
  return ReportReturnStatus;
}
static VOID InjectReportTo (unsigned Ep, unsigned Length, unsigned char Seed) {
  EpReg[Ep][6] = MU_RX_READY;
  EpReg[Ep][7] = 0;
  EpReg[Ep][8] = (unsigned char)Length; EpReg[Ep][9] = (unsigned char)(Length >> 8);
  FifoPos[Ep] = 0; FifoLength[Ep] = Length;
  for (unsigned I = 0; I < Length; ++I) { Fifo[Ep][I] = (unsigned char)(Seed + I); }
}
static VOID InjectReport (unsigned Length) {
  InjectReportTo (1, Length, 0xA0);
}
typedef struct {
  unsigned Reports;
  unsigned Errors;
  UINTN Length;
  UINT8 Data[64];
} TRACKED_REPORT;
static EFI_STATUS EFIAPI OnTrackedReport (VOID *Data, UINTN Length, VOID *Context, UINT32 Result) {
  TRACKED_REPORT *Report = Context;
  ++Report->Reports;
  if (Result) { ++Report->Errors; }
  Report->Length = Length;
  assert (Length <= sizeof (Report->Data));
  memcpy (Report->Data, Data, Length);
  return EFI_SUCCESS;
}

static VOID TestV5Diagnostics (VOID) {
  EFI_USB_DEVICE_REQUEST Request = { 0x80, 6, 0x100, 7, 8 };
  EFI_USB2_HC_TRANSACTION_TRANSLATOR Translator = { 2, 1 };
  MOTO_G20_USB_TRANSFER_FAILURE Failure;
  MOTO_G20_USB_DIAGNOSTIC_STATUS Diagnostic;
  UINT8 Data[8], Toggle;
  UINTN Length;
  UINT32 Result;
  unsigned Before;

  for (unsigned Phase = 1; Phase <= 3; ++Phase) {
    ResetModel (); SourceLength = 8; StallStage = Phase; Length = sizeof (Data);
    assert (Control (&mUsb.Hc, 3, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn,
                     Data, &Length, 100, &Translator, &Result) == EFI_DEVICE_ERROR);
    Failure = mDiagnostic.LastFailure;
    assert (Failure.Valid && Failure.Kind == 1 && Failure.Phase == Phase);
    assert (Failure.Ep0.Valid && Failure.Ep0.Cause == MOTO_G20_USB_EP0_CAUSE_STALL);
    assert (Failure.Ep0.Csr0 == MU_C0_STALL && Failure.Ep0.ValidMask == 3);
    assert (Failure.Address == 3 && Failure.Endpoint == 0 && Failure.Speed == EFI_USB_SPEED_FULL);
    assert (Failure.HubAddress == 2 && Failure.HubPort == 1 && Failure.RequestValid);
    assert (Failure.RequestType == 0x80 && Failure.Request == 6 && Failure.Value == 0x100 && Failure.Index == 7 && Failure.Length == 8);
    assert (Failure.Status == EFI_DEVICE_ERROR && Failure.ResultValid && Failure.Result == EFI_USB_ERR_STALL);
    assert (mDiagnostic.ControlCalls == 1 && mDiagnostic.ControlFailures == 1 && !mDiagnostic.ControlCompleted);
    StallStage = TxStage = RxSourcePos = 0; Length = sizeof (Data);
    assert (Control (&mUsb.Hc, 3, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn,
                     Data, &Length, 100, &Translator, &Result) == EFI_SUCCESS);
    assert (!memcmp (&Failure, &mDiagnostic.LastFailure, sizeof (Failure)));
    assert (mDiagnostic.ControlCalls == 2 && mDiagnostic.ControlFailures == 1 && mDiagnostic.ControlCompleted == 1);
    Toggle = 0;
    assert (AsyncInterrupt (&mUsb.Hc, 2, 0x81, EFI_USB_SPEED_HIGH, 1, TRUE,
                           &Toggle, 1, 1, NULL, OnReport, NULL) == EFI_SUCCESS);
    InjectReport (1); PollInterrupt ();
    assert (!memcmp (&Failure, &mDiagnostic.LastFailure, sizeof (Failure)));
    assert (mDiagnostic.LastTransferResult == EFI_USB_NOERROR && mDiagnostic.Stage == MotoUsbHcStageInterruptReport);
  }
  puts ("PASS control setup/data/status failures retain request and TT evidence across later control and IN success");

  ResetModel (); Length = sizeof (Data); Before = MmioCount;
  assert (Control (&mUsb.Hc, 3, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn,
                   Data, &Length, 100, &Translator, NULL) == EFI_INVALID_PARAMETER);
  assert (MmioCount == Before && mDiagnostic.LastFailure.Valid && !mDiagnostic.LastFailure.ResultValid);
  assert (mDiagnostic.LastFailure.Phase == 0 && mDiagnostic.LastFailure.RequestValid && mDiagnostic.ControlFailures == 1);
  assert (Control (&mUsb.Hc, 3, EFI_USB_SPEED_FULL, 8, NULL, EfiUsbDataIn,
                   Data, &Length, 100, &Translator, &Result) == EFI_INVALID_PARAMETER);
  assert (MmioCount == Before && !mDiagnostic.LastFailure.RequestValid && mDiagnostic.LastFailure.ResultValid);
  assert (mDiagnostic.LastFailure.Result == EFI_USB_ERR_NOTEXECUTE);
  mUsb.Busy = TRUE;
  assert (Control (&mUsb.Hc, 3, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn,
                   Data, &Length, 100, &Translator, &Result) == EFI_NOT_READY);
  assert (MmioCount == Before && mDiagnostic.LastFailure.Phase == 0 && mDiagnostic.LastFailure.Status == EFI_NOT_READY);
  mUsb.Busy = FALSE; Length = 7;
  assert (Control (&mUsb.Hc, 3, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn,
                   Data, &Length, 100, &Translator, &Result) == EFI_INVALID_PARAMETER);
  assert (MmioCount == Before && mDiagnostic.ControlCalls == 4 && mDiagnostic.ControlFailures == 4);
  Failure = mDiagnostic.LastFailure;
  assert (Control (NULL, 3, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn,
                   Data, &Length, 100, &Translator, &Result) == EFI_INVALID_PARAMETER);
  assert (MmioCount == Before && mDiagnostic.ControlCalls == 4 && !memcmp (&Failure, &mDiagnostic.LastFailure, sizeof (Failure)));
  puts ("PASS rejected control calls and NULL Result record phase zero without MMIO; foreign protocol cannot alter evidence");

  ResetModel (); Before = MmioCount; Toggle = 0;
  assert (AsyncInterrupt (&mUsb.Hc, 3, 0x81, EFI_USB_SPEED_FULL, 8, TRUE,
                         NULL, 1, 8, &Translator, OnReport, NULL) == EFI_INVALID_PARAMETER);
  assert (MmioCount == Before && mDiagnostic.LastFailure.Kind == 2 && !mDiagnostic.LastFailure.ResultValid);
  assert (AsyncInterrupt (&mUsb.Hc, 3, 0x81, EFI_USB_SPEED_HIGH, 512, TRUE,
                         &Toggle, 1, 8, NULL, OnReport, NULL) == EFI_UNSUPPORTED);
  assert (MmioCount == Before && mDiagnostic.LastFailure.Status == EFI_UNSUPPORTED);
  assert (AsyncInterrupt (&mUsb.Hc, 3, 0x81, EFI_USB_SPEED_FULL, 8, FALSE,
                         &Toggle, 0, 0, &Translator, NULL, NULL) == EFI_INVALID_PARAMETER);
  assert (MmioCount == Before && mDiagnostic.LastFailure.Kind == 4 && mDiagnostic.AsyncFailures == 3);
  Failure = mDiagnostic.LastFailure;
  assert (AsyncInterrupt (&mUsb.Hc, 3, 0x81, EFI_USB_SPEED_FULL, 8, TRUE,
                         &Toggle, 1, 8, &Translator, OnReport, NULL) == EFI_SUCCESS);
  assert (!memcmp (&Failure, &mDiagnostic.LastFailure, sizeof (Failure)));
  puts ("PASS async validation and cancellation failures are persistent, typed, and perform no HC access");

  EpReg[1][6] = MU_RX_STALL; PollInterrupt ();
  Failure = mDiagnostic.LastFailure;
  assert (Failure.Valid && Failure.Kind == 3 && !Failure.RequestValid && Failure.ResultValid);
  assert (Failure.Address == 3 && Failure.Endpoint == 0x81 && Failure.HubAddress == 2 && Failure.HubPort == 1);
  assert (Failure.Result == EFI_USB_ERR_STALL && mDiagnostic.InterruptFailures == 1);
  assert (mDiagnostic.LastInterrupt.Valid && mDiagnostic.LastInterrupt.Result == EFI_USB_ERR_STALL);
  assert (mDiagnostic.LastInterrupt.Length == 0 && mDiagnostic.LastInterrupt.CallbackStatusValid);
  assert (mDiagnostic.LastInterrupt.CallbackStatus == EFI_SUCCESS && !mDiagnostic.ZeroLengthInterruptReports);
  PollInterrupt (); InjectReport (8); PollInterrupt ();
  assert (!memcmp (&Failure, &mDiagnostic.LastFailure, sizeof (Failure)) && mDiagnostic.InterruptReports == 1);
  assert (mDiagnostic.LastInterrupt.Length == 8 && mDiagnostic.LastInterrupt.Result == EFI_USB_NOERROR);
  PowerActive = FALSE; mUsb.Ready = FALSE; Before = MmioCount;
  assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_SUCCESS);
  assert (MmioCount == Before && !OffMmio && Diagnostic.Revision == 6);
  assert (!memcmp (&Failure, &Diagnostic.LastFailure, sizeof (Failure)) && Diagnostic.LastInterrupt.CallbackStatusValid);
  puts ("PASS IN error keeps endpoint/TT evidence after success; getter copies all V5 diagnostics with power off and no MMIO");

  ResetModel (); Toggle = 0; ReportReturnStatus = EFI_OUT_OF_RESOURCES;
  assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, TRUE,
                         &Toggle, 1, 8, NULL, OnReport, NULL) == EFI_SUCCESS);
  assert (!mDiagnostic.LastInterrupt.Valid);
  InjectReport (0); PollInterrupt ();
  assert (mDiagnostic.LastInterrupt.Valid && mDiagnostic.LastInterrupt.Address == 1 && mDiagnostic.LastInterrupt.Endpoint == 0x81);
  assert (!mDiagnostic.LastInterrupt.Length && !mDiagnostic.LastInterrupt.Result && mDiagnostic.ZeroLengthInterruptReports == 1);
  assert (mDiagnostic.InterruptReports == 1 && !mDiagnostic.InterruptFailures && !mDiagnostic.LastFailure.Valid);
  assert (mDiagnostic.LastInterrupt.CallbackStatusValid && mDiagnostic.LastInterrupt.CallbackStatus == EFI_OUT_OF_RESOURCES);
  puts ("PASS zero-length IN success is distinct from payload delivery and callback failure is reported without becoming a bus failure");

  ResetModel (); CurrentTpl = TPL_CALLBACK; Toggle = 0;
  assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, TRUE,
                         &Toggle, 1, 8, NULL, OnReport, NULL) == EFI_SUCCESS);
  InjectReport (8); CancelOnRestore = RecreateOnRestore = TRUE; PollInterrupt ();
  assert (!Callbacks && mDiagnostic.LastInterrupt.Valid && !mDiagnostic.LastInterrupt.CallbackStatusValid);
  ReportReturnStatus = EFI_DEVICE_ERROR; InjectReport (8); PollInterrupt ();
  assert (Callbacks == 1 && mDiagnostic.LastInterrupt.CallbackStatusValid && mDiagnostic.LastInterrupt.CallbackStatus == EFI_DEVICE_ERROR);
  ResetModel (); Toggle = 0; CancelInCallback = TRUE;
  assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, TRUE,
                         &Toggle, 1, 8, NULL, OnReport, NULL) == EFI_SUCCESS);
  InjectReport (8); PollInterrupt ();
  assert (!mUsb.Pipes[0].Active && mDiagnostic.LastInterrupt.CallbackStatusValid && !mDiagnostic.LastInterrupt.CallbackStatus);
  puts ("PASS callback validity distinguishes cancellation before delivery from self-cancellation after delivery");

  StartTimeout (0); mDiagnostic.SessionAttempts = mDiagnostic.Rearms = 0xFFFFFFFFU; DetectState = 0;
  for (unsigned I = 0; I < 6; ++I) { RoleCycle (); }
  assert (!mUsb.WaitingDetach && mDiagnostic.Rearms == 0xFFFFFFFFU);
  Reg[MU_DEVCTL] = 0; RoleCycle ();
  assert (mDiagnostic.SessionAttempts == 0xFFFFFFFFU && SessionWrites == 2 && mUsb.WaitingDetach);
  assert (!mUsb.Fatal && !mDiagnostic.RecoveryExhausted && !PowerActive && !OffMmio);
  puts ("PASS saturated attempt/rearm counters do not wrap or block a qualified retry");

  ResetModel (); SourceLength = 8; mDiagnostic.ControlCalls = mDiagnostic.ControlCompleted = 0xFFFFFFFFFFFFFFFEULL;
  for (unsigned I = 0; I < 2; ++I) {
    TxStage = RxSourcePos = 0; Length = sizeof (Data);
    assert (Control (&mUsb.Hc, 3, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn,
                     Data, &Length, 100, &Translator, &Result) == EFI_SUCCESS);
  }
  assert (mDiagnostic.ControlCalls == 0xFFFFFFFFFFFFFFFFULL && mDiagnostic.ControlCompleted == 0xFFFFFFFFFFFFFFFFULL);
  mDiagnostic.ControlFailures = mDiagnostic.AsyncFailures = 0xFFFFFFFFFFFFFFFEULL;
  for (unsigned I = 0; I < 2; ++I) {
    assert (Control (&mUsb.Hc, 3, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn,
                     Data, &Length, 100, &Translator, NULL) == EFI_INVALID_PARAMETER);
    assert (AsyncInterrupt (&mUsb.Hc, 3, 0x81, EFI_USB_SPEED_FULL, 8, TRUE,
                           NULL, 1, 8, &Translator, OnReport, NULL) == EFI_INVALID_PARAMETER);
  }
  assert (mDiagnostic.ControlFailures == 0xFFFFFFFFFFFFFFFFULL && mDiagnostic.AsyncFailures == 0xFFFFFFFFFFFFFFFFULL);
  mDiagnostic.InterruptReports = mDiagnostic.ZeroLengthInterruptReports = mDiagnostic.InterruptFailures = 0xFFFFFFFFFFFFFFFEULL;
  Toggle = 0;
  assert (AsyncInterrupt (&mUsb.Hc, 3, 0x81, EFI_USB_SPEED_FULL, 8, TRUE,
                         &Toggle, 1, 8, &Translator, OnReport, NULL) == EFI_SUCCESS);
  for (unsigned I = 0; I < 2; ++I) { InjectReport (0); PollInterrupt (); PollInterrupt (); }
  for (unsigned I = 0; I < 2; ++I) { EpReg[1][6] = MU_RX_STALL; PollInterrupt (); PollInterrupt (); }
  assert (mDiagnostic.InterruptReports == 0xFFFFFFFFFFFFFFFFULL && mDiagnostic.ZeroLengthInterruptReports == 0xFFFFFFFFFFFFFFFFULL);
  assert (mDiagnostic.InterruptFailures == 0xFFFFFFFFFFFFFFFFULL);
  puts ("PASS persistent transfer counters saturate across real success and error paths without wrapping");
}

static VOID TestV6Ep0Trace (VOID) {
  EFI_USB_DEVICE_REQUEST Request = { 0, 5, 1, 0, 0 };
  MOTO_G20_USB_TRANSFER_FAILURE Failure;
  MOTO_G20_USB_DIAGNOSTIC_STATUS Diagnostic;
  UINT32 Result;
  unsigned Before;

  ResetModel (); ErrorStage = 1;
  assert (Control (&mUsb.Hc, 0, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbNoData,
                   NULL, NULL, 100, NULL, &Result) == EFI_DEVICE_ERROR);
  Failure = mDiagnostic.LastFailure;
  assert (Result == EFI_USB_ERR_SYSTEM && Failure.Phase == 1 && Failure.Ep0.Valid);
  assert (Failure.Request == 5 && Failure.Value == 1 && Failure.Address == 0);
  assert (Failure.Ep0.Cause == MOTO_G20_USB_EP0_CAUSE_CSR_ERROR && !Failure.Ep0.HardwareAccessLost);
  assert (Failure.Ep0.ValidMask == 3 && Failure.Ep0.Csr0 == MU_C0_ERROR && Failure.Ep0.IntrTx == 1);
  assert (Failure.Ep0.Polls == 1 && !Failure.Ep0.DelayUs && !Failure.Ep0.DevCtl);
  assert (!EpReg[0][2] && !EpReg[0][3] && !Reg[MU_INTRTX]); // Cleanup must not erase the recorded error.
  ErrorStage = TxStage = 0;
  assert (Control (&mUsb.Hc, 0, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbNoData,
                   NULL, NULL, 100, NULL, &Result) == EFI_SUCCESS);
  assert (!memcmp (&Failure, &mDiagnostic.LastFailure, sizeof (Failure)));
  PowerActive = FALSE; mUsb.Ready = FALSE; Before = MmioCount;
  assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_SUCCESS);
  assert (MmioCount == Before && !OffMmio && !memcmp (&Failure, &Diagnostic.LastFailure, sizeof (Failure)));
  puts ("PASS V6 CSR0_ERROR distinguishes SET_ADDRESS setup failure before cleanup and survives success plus powered-off getter");

  for (unsigned I = 0; I < 2; ++I) {
    ResetModel (); TimeoutStage = 1; DevCtlLossAtUs = 10;
    DevCtlClearMask = I ? MU_DEV_VBUS : MU_DEV_HOST;
    assert (Control (&mUsb.Hc, 0, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbNoData,
                     NULL, NULL, 100, NULL, &Result) == EFI_DEVICE_ERROR);
    Failure = mDiagnostic.LastFailure;
    assert (Result == EFI_USB_ERR_SYSTEM && Failure.Phase == 1);
    assert (Failure.Ep0.Cause == MOTO_G20_USB_EP0_CAUSE_HOST_VBUS_LOST && !Failure.Ep0.HardwareAccessLost);
    assert (Failure.Ep0.ValidMask == 7 && Failure.Ep0.DevCtl == Reg[MU_DEVCTL]);
    assert (!(Failure.Ep0.DevCtl & DevCtlClearMask));
    assert (Failure.Ep0.Csr0 == (MU_C0_SETUP | MU_C0_TXREADY) && !Failure.Ep0.IntrTx);
    assert (Failure.Ep0.Polls == 2 && Failure.Ep0.DelayUs == 10 && DelayUs == 10);
    assert (mUsb.Ready && PowerActive && !OffMmio); // The diagnostics do not change V5 error recovery.
  }
  puts ("PASS V6 HOST and VBUS loss use the existing DEVCTL sample and differ from CSR error despite identical SYSTEM result");

  ResetModel (); TimeoutStage = 1; RoleLossAtUs = 10;
  assert (Control (&mUsb.Hc, 0, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbNoData,
                   NULL, NULL, 300, NULL, &Result) == EFI_DEVICE_ERROR);
  Failure = mDiagnostic.LastFailure;
  assert (Result == EFI_USB_ERR_SYSTEM && Failure.Ep0.Cause == MOTO_G20_USB_EP0_CAUSE_ROLE_LOST);
  assert (Failure.Ep0.HardwareAccessLost && Failure.Ep0.ValidMask == 7);
  assert (Failure.Ep0.Csr0 == (MU_C0_SETUP | MU_C0_TXREADY) && !Failure.Ep0.IntrTx);
  assert ((Failure.Ep0.DevCtl & (MU_DEV_HOST | MU_DEV_VBUS)) == (MU_DEV_HOST | MU_DEV_VBUS));
  assert (Failure.Ep0.Polls == 10000 && Failure.Ep0.DelayUs == 99990 && DelayUs == 99990);
  assert (!mUsb.Ready && !PowerActive && !OffMmio);
  Before = MmioCount;
  assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_SUCCESS);
  assert (MmioCount == Before && !memcmp (&Failure, &Diagnostic.LastFailure, sizeof (Failure)));
  puts ("PASS V6 role loss preserves only the earlier historical poll and performs zero HC MMIO after gating, including cleanup/getter");

  for (unsigned I = 0; I < 3; ++I) {
    ResetModel ();
    if (I == 0) { StallStage = 1; }
    else if (I == 1) { NakStage = 1; }
    else { TimeoutStage = 1; }
    EFI_STATUS Status = Control (&mUsb.Hc, 0, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbNoData,
                                 NULL, NULL, 1, NULL, &Result);
    Failure = mDiagnostic.LastFailure;
    assert (Failure.Ep0.Valid && !Failure.Ep0.HardwareAccessLost && Failure.Phase == 1);
    if (I == 0) {
      assert (Status == EFI_DEVICE_ERROR && Result == EFI_USB_ERR_STALL);
      assert (Failure.Ep0.Cause == MOTO_G20_USB_EP0_CAUSE_STALL && Failure.Ep0.Csr0 == MU_C0_STALL);
    } else if (I == 1) {
      assert (Status == EFI_TIMEOUT && Result == (EFI_USB_ERR_TIMEOUT | EFI_USB_ERR_NAK));
      assert (Failure.Ep0.Cause == MOTO_G20_USB_EP0_CAUSE_NAK && Failure.Ep0.Csr0 == MU_C0_NAK);
    } else {
      assert (Status == EFI_TIMEOUT && Result == EFI_USB_ERR_TIMEOUT);
      assert (Failure.Ep0.Cause == MOTO_G20_USB_EP0_CAUSE_TIMEOUT && Failure.Ep0.ValidMask == 7);
      assert (Failure.Ep0.Polls == 100 && Failure.Ep0.DelayUs == 1000 && DelayUs == 1000);
    }
    if (I != 2) { assert (Failure.Ep0.ValidMask == 3 && Failure.Ep0.Polls == 1 && !Failure.Ep0.DelayUs); }
  }
  puts ("PASS V6 STALL, NAK and budget timeout have distinct causes, valid samples and unchanged polling delay budgets");

  ResetModel (); Before = MmioCount;
  assert (Control (&mUsb.Hc, 0, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbNoData,
                   NULL, NULL, 100, NULL, NULL) == EFI_INVALID_PARAMETER);
  assert (!mDiagnostic.LastFailure.Ep0.Valid && !mDiagnostic.LastFailure.Ep0.ValidMask && MmioCount == Before);
  TimeoutStage = 1;
  assert (Control (&mUsb.Hc, 0, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbNoData,
                   NULL, NULL, 1, NULL, &Result) == EFI_TIMEOUT);
  assert (mDiagnostic.LastFailure.Ep0.Valid);
  UINT8 Toggle = 0;
  assert (AsyncInterrupt (&mUsb.Hc, 3, 0x81, EFI_USB_SPEED_FULL, 8, FALSE,
                         &Toggle, 0, 0, NULL, NULL, NULL) == EFI_INVALID_PARAMETER);
  assert (mDiagnostic.LastFailure.Kind == 4 && !mDiagnostic.LastFailure.Ep0.Valid && !mDiagnostic.LastFailure.Ep0.ValidMask);
  puts ("PASS V6 validation and non-control failures never inherit an unrelated EP0 trace");

  ResetModel (); assert (!mDiagnostic.RootResetValid);
  assert (SetPort (&mUsb.Hc, 0, EfiUsbPortReset) == EFI_SUCCESS);
  assert (!mDiagnostic.RootResetValid && mUsb.Resetting);
  Reg[MU_POWER] |= MU_POWER_HSMODE;
  assert (ClearPort (&mUsb.Hc, 0, EfiUsbPortReset) == EFI_SUCCESS);
  assert (mDiagnostic.RootResetValid && !mUsb.Resetting && mDiagnostic.RootResetDevCtl == Reg[MU_DEVCTL]);
  assert (mDiagnostic.RootResetPower == Reg[MU_POWER] && !(mDiagnostic.RootResetPower & MU_POWER_RESET));
  assert (mDiagnostic.RootResetPower & MU_POWER_HSMODE);
  UINT8 ResetDevCtl = mDiagnostic.RootResetDevCtl, ResetPower = mDiagnostic.RootResetPower;
  RoleAllowed = FALSE; assert (CheckRole () == EFI_NOT_READY);
  Before = MmioCount;
  assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_SUCCESS);
  assert (Diagnostic.RootResetValid && Diagnostic.RootResetDevCtl == ResetDevCtl && Diagnostic.RootResetPower == ResetPower);
  assert (MmioCount == Before && !OffMmio); // These remain historical; never a live powered-off read.
  ResetModel ();
  assert (SetPort (&mUsb.Hc, 0, EfiUsbPortReset) == EFI_SUCCESS);
  assert (ClearPort (&mUsb.Hc, 0, EfiUsbPortReset) == EFI_SUCCESS && mDiagnostic.RootResetValid);
  assert (SetPort (&mUsb.Hc, 0, EfiUsbPortReset) == EFI_SUCCESS && !mDiagnostic.RootResetValid);
  RoleAllowed = FALSE; Before = MmioCount;
  assert (ClearPort (&mUsb.Hc, 0, EfiUsbPortReset) == EFI_NOT_READY);
  assert (!mDiagnostic.RootResetValid && MmioCount == Before && !OffMmio);
  puts ("PASS V6 root reset samples only after deassertion, remains historical after power-off, and is invalid after a new interrupted reset");
}

int main (void) {
  EFI_USB_DEVICE_REQUEST Request = { 0x80, 6, 0x100, 0, 18 };
  UINT8 Data[80], Toggle;
  UINTN Length;
  UINT32 Result;
  EFI_USB_PORT_STATUS Port;
  EFI_USB2_HC_TRANSACTION_TRANSLATOR Translator = { 2, 1 };
  EFI_USB2_HC_TRANSACTION_TRANSLATOR RootTranslator = { 0, 1 };
  TRACKED_REPORT Hub = {0}, Keyboard = {0};
  MOTO_G20_USB_DIAGNOSTIC_STATUS Diagnostic;
  ResetModel (); Length = 18;
  assert (Control (&mUsb.Hc, 0, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn, Data, &Length, 100, NULL, &Result) == EFI_SUCCESS);
  assert (Result == 0 && Length == 18 && !memcmp (Data, Source, 18) && TxStage == 5);
  assert (CurrentTpl == TPL_APPLICATION && mDiagnostic.ControlCompleted == 1 && OffMmio == 0);
  puts ("PASS control descriptor multiple packets and status stage");

  ResetModel (); SourceLength = 11; Length = 18;
  assert (Control (&mUsb.Hc, 0, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn, Data, &Length, 100, NULL, &Result) == EFI_SUCCESS);
  assert (Length == 11 && TxStage == 4 && !memcmp (Data, Source, 11));
  puts ("PASS control short packet terminates data stage");

  ResetModel (); Request.RequestType = 0; Request.Length = 10; Length = 10;
  assert (Control (&mUsb.Hc, 1, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataOut, Data, &Length, 100, NULL, &Result) == EFI_SUCCESS);
  assert (Length == 10 && TxStage == 4);
  ResetModel (); Request.Length = 0;
  assert (Control (&mUsb.Hc, 0, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbNoData, NULL, NULL, 100, NULL, &Result) == EFI_SUCCESS);
  assert (TxStage == 2);
  puts ("PASS control OUT and no-data status-IN");

  ResetModel (); Request.RequestType = 0x80; Request.Length = 18; Length = 18; StallStage = 2;
  assert (Control (&mUsb.Hc, 0, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn, Data, &Length, 1, NULL, &Result) == EFI_DEVICE_ERROR);
  assert (Result == EFI_USB_ERR_STALL && Length == 0 && !mUsb.Busy);
  ResetModel (); Length = 18; TimeoutStage = 1;
  assert (Control (&mUsb.Hc, 0, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn, Data, &Length, 1, NULL, &Result) == EFI_TIMEOUT);
  assert (Result == EFI_USB_ERR_TIMEOUT && Length == 0 && DelayUs == 1000);
  ResetModel (); Length = 18; OverflowStage = 2;
  memset (Data, 0xCC, sizeof (Data));
  assert (Control (&mUsb.Hc, 0, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn, Data, &Length, 1, NULL, &Result) == EFI_DEVICE_ERROR);
  assert (Result & EFI_USB_ERR_BABBLE); assert (Data[0] == 0xCC && Data[79] == 0xCC);
  puts ("PASS EP0 stall, bounded timeout and overflow never report success");

  ResetModel (); Length = 18;
  assert (Control (&mUsb.Hc, 3, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn, Data, &Length, 100, &Translator, &Result) == EFI_SUCCESS);
  assert (Result == 0 && Length == 18 && TxStage == 5 && Reg[MU_TXFUNC (0)] == 3 && Reg[MU_RXFUNC (0)] == 3);
  for (unsigned I = 1; I <= TxStage; ++I) {
    assert (StageTxHub[I] == 2 && StageRxHub[I] == 2 && StageTxPort[I] == 1 && StageRxPort[I] == 1);
  }
  ResetModel (); Length = 18;
  assert (Control (&mUsb.Hc, 1, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn, Data, &Length, 100, &RootTranslator, &Result) == EFI_SUCCESS);
  for (unsigned I = 1; I <= TxStage; ++I) {
    assert (!StageTxHub[I] && !StageRxHub[I] && !StageTxPort[I] && !StageRxPort[I]);
  }
  puts ("PASS split EP0 retains single-TT hub/port for setup, data, status; root {0,1} is direct");

  ResetModel (); Length = 18; Translator.TranslatorPortNumber = 0;
  assert (Control (&mUsb.Hc, 3, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn, Data, &Length, 1, &Translator, &Result) == EFI_INVALID_PARAMETER);
  Translator.TranslatorPortNumber = 1; Translator.TranslatorHubAddress = 0x82;
  assert (Control (&mUsb.Hc, 3, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn, Data, &Length, 1, &Translator, &Result) == EFI_INVALID_PARAMETER);
  Translator.TranslatorHubAddress = 2;
  assert (MmioCount == 0 && Result == EFI_USB_ERR_NOTEXECUTE);
  assert (Bulk (&mUsb.Hc, 1, 1, 0, 8, 1, NULL, &Length, NULL, 1, NULL, &Result) == EFI_UNSUPPORTED && Result == EFI_USB_ERR_NOTEXECUTE);
  puts ("PASS malformed TT and unsupported bulk refuse execution");

  ResetModel (); Toggle = 0;
  assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, TRUE, &Toggle, 10, 8, NULL, OnReport, NULL) == EFI_SUCCESS);
  EpReg[1][6] = MU_RX_DATAERR;
  PollInterrupt (); assert (Callbacks == 0 && mUsb.Pipes[0].Toggle == 0 && !mUsb.Pipes[0].Pending);
  for (unsigned I = 0; I < 10; ++I) { PollInterrupt (); }
  assert (mUsb.Pipes[0].Pending);
  InjectReport (8); PollInterrupt ();
  assert (Callbacks == 1 && CallbackErrors == 0 && mUsb.Pipes[0].Toggle == 1 && CallbackData[7] == 0xA7);
  assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, FALSE, &Toggle, 0, 0, NULL, NULL, NULL) == EFI_SUCCESS && Toggle == 1);
  assert (!mUsb.Pipes[0].Active && !mUsb.Pipes[0].Pending);
  PollInterrupt (); assert (Callbacks == 1);
  puts ("PASS periodic interrupt-IN, NAK suppression, toggle and cancel");

  ResetModel (); Toggle = 0; CancelInCallback = TRUE;
  assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, TRUE, &Toggle, 1, 8, NULL, OnReport, NULL) == EFI_SUCCESS);
  InjectReport (8); PollInterrupt (); assert (Callbacks == 1 && !mUsb.Pipes[0].Active);
  ResetModel (); Toggle = 0;
  assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, TRUE, &Toggle, 1, 8, NULL, OnReport, NULL) == EFI_SUCCESS);
  InjectReport (9); PollInterrupt (); assert (CallbackErrors == 1 && mUsb.Pipes[0].Toggle == 0);
  puts ("PASS cancellation inside callback and oversize report failure");

  ResetModel (); Toggle = 0;
  assert (AsyncInterrupt (&mUsb.Hc, 2, 0x81, EFI_USB_SPEED_HIGH, 1, TRUE, &Toggle, 10, 1, NULL, OnTrackedReport, &Hub) == EFI_SUCCESS);
  assert (AsyncInterrupt (&mUsb.Hc, 3, 0x81, EFI_USB_SPEED_LOW, 8, TRUE, &Toggle, 1, 8, &Translator, OnTrackedReport, &Keyboard) == EFI_SUCCESS);
  assert (Reg[MU_RXFUNC (1)] == 2 && Reg[MU_RXHUB (1)] == 0 && Reg[MU_RXPORT (1)] == 0);
  assert (Reg[MU_RXFUNC (2)] == 3 && Reg[MU_RXHUB (2)] == 2 && Reg[MU_RXPORT (2)] == 1);
  assert (EpReg[1][12] == (0x40 | 0x30 | 1) && EpReg[2][12] == (0xC0 | 0x30 | 1));
  InjectReportTo (1, 1, 0x02); InjectReportTo (2, 8, 0x30); PollInterrupt ();
  assert (Hub.Reports == 1 && !Hub.Errors && Hub.Length == 1 && Hub.Data[0] == 2);
  assert (Keyboard.Reports == 1 && !Keyboard.Errors && Keyboard.Length == 8 && Keyboard.Data[7] == 0x37);
  assert (mUsb.Pipes[0].Toggle == 1 && mUsb.Pipes[1].Toggle == 1);
  // Cancel the hub's status pipe while the keyboard remains independently usable.
  assert (AsyncInterrupt (&mUsb.Hc, 2, 0x81, EFI_USB_SPEED_HIGH, 1, FALSE, &Toggle, 0, 0, NULL, NULL, NULL) == EFI_SUCCESS && Toggle == 1);
  assert (!mUsb.Pipes[0].Active && mUsb.Pipes[1].Active);
  PollInterrupt (); assert (mUsb.Pipes[1].Pending);
  InjectReportTo (2, 8, 0x40); PollInterrupt ();
  assert (Hub.Reports == 1 && Keyboard.Reports == 2 && Keyboard.Data[0] == 0x40 && mUsb.Pipes[1].Toggle == 0);
  // Reuse freed hardware EP1 with a different device and preserve EP2's state.
  Toggle = 0;
  assert (AsyncInterrupt (&mUsb.Hc, 4, 0x82, EFI_USB_SPEED_FULL, 8, TRUE, &Toggle, 1, 8, &RootTranslator, OnTrackedReport, &Hub) == EFI_SUCCESS);
  assert (Reg[MU_RXFUNC (1)] == 4 && Reg[MU_RXHUB (1)] == 0 && Reg[MU_RXPORT (1)] == 0);
  assert (mUsb.Pipes[1].Address == 3 && mUsb.Pipes[1].Toggle == 0 && mUsb.Pipes[1].Active);
  puts ("PASS simultaneous hub status and split keyboard use isolated FIFOs, toggles, cancellation and slot reuse");

  ResetModel (); Toggle = 0;
  for (unsigned I = 0; I < INTERRUPT_PIPE_COUNT; ++I) {
    assert (AsyncInterrupt (&mUsb.Hc, (UINT8)(I + 1), 0x81, EFI_USB_SPEED_FULL, 8, TRUE, &Toggle, 1, 8, NULL, OnReport, NULL) == EFI_SUCCESS);
  }
  { unsigned Before = MmioCount;
    assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, TRUE, &Toggle, 1, 8, NULL, OnReport, NULL) == EFI_INVALID_PARAMETER);
    assert (AsyncInterrupt (&mUsb.Hc, 5, 0x81, EFI_USB_SPEED_FULL, 8, TRUE, &Toggle, 1, 8, NULL, OnReport, NULL) == EFI_OUT_OF_RESOURCES);
    assert (AsyncInterrupt (&mUsb.Hc, 5, 0x81, EFI_USB_SPEED_HIGH, 512, TRUE, &Toggle, 1, 8, NULL, OnReport, NULL) == EFI_UNSUPPORTED);
    assert (MmioCount == Before);
  }
  RoleAllowed = FALSE; PollInterrupt ();
  for (unsigned I = 0; I < INTERRUPT_PIPE_COUNT; ++I) { assert (!mUsb.Pipes[I].Active && !mUsb.Pipes[I].Pending); }
  assert (Callbacks == 0 && OffMmio == 0);
  puts ("PASS four-pipe bound and duplicate rejection; role loss invalidates every pipe without MMIO");

  ResetModel (); mUsb.Enabled = FALSE;
  assert (SetPort (&mUsb.Hc, 0, EfiUsbPortReset) == EFI_SUCCESS);
  assert (mUsb.Resetting && !mUsb.Enabled && (Reg[MU_POWER] & MU_POWER_RESET));
  assert (ClearPort (&mUsb.Hc, 0, EfiUsbPortReset) == EFI_SUCCESS);
  assert (GetPort (&mUsb.Hc, 0, &Port) == EFI_SUCCESS);
  assert ((Port.PortStatus & (USB_PORT_STAT_ENABLE | USB_PORT_STAT_CONNECTION)) == (USB_PORT_STAT_ENABLE | USB_PORT_STAT_CONNECTION));
  assert (Port.PortChangeStatus & USB_PORT_STAT_C_RESET);
  assert (ClearPort (&mUsb.Hc, 0, EfiUsbPortResetChange) == EFI_SUCCESS);
  puts ("PASS root reset completion advertises ENABLE to UsbBusDxe");

  // Every unused endpoint must be idle before sharing its inherited FIFO RAM.
  for (unsigned Ep = 1; Ep <= 15; ++Ep) {
    const unsigned Bits[] = {0x0001, 0x0002, 0x8000};
    for (unsigned Bit = 0; Bit < 3; ++Bit) {
      ResetModel (); EpReg[Ep][2] = (unsigned char)Bits[Bit]; EpReg[Ep][3] = (unsigned char)(Bits[Bit] >> 8);
      assert (ConfigureController (TRUE) == EFI_ACCESS_DENIED);
      assert (FifoConfigWrites == 0 && EndpointWrites == 0);
    }
  }
  for (unsigned Ep = 5; Ep <= 15; ++Ep) {
    const unsigned Bits[] = {0x0001, 0x0020, 0x4000};
    for (unsigned Bit = 0; Bit < 3; ++Bit) {
      ResetModel (); EpReg[Ep][6] = (unsigned char)Bits[Bit]; EpReg[Ep][7] = (unsigned char)(Bits[Bit] >> 8);
      assert (ConfigureController (TRUE) == EFI_ACCESS_DENIED);
      assert (FifoConfigWrites == 0 && EndpointWrites == 0);
    }
  }
  ResetModel (); CurrentTpl = TPL_CALLBACK; EpReg[15][6] = 0x20;
  assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS);
  for (unsigned I = 0; I < 200; ++I) { TimerTick (NULL, NULL); }
  assert (Installs == 1 && !mUsb.Published && mUsb.Fatal && mDiagnostic.Status == EFI_ACCESS_DENIED);
  assert (EndpointWrites == 0 && FifoConfigWrites == 0);
  puts ("PASS inherited unused TX/RX activity prevents FIFO reassignment and endpoint cleanup");

  ResetModel (); Reg[MU_OTG_EXT_CSR] = MU_HOST_FORCE;
  CheckStoppedSession = TRUE;
  assert (ConfigureController (TRUE) == EFI_SUCCESS && FifoConfigWrites > 0);
  assert ((Reg[MU_DEVCTL] & MU_DEV_SESSION) && (Reg[MU_OTG_EXT_CSR] & MU_HOST_FORCE));
  ResetModel (); Reg[MU_OTG_EXT_CSR] = MU_HOST_FORCE; BlockSessionStop = TRUE;
  assert (ConfigureController (TRUE) == EFI_DEVICE_ERROR);
  assert (FifoConfigWrites == 0 && EndpointWrites == 0);
  puts ("PASS session is stopped before FIFO reassignment and failed stop prevents takeover");

  ResetModel (); Reg[MU_DMA_EN_STATUS] = 1;
  assert (ConfigureController (TRUE) == EFI_ACCESS_DENIED && mDiagnostic.ControllerStep == 2);
  ResetModel (); assert (ConfigureController (TRUE) == EFI_SUCCESS);
  assert (Reg[MU_OTG_EXT_CSR] & MU_HOST_FORCE);
  for (unsigned I = 1; I <= INTERRUPT_PIPE_COUNT; ++I) {
    assert (FifoReg[I][1] == 3 && FifoReg[I][4] == I * 8 && FifoReg[I][5] == 0);
  }
  ResetModel (); RoleAllowed = FALSE;
  assert (CheckRole () == EFI_NOT_READY && !mUsb.Ready && OffMmio == 0);
  puts ("PASS active DMA blocks controller; role loss prevents MMIO");

  ResetModel (); RoleAllowed = FALSE; PowerActive = FALSE;
  assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS);
  for (unsigned I = 0; I < 200; ++I) { TimerTick (NULL, NULL); }
  assert (!mUsb.Published && Installs == 1 && MmioCount == 0);
  assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_SUCCESS && !Diagnostic.HostReady);
  ResetModel (); PrepareStatus = EFI_ACCESS_DENIED;
  assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS);
  assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_SUCCESS && Diagnostic.Status == EFI_ACCESS_DENIED);
  assert (!mUsb.Published && Installs == 1 && MmioCount == 0);
  puts ("PASS resident diagnostic distinguishes waiting cable and prepare failure from HC success");

  ResetModel (); PowerActive = FALSE; mUsb.Ready = FALSE;
  mDiagnostic.Stage = MotoUsbHcStageWaitRole; mDiagnostic.Status = EFI_NOT_READY;
  mDiagnostic.ControllerStep = 3; mDiagnostic.LastTransferResult = EFI_USB_ERR_SYSTEM;
  mDiagnostic.ControlCompleted = 11; mDiagnostic.InterruptReports = 17;
  RoleDiagnostic.Samples = 0x100000013ULL; RoleDiagnostic.SourceSeen = 0x100000005ULL;
  RoleDiagnostic.BoostAttempts = 0x100000002ULL; RoleDiagnostic.SnapshotAttempts = 0x100000007ULL;
  RoleDiagnostic.SnapshotFailures = 0x100000001ULL; RoleDiagnostic.SeenStates = BIT0 | BIT3 | BIT5 | BIT9;
  RoleDiagnostic.ValidMask = MOTO_G20_USB_ROLE_ALL_VALID & ~MOTO_G20_USB_ROLE_TRIM_VALID;
  RoleDiagnostic.ErrorMask = MOTO_G20_USB_ROLE_TRIM_VALID;
  RoleDiagnostic.RoleReadStatus = EFI_NOT_READY; RoleDiagnostic.SnapshotStatus = EFI_TIMEOUT;
  RoleDiagnostic.CalibrationStatus = EFI_DEVICE_ERROR;
  RoleDiagnostic.En = 0x1234; RoleDiagnostic.Mode = 0x2345; RoleDiagnostic.IntEn = 0x3456;
  RoleDiagnostic.IntRaw = 0x4567; RoleDiagnostic.IntMask = 0x5678; RoleDiagnostic.TypecStatus = 0x6789;
  RoleDiagnostic.Trim = 0x789A; RoleDiagnostic.EfuseData = 0x89AB; RoleDiagnostic.RequestedTrim = 0x9ABC;
  RoleDiagnostic.EfuseValid = TRUE; RoleDiagnostic.TrimApplied = FALSE;
  assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_SUCCESS);
  assert (mDiagnosticProtocol.Revision == 6 && Diagnostic.Revision == 6 && Diagnostic.Size == sizeof (Diagnostic));
  assert (!memcmp (&Diagnostic.Role, &RoleDiagnostic, sizeof (RoleDiagnostic)));
  assert (Diagnostic.Stage == MotoUsbHcStageWaitRole && Diagnostic.Status == EFI_NOT_READY);
  assert (Diagnostic.ControllerStep == 3 && Diagnostic.LastTransferResult == EFI_USB_ERR_SYSTEM);
  assert (Diagnostic.ControlCompleted == 11 && Diagnostic.InterruptReports == 17);
  assert (!Diagnostic.HostReady && !Diagnostic.HcPublished && Diagnostic.PortConnected && Diagnostic.PortEnabled);
  assert (Diagnostic.PlatformStage == MotoUsbStageRunning && Diagnostic.PlatformStatus == EFI_SUCCESS);
  assert (Diagnostic.TypecState == 5 && !Diagnostic.VbusPresent && RoleDiagnosticCalls == 1);
  assert (MmioCount == 0 && OffMmio == 0);
  puts ("PASS diagnostic revision 6 preserves complete cached role telemetry and HC state without MMIO");

  ResetModel (); PowerActive = FALSE;
  mDiagnostic.Status = EFI_ACCESS_DENIED;
  RoleDiagnostic.ValidMask = MOTO_G20_USB_ROLE_ALL_VALID;
  RoleDiagnostic.SourceSeen = 9; RoleDiagnostic.TrimApplied = TRUE;
  RoleDiagnosticStatus = EFI_TIMEOUT;
  assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_TIMEOUT);
  assert (Diagnostic.Revision == 6 && Diagnostic.Size == sizeof (Diagnostic) && Diagnostic.Status == EFI_ACCESS_DENIED);
  assert (Diagnostic.Role.Revision == MOTO_G20_USB_ROLE_DIAGNOSTIC_REVISION && Diagnostic.Role.Size == sizeof (Diagnostic.Role));
  assert (!Diagnostic.Role.ValidMask && !Diagnostic.Role.ErrorMask && !Diagnostic.Role.SourceSeen && !Diagnostic.Role.TrimApplied);
  assert (Diagnostic.Role.RoleReadStatus == EFI_TIMEOUT && Diagnostic.Role.SnapshotStatus == EFI_TIMEOUT && Diagnostic.Role.CalibrationStatus == EFI_TIMEOUT);
  assert (MmioCount == 0 && RoleDiagnosticCalls == 1);
  puts ("PASS diagnostic propagates cached getter failure and invalidates partial role data");

  ResetModel (); RoleDiagnostic.Revision += 1;
  assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_INCOMPATIBLE_VERSION);
  assert (!Diagnostic.Role.ValidMask && Diagnostic.Role.SnapshotStatus == EFI_INCOMPATIBLE_VERSION);
  RoleDiagnostic.Revision = MOTO_G20_USB_ROLE_DIAGNOSTIC_REVISION; RoleDiagnostic.Size -= 1;
  assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_INCOMPATIBLE_VERSION);
  RoleDiagnostic.Size += 2;
  assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_INCOMPATIBLE_VERSION);
  assert (MmioCount == 0 && RoleDiagnosticCalls == 3);
  puts ("PASS diagnostic rejects unsupported cached role revision and both truncated and oversized payloads");

  ResetModel (); memset (&Diagnostic, 0xA5, sizeof (Diagnostic));
  assert (ReadDiagnostic (NULL, &Diagnostic) == EFI_INVALID_PARAMETER);
  assert (ReadDiagnostic (&mDiagnosticProtocol, NULL) == EFI_INVALID_PARAMETER);
  { const unsigned char *Bytes = (const unsigned char *)&Diagnostic;
    for (unsigned I = 0; I < sizeof (Diagnostic); ++I) { assert (Bytes[I] == 0xA5); }
  }
  assert (MmioCount == 0 && RoleDiagnosticCalls == 0);
  puts ("PASS invalid diagnostic arguments leave output untouched and never retrieve platform state");

  ResetModel (); CurrentTpl = TPL_CALLBACK;
  mDiagnostic.ControlCompleted = 41; RoleDiagnostic.Samples = 41;
  RoleDiagnostic.TypecStatus = 5; DiagnosticNotifyPending = TRUE;
  assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_SUCCESS);
  assert (Diagnostic.ControlCompleted == 41 && Diagnostic.Role.Samples == 41);
  assert (Diagnostic.Status == EFI_SUCCESS && Diagnostic.Role.RoleReadStatus == EFI_SUCCESS);
  assert (Diagnostic.HostReady && Diagnostic.TypecState == 5 && Diagnostic.VbusPresent && Diagnostic.Role.TypecStatus == 5);
  assert (PlatformGetterTpl == TPL_NOTIFY && RoleGetterTpl == TPL_NOTIFY && CurrentTpl == TPL_CALLBACK);
  assert (DiagnosticNotifyCalls == 1 && !DiagnosticNotifyPending && mDiagnostic.ControlCompleted == 82);
  assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_SUCCESS);
  assert (Diagnostic.ControlCompleted == 82 && Diagnostic.Role.Samples == 82);
  assert (Diagnostic.Status == EFI_NOT_READY && Diagnostic.Role.RoleReadStatus == EFI_NOT_READY);
  assert (!Diagnostic.HostReady && Diagnostic.TypecState == 2 && !Diagnostic.VbusPresent && Diagnostic.Role.TypecStatus == 2);
  assert (MmioCount == 0 && OffMmio == 0 && CurrentTpl == TPL_CALLBACK);
  puts ("PASS pending role notification cannot mix HC and platform snapshots and runs after restoration");

  { const EFI_TPL Levels[] = {TPL_APPLICATION, TPL_CALLBACK, TPL_NOTIFY};
    for (unsigned I = 0; I < sizeof (Levels) / sizeof (Levels[0]); ++I) {
      ResetModel (); CurrentTpl = Levels[I];
      assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_SUCCESS && CurrentTpl == Levels[I]);
      RoleDiagnosticStatus = EFI_TIMEOUT;
      assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_TIMEOUT && CurrentTpl == Levels[I]);
      RoleDiagnosticStatus = EFI_SUCCESS; RoleDiagnostic.Revision += 1;
      assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_INCOMPATIBLE_VERSION && CurrentTpl == Levels[I]);
      assert (PlatformGetterTpl == TPL_NOTIFY && RoleGetterTpl == TPL_NOTIFY && MmioCount == 0);
    }
  }
  puts ("PASS cached diagnostic restores caller TPL on success, getter failure and incompatible role metadata");

  ResetModel (); CurrentTpl = TPL_NOTIFY; Length = 18;
  assert (Control (&mUsb.Hc, 0, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn, Data, &Length, 100, NULL, &Result) == EFI_SUCCESS);
  assert (CurrentTpl == TPL_NOTIFY);
  assert (GetPort (&mUsb.Hc, 0, &Port) == EFI_SUCCESS && CurrentTpl == TPL_NOTIFY);
  assert (SetPort (&mUsb.Hc, 0, EfiUsbPortReset) == EFI_SUCCESS && CurrentTpl == TPL_NOTIFY);
  assert (ClearPort (&mUsb.Hc, 0, EfiUsbPortReset) == EFI_SUCCESS && CurrentTpl == TPL_NOTIFY);
  Toggle = 0;
  assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, TRUE, &Toggle, 1, 8, NULL, OnReport, NULL) == EFI_SUCCESS);
  assert (CurrentTpl == TPL_NOTIFY);
  assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, FALSE, &Toggle, 0, 0, NULL, NULL, NULL) == EFI_SUCCESS);
  assert (CurrentTpl == TPL_NOTIFY);
  puts ("PASS UsbBusDxe TPL_NOTIFY entry never lowers TPL");

  ResetModel (); CurrentTpl = TPL_CALLBACK; PowerActive = FALSE; RoleAllowed = FALSE;
  TimerTick (NULL, NULL);
  assert (OffMmio == 0 && MmioCount == 0 && !mUsb.Ready);
  ResetModel (); CurrentTpl = TPL_NOTIFY; PowerActive = FALSE; RoleAllowed = FALSE;
  ExitBoot (NULL, NULL);
  assert (OffMmio == 0 && MmioCount == 0 && mUsb.Ended);
  ResetModel (); Length = 18; TimeoutStage = 1; RoleLossAtUs = 150000;
  assert (Control (&mUsb.Hc, 0, EFI_USB_SPEED_FULL, 8, &Request, EfiUsbDataIn, Data, &Length, 300, NULL, &Result) == EFI_DEVICE_ERROR);
  assert (OffMmio == 0 && !mUsb.Ready && DelayUs < 300000 && Result == EFI_USB_ERR_SYSTEM);
  puts ("PASS platform power loss between ticks and during control prevents stale MMIO");

  ResetModel (); CurrentTpl = TPL_CALLBACK; Toggle = 0;
  assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, TRUE, &Toggle, 1, 8, NULL, OnReport, NULL) == EFI_SUCCESS);
  InjectReport (8); CancelOnRestore = TRUE; PollInterrupt ();
  assert (!mUsb.Pipes[0].Active && Callbacks == 0 && CurrentTpl == TPL_CALLBACK);
  ResetModel (); Toggle = 0;
  assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, TRUE, &Toggle, 1, 8, NULL, OnReport, NULL) == EFI_SUCCESS);
  PowerActive = RoleAllowed = FALSE;
  { unsigned Before = MmioCount;
    assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, FALSE, &Toggle, 0, 0, NULL, NULL, NULL) == EFI_SUCCESS);
    assert (MmioCount == Before && OffMmio == 0 && !mUsb.Pipes[0].Active);
  }
  puts ("PASS cancel at TPL restore suppresses stale callback; cancel after power loss touches no MMIO");

  ResetModel (); CurrentTpl = TPL_CALLBACK; Toggle = 0;
  assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, TRUE, &Toggle, 1, 8, NULL, OnReport, NULL) == EFI_SUCCESS);
  InjectReport (8); CancelOnRestore = RecreateOnRestore = TRUE; PollInterrupt ();
  assert (mUsb.Pipes[0].Active && mUsb.Pipes[0].Pending && mUsb.Pipes[0].Toggle == 0 && Callbacks == 0);
  InjectReport (8); PollInterrupt ();
  assert (Callbacks == 1 && mUsb.Pipes[0].Toggle == 1 && CurrentTpl == TPL_CALLBACK);
  puts ("PASS cancel plus recreation on TPL restore suppresses stale generation callback");

  ResetModel (); CurrentTpl = TPL_CALLBACK;
  assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS);
  for (unsigned I = 0; I < 100; ++I) { TimerTick (NULL, NULL); }
  assert (Installs == 2 && Connects == 1 && mUsb.Published && mUsb.Ready && OffMmio == 0);
  assert (ClearPort (&mUsb.Hc, 0, EfiUsbPortPower) == EFI_SUCCESS);
  assert (!(Reg[MU_DEVCTL] & MU_DEV_SESSION) && !(Reg[MU_OTG_EXT_CSR] & MU_HOST_FORCE));
  { unsigned Before = MmioCount;
    for (unsigned I = 0; I < 200; ++I) { TimerTick (NULL, NULL); }
    assert (MmioCount == Before && !mUsb.Ready && !PowerActive);
  }
  ResetModel (); Reg[MU_DMA_EN_STATUS] = 1; CurrentTpl = TPL_CALLBACK;
  assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS);
  for (unsigned I = 0; I < 200; ++I) { TimerTick (NULL, NULL); }
  assert (Installs == 1 && !mUsb.Published && mUsb.Fatal && mDiagnostic.ControllerStep == 2 && mDiagnostic.Status == EFI_ACCESS_DENIED);
  ResetModel (); Reg[MU_DEVCTL] = 0; CurrentTpl = TPL_CALLBACK;
  assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS);
  for (unsigned I = 0; I < 200; ++I) { TimerTick (NULL, NULL); }
  assert (Installs == 1 && !mUsb.Published && !mUsb.Fatal && mUsb.WaitingDetach && mDiagnostic.ControllerStep == 5 && mDiagnostic.Status == EFI_NOT_READY);
  puts ("PASS publication requires real controller qualification; power-off persists and clears host session");

  { const UINT8 Partial[] = {MU_DEV_HOST, MU_DEV_VBUS, 0};
    for (unsigned I = 0; I < sizeof (Partial); ++I) {
      StartTimeout (Partial[I]);
      assert (Installs == 1 && Connects == 0 && SessionWrites == 1);
      assert (mDiagnostic.SessionStatus == EFI_NOT_READY && mDiagnostic.ControllerStep == 5);
      assert (mDiagnostic.SessionWaitUs == ((Partial[I] == MU_DEV_VBUS) ? 1100000U : 100000U) && DelayUs == 100000);
      assert (mDiagnostic.SessionSnapshotValidMask == MOTO_G20_USB_SESSION_ALL_VALID);
      assert (mDiagnostic.SessionDevCtlBefore == Partial[I]);
      assert (mDiagnostic.SessionDevCtlStart == (Partial[I] | MU_DEV_SESSION));
      assert (mDiagnostic.SessionDevCtlEnd == (Partial[I] | MU_DEV_SESSION));
      assert (mDiagnostic.SessionPower == MU_POWER_HSEN && (mDiagnostic.SessionOtg & MU_HOST_FORCE));
      assert (!(Reg[MU_DEVCTL] & MU_DEV_SESSION) && !(Reg[MU_OTG_EXT_CSR] & MU_HOST_FORCE));
      assert (SessionWriteSequence < ForceWriteSequence && QuiesceCalls == 1 && StopCalls == 0);
      assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_SUCCESS);
      assert (Diagnostic.RecoveryWaiting && !Diagnostic.FatalLatched && Diagnostic.SessionAttempts == 1);
      assert (Diagnostic.SessionDevCtlEnd == (Partial[I] | MU_DEV_SESSION));
    }
  }
  puts ("PASS HOST-only/neither fail at 100ms; valid VBUS-only waits to 1100ms, never publishes, preserves pre-OFF samples");

  StartTimeout (0);
  { unsigned Before = MmioCount;
    for (unsigned I = 0; I < 50; ++I) { RoleCycle (); }
    assert (mUsb.WaitingDetach && !mDiagnostic.Rearms && InitCalls == 1 && SessionWrites == 1);
    assert (MmioCount == Before && !OffMmio && DetectCalls == 50);
    assert (Reset (&mUsb.Hc, EFI_USB_HC_RESET_GLOBAL) == EFI_NOT_READY);
    assert (GetPort (&mUsb.Hc, 0, &Port) == EFI_SUCCESS && !(Port.PortStatus & USB_PORT_STAT_POWER));
    assert (MmioCount == Before && !OffMmio);
  }
  puts ("PASS unchanged cable never reenergizes and public Reset/GetPort after OFF perform no HC MMIO");

  StartTimeout (0); DetectState = 9;
  for (unsigned I = 0; I < 12; ++I) { RoleCycle (); }
  assert (mUsb.WaitingDetach && !mDiagnostic.Rearms && mDiagnostic.DetachStableMs == 500);
  assert (!mUsb.DetachedStateObserved && InitCalls == 1 && SessionWrites == 1 && !OffMmio);
  puts ("PASS DRP sleep alone is insufficient to certify a detached cable");

  StartTimeout (0); DetectState = 0;
  for (unsigned I = 0; I < 5; ++I) { RoleCycle (); }
  assert (mDiagnostic.DetachStableMs == 400 && mUsb.WaitingDetach);
  DetectState = 5; RoleCycle ();
  assert (!mUsb.DetachTracking && mDiagnostic.DetachStableMs == 0 && !mDiagnostic.Rearms);
  DetectState = 3; DetectVbus = TRUE;
  for (unsigned I = 0; I < 10; ++I) { RoleCycle (); }
  assert (!mUsb.DetachTracking && !mDiagnostic.Rearms && SessionWrites == 1 && !OffMmio);
  puts ("PASS a sub-500ms detach and detached state with EIC VBUS present cannot rearm");

  StartTimeout (0); DetectState = 0;
  for (unsigned I = 0; I < 5; ++I) { RoleCycle (); }
  DetectStatus = EFI_NOT_READY; RoleCycle ();
  assert (!mDiagnostic.RecoverySampleValid && !mUsb.DetachTracking && !mDiagnostic.Rearms);
  assert (mUsb.WaitingDetach && !mUsb.Fatal);
  DetectStatus = EFI_SUCCESS;
  for (unsigned I = 0; I < 5; ++I) { RoleCycle (); }
  assert (mUsb.WaitingDetach && mDiagnostic.DetachStableMs == 400 && !mDiagnostic.Rearms && !OffMmio);
  puts ("PASS unavailable fresh role read invalidates the sample and restarts the entire detach interval");

  StartTimeout (0); DetectState = 3;
  { unsigned Before = MmioCount;
    RoleAllowed = FALSE;
    for (unsigned I = 0; I < 5; ++I) { RoleCycle (); }
    assert (mUsb.WaitingDetach && mDiagnostic.DetachStableMs == 400);
    RoleCycle ();
    assert (!mUsb.WaitingDetach && mDiagnostic.Rearms == 1 && !mDiagnostic.RecoverySampleValid);
    assert (MmioCount == Before && InitCalls == 1 && !OffMmio);
    RoleCycle (); // Still detached: Init may inspect the detector but cannot energize.
    assert (!PowerActive && SessionWrites == 1 && MmioCount == Before);
    RoleAllowed = TRUE; Reg[MU_DEVCTL] = MU_DEV_HOST | MU_DEV_VBUS | MU_DEV_FS;
    RoleCycle ();
    assert (mUsb.Ready && mUsb.Published && SessionWrites == 2 && mDiagnostic.SessionAttempts == 2);
    assert (mDiagnostic.SessionStatus == EFI_SUCCESS && Installs == 2 && Connects == 1 && !OffMmio);
    for (unsigned I = 0; I < 10; ++I) { RoleCycle (); }
    assert (SessionWrites == 2 && mDiagnostic.Rearms == 1 && mDiagnostic.SessionAttempts == 2);
  }
  puts ("PASS 500ms fresh detached EIC-low history permits one new powered SESSION after reattachment");

  StartTimeout (0);
  for (unsigned Attempt = 2; Attempt <= 4; ++Attempt) {
    DetectState = 0;
    for (unsigned I = 0; I < 6; ++I) { RoleCycle (); }
    assert (!mUsb.WaitingDetach && mDiagnostic.Rearms == Attempt - 1);
    Reg[MU_DEVCTL] = 0; RoleCycle ();
    assert (mDiagnostic.SessionAttempts == Attempt && SessionWrites == Attempt);
    assert (mUsb.WaitingDetach && !mUsb.Fatal && !mDiagnostic.RecoveryExhausted);
  }
  assert (!mUsb.Fatal && mUsb.WaitingDetach && !mDiagnostic.RecoveryExhausted && !PowerActive);
  assert (QuiesceCalls == 4 && StopCalls == 0 && !mUsb.Published && Installs == 1);
  DetectState = 5;
  { unsigned Before = MmioCount, BeforeInit = InitCalls, BeforeDetect = DetectCalls;
    for (unsigned I = 0; I < 20; ++I) { RoleCycle (); }
    assert (MmioCount == Before && InitCalls == BeforeInit && DetectCalls > BeforeDetect && !OffMmio);
    assert (SessionWrites == 4 && mUsb.WaitingDetach && !mUsb.Fatal);
  }
  assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_SUCCESS && Diagnostic.SessionLimit == 0);
  puts ("PASS fourth powered SESSION is allowed only after a fresh 500ms detach; continuous attach cannot restart power");

  ResetModel (); CurrentTpl = TPL_CALLBACK; PowerActive = FALSE; Reg[MU_DEVCTL] = 0;
  QuiesceStatus = EFI_DEVICE_ERROR;
  assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS); RoleCycle ();
  assert (mUsb.Fatal && !mUsb.WaitingDetach && mDiagnostic.CleanupStatus == EFI_DEVICE_ERROR);
  assert (mDiagnostic.SessionDevCtlEnd == MU_DEV_SESSION && mDiagnostic.SessionSnapshotValidMask == 0x1F);
  { unsigned Before = MmioCount;
    for (unsigned I = 0; I < 10; ++I) { RoleCycle (); }
    assert (InitCalls == 1 && DetectCalls == 0 && MmioCount == Before && !OffMmio);
  }
  puts ("PASS uncertain rollback latches fatal and preserves failed SESSION diagnostics without further HC access");

  StartTimeout (0); DetectState = 0; RoleCycle ();
  DetectStatus = EFI_TIMEOUT; RoleCycle ();
  assert (mUsb.Fatal && !mUsb.WaitingDetach && !mDiagnostic.RecoverySampleValid && !mDiagnostic.Rearms);
  assert (mDiagnostic.RecoveryProbeStatus == EFI_TIMEOUT && StopCalls == 1 && !OffMmio);
  puts ("PASS detector transport failure is fatal and never substitutes zeroes for a detached observation");

  StartTimeout (0); DetectState = 0; RoleCycle ();
  CounterNs = 0; RoleCycle ();
  assert (mUsb.Fatal && !mUsb.WaitingDetach && !mDiagnostic.Rearms);
  assert (mDiagnostic.RecoveryProbeStatus == EFI_DEVICE_ERROR && !OffMmio);
  puts ("PASS counter reversal cannot certify the 500ms unplug interval");

  ResetModel (); mDiagnostic.SessionAttempts = 3;
  for (unsigned I = 0; I < 8; ++I) {
    assert (Reset (&mUsb.Hc, EFI_USB_HC_RESET_GLOBAL) == EFI_SUCCESS);
    assert (mUsb.Ready && mDiagnostic.SessionAttempts == 3 && !mDiagnostic.RecoveryExhausted);
  }
  assert (SessionWrites == 8 && !InitCalls && !QuiesceCalls && !StopCalls && PowerActive && !OffMmio);
  Reg[MU_DEVCTL] = 0;
  assert (Reset (&mUsb.Hc, EFI_USB_HC_RESET_HOST_CONTROLLER) == EFI_NOT_READY);
  assert (!mUsb.Fatal && mUsb.WaitingDetach && !mDiagnostic.RecoveryExhausted && mDiagnostic.SessionAttempts == 3 && !PowerActive && !OffMmio);
  puts ("PASS normal powered HC resets do not increment attempts; timeout still shuts down and requires detach");

  ResetModel (); mDiagnostic.SessionAttempts = 1; Reg[MU_DEVCTL] = 0;
  assert (Reset (&mUsb.Hc, EFI_USB_HC_RESET_GLOBAL) == EFI_NOT_READY);
  assert (mUsb.WaitingDetach && !mUsb.Fatal && mDiagnostic.SessionAttempts == 1 && QuiesceCalls == 1);
  assert (mDiagnostic.SessionWaitUs == 100000 && !OffMmio);
  puts ("PASS a failed powered HC reset goes through the same OFF and stable-detach gate without spending a new power-on");

  ResetModel (); CurrentTpl = TPL_CALLBACK; GateOnInitRestore = TRUE;
  assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS); RoleCycle ();
  assert (!mUsb.Ready && !mUsb.Published && !PowerActive && !GatePending && !OffMmio);
  assert (mDiagnostic.SessionAttempts == 1 && mDiagnostic.SessionStatus == EFI_SUCCESS);
  puts ("PASS platform monitor cannot power off the HC between Init success and protected configuration");

  ResetModel (); CurrentTpl = TPL_CALLBACK; InitStatus = EFI_DEVICE_ERROR;
  assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS); RoleCycle ();
  assert (mUsb.Fatal && !mUsb.Published && mDiagnostic.Status == EFI_DEVICE_ERROR);
  for (unsigned I = 0; I < 10; ++I) { RoleCycle (); }
  assert (InitCalls == 1 && !MmioCount && !OffMmio && StopCalls == 1 && !mDiagnostic.SessionAttempts);
  puts ("PASS platform initialization transport failure latches fatal without HC MMIO or repeated power attempts");

  // Reproduce the photographed 0x19 condition, then model HM appearing later.
  ResetModel (); CurrentTpl = TPL_CALLBACK; PowerActive = FALSE;
  Reg[MU_DEVCTL] = MU_DEV_VBUS; Reg[MU_TESTMODE] = 0x08;
  assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS); RoleCycle ();
  assert (mUsb.SessionPending && PowerActive && !mUsb.Ready && !mUsb.Published);
  assert (mDiagnostic.SessionWaitUs == 100000 && DelayUs == 100000 && InitCalls == 1);
  assert (mDiagnostic.PeerSnapshotValidMask == (MOTO_G20_USB_PEER_INITIAL_VALID | MOTO_G20_USB_PEER_100MS_VALID));
  assert (mDiagnostic.PeerDevCtl100Ms == 0x19 && mDiagnostic.SessionPowerInitial == MU_POWER_HSEN);
  assert (mDiagnostic.SessionTestModeInitial == 0x08 && Reg[MU_TESTMODE] == 0x08);
  assert (ReadDiagnostic (&mDiagnosticProtocol, &Diagnostic) == EFI_SUCCESS);
  assert (Diagnostic.SessionPending && Diagnostic.SessionDeadlineUs == 1100000);
  { unsigned Before = MmioCount;
    assert (Reset (&mUsb.Hc, EFI_USB_HC_RESET_GLOBAL) == EFI_NOT_READY);
    assert (GetPort (&mUsb.Hc, 0, &Port) == EFI_SUCCESS && !(Port.PortStatus & USB_PORT_STAT_POWER));
    assert (MmioCount == Before && Installs == 1 && !Connects);
  }
  for (unsigned I = 0; I < 5; ++I) { RoleCycle (); }
  assert (mUsb.SessionPending && mDiagnostic.SessionWaitUs == 600000 && !mUsb.Published);
  assert (InitCalls == 1 && SessionWrites == 1 && DelayUs == 100000);
  Reg[MU_DEVCTL] |= MU_DEV_HOST | MU_DEV_FS;
  RoleCycle ();
  assert (!mUsb.SessionPending && mUsb.Ready && mUsb.Published && Connects == 1);
  assert (mDiagnostic.FirstHostUs == 700000 && mDiagnostic.FirstSpeedUs == 700000);
  assert (mDiagnostic.PeerDevCtl100Ms == 0x19 && mDiagnostic.SessionStatus == EFI_SUCCESS && !OffMmio);
  puts ("PASS delayed HM at 700ms publishes only after qualification; initial/100ms samples and observation times survive");

  // A late notification must use elapsed time, not assume one more millisecond.
  ResetModel (); CurrentTpl = TPL_CALLBACK; Reg[MU_DEVCTL] = MU_DEV_VBUS;
  assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS); RoleCycle ();
  CounterNs = mUsb.SessionSince + 1400000000ULL; Reg[MU_DEVCTL] |= MU_DEV_HOST;
  TimerTick (NULL, NULL);
  assert (!mUsb.SessionPending && mUsb.WaitingDetach && !mUsb.Published && !PowerActive);
  assert (mDiagnostic.SessionWaitUs == 1400000 && mDiagnostic.FirstHostUs == 1400000);
  assert (mDiagnostic.SessionStatus == EFI_NOT_READY && QuiesceCalls == 1 && !OffMmio);
  puts ("PASS late timer rejects HM observed beyond wall-clock deadline and shuts down once");

  for (unsigned Mode = 0; Mode < 2; ++Mode) {
    ResetModel (); CurrentTpl = TPL_CALLBACK; Reg[MU_DEVCTL] = MU_DEV_VBUS;
    assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS); RoleCycle ();
    if (Mode == 0) { RoleAllowed = FALSE; } else { PowerActive = FALSE; }
    { unsigned Before = MmioCount;
      TimerTick (NULL, NULL);
      assert (MmioCount == Before && !OffMmio && !mUsb.SessionPending && !mUsb.Ready);
      assert (mUsb.WaitingDetach && !PowerActive && mDiagnostic.SessionAttempts == 1);
    }
  }
  puts ("PASS fresh role loss or prior monitor gate while pending makes zero subsequent HC accesses");

  ResetModel (); CurrentTpl = TPL_CALLBACK; GateOnInitRestore = TRUE; Reg[MU_DEVCTL] = MU_DEV_VBUS;
  assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS); RoleCycle ();
  assert (mUsb.SessionPending && !PowerActive && !OffMmio);
  { unsigned Before = MmioCount;
    TimerTick (NULL, NULL);
    assert (MmioCount == Before && !OffMmio && !mUsb.Published && !mUsb.SessionPending);
  }
  puts ("PASS monitor deferred by initial TPL guard can gate before next pending tick without stale MMIO");

  ResetModel (); CurrentTpl = TPL_CALLBACK; Reg[MU_DEVCTL] = MU_DEV_VBUS;
  assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS); RoleCycle ();
  CounterNs = 0; TimerTick (NULL, NULL);
  assert (mUsb.Fatal && !mUsb.SessionPending && !PowerActive && mDiagnostic.SessionStatus == EFI_DEVICE_ERROR);
  assert (StopCalls == 1 && !QuiesceCalls && !OffMmio);
  puts ("PASS pending counter reversal is fatal rather than extending or completing the wait");

  for (unsigned Mode = 0; Mode < 3; ++Mode) {
    ResetModel (); CurrentTpl = TPL_CALLBACK; Reg[MU_DEVCTL] = MU_DEV_VBUS;
    assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS); RoleCycle ();
    if (Mode == 0) { Reg[MU_DEVCTL] |= MU_DEV_BDEVICE; }
    if (Mode == 1) { Reg[MU_DEVCTL] &= ~MU_DEV_VBUS; }
    if (Mode == 2) { Reg[MU_OTG_EXT_CSR] = 0; }
    TimerTick (NULL, NULL);
    assert (mUsb.WaitingDetach && !mUsb.SessionPending && !PowerActive && !mUsb.Published && !OffMmio);
  }
  puts ("PASS BDEVICE, VBUS sense loss or lost HOST_FORCE cancels pending connection immediately");

  for (unsigned Mode = 0; Mode < 2; ++Mode) {
    ResetModel (); CurrentTpl = TPL_NOTIFY; Reg[MU_DEVCTL] = MU_DEV_VBUS;
    assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS); RoleCycle ();
    if (Mode) { PowerActive = FALSE; }
    { unsigned Before = MmioCount;
      ExitBoot (NULL, NULL);
      assert (!PowerActive && !mUsb.SessionPending && mUsb.Ended && !OffMmio);
      if (Mode) { assert (MmioCount == Before); }
      Before = MmioCount; TimerTick (NULL, NULL); assert (MmioCount == Before);
    }
  }
  puts ("PASS ExitBoot shuts down a pending source and skips HC registers if the platform already gated");

  ResetModel (); Reg[MU_DEVCTL] = MU_DEV_VBUS; mDiagnostic.SessionAttempts = 1;
  assert (Reset (&mUsb.Hc, EFI_USB_HC_RESET_GLOBAL) == EFI_NOT_READY);
  assert (!mUsb.SessionPending && mUsb.WaitingDetach && !PowerActive && DelayUs == 100000);
  assert (mDiagnostic.SessionAttempts == 1 && !OffMmio);
  puts ("PASS synchronous UEFI Reset never returns with an unqualified asynchronous session active");

  { const EFI_STATUS Errors[] = {EFI_TIMEOUT, EFI_NO_RESPONSE, EFI_DEVICE_ERROR};
    for (unsigned I = 0; I < sizeof (Errors) / sizeof (Errors[0]); ++I) {
      ResetModel (); CurrentTpl = TPL_CALLBACK; Reg[MU_DEVCTL] = MU_DEV_VBUS;
      assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS); RoleCycle ();
      PollStatus = Errors[I];
      { unsigned Before = MmioCount;
        TimerTick (NULL, NULL);
        assert (MmioCount == Before && !OffMmio && mUsb.Fatal && !mUsb.SessionPending && !PowerActive);
        assert (mDiagnostic.SessionStatus == Errors[I] && StopCalls == 1 && !QuiesceCalls);
      }
    }
  }
  puts ("PASS pending transport, source-voltage and platform faults remain fatal with zero HC MMIO after gating");

  StartTimeout (MU_DEV_VBUS);
  for (unsigned Attempt = 2; Attempt <= 4; ++Attempt) {
    DetectState = 0;
    for (unsigned I = 0; I < 6; ++I) { RoleCycle (); }
    assert (!mUsb.WaitingDetach && mDiagnostic.Rearms == Attempt - 1);
    Reg[MU_DEVCTL] = MU_DEV_VBUS; RoleCycle ();
    assert (mUsb.SessionPending && mDiagnostic.SessionAttempts == Attempt);
    CounterNs = mUsb.SessionSince + 1100000000ULL; TimerTick (NULL, NULL);
    assert (mUsb.WaitingDetach && !mUsb.Fatal && !mDiagnostic.RecoveryExhausted && !PowerActive);
  }
  assert (!mUsb.Fatal && !mDiagnostic.RecoveryExhausted && !mUsb.SessionPending && !PowerActive);
  assert (SessionWrites == 4 && InitCalls == 4 && QuiesceCalls == 4 && StopCalls == 0 && !OffMmio);
  puts ("PASS fourth asynchronous attempt retains 1100ms deadline and powers off for a new detach");

  ResetModel (); CurrentTpl = TPL_CALLBACK; Reg[MU_DEVCTL] = MU_DEV_VBUS;
  assert (MotoG20MusbEntryPoint (NULL, NULL) == EFI_SUCCESS); RoleCycle ();
  Reg[MU_DEVCTL] |= MU_DEV_FS; RoleCycle ();
  assert (mUsb.SessionPending && mDiagnostic.FirstSpeedUs == 200000);
  assert (!(mDiagnostic.PeerSnapshotValidMask & MOTO_G20_USB_PEER_FIRST_HOST_VALID));
  CounterNs = mUsb.SessionSince + 1100000000ULL; TimerTick (NULL, NULL);
  assert (mUsb.WaitingDetach && !mUsb.Published && mDiagnostic.FirstSpeedUs == 200000 && !OffMmio);
  puts ("PASS FS/LS observation alone cannot publish host; first speed time survives later timeout");
  TestV5Diagnostics ();
  TestV6Ep0Trace ();
  return 0;
}
