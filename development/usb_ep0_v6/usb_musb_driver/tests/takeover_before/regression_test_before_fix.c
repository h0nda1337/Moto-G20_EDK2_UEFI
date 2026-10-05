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
static unsigned char Source[64], CallbackData[1024];
static unsigned SourceLength;
static BOOLEAN PowerActive, RoleAllowed, CancelInCallback;
static BOOLEAN CancelOnRestore, RecreateOnRestore;
static unsigned RoleLossAtUs;
static EFI_STATUS PrepareStatus;
static EFI_TPL CurrentTpl;
static unsigned FifoConfigWrites, EndpointWrites;
static BOOLEAN CheckStoppedSession, BlockSessionStop;

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
  if (RoleLossAtUs && DelayUs >= RoleLossAtUs) { RoleAllowed = FALSE; }
  return Us;
}
VOID *EFIAPI ZeroMem (VOID *Buffer, UINTN Length) { return memset (Buffer, 0, Length); }
VOID *EFIAPI CopyMem (VOID *Dest, CONST VOID *SourceBuffer, UINTN Length) { return memcpy (Dest, SourceBuffer, Length); }

EFI_STATUS EFIAPI MotoG20UsbPlatformPrepare (VOID) { return PrepareStatus; }
EFI_STATUS EFIAPI MotoG20UsbPlatformInit (UINTN *Base) {
  if (!RoleAllowed) { *Base = 0; return EFI_NOT_READY; }
  PowerActive = TRUE; *Base = 0x5FFF0000U; return EFI_SUCCESS;
}
EFI_STATUS EFIAPI MotoG20UsbPlatformPollRole (BOOLEAN *Active) {
  *Active = PowerActive && RoleAllowed;
  if (!*Active) { PowerActive = FALSE; return EFI_NOT_READY; }
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI MotoG20UsbPlatformStop (VOID) { PowerActive = FALSE; return EFI_SUCCESS; }
EFI_STATUS EFIAPI MotoG20UsbPlatformShutdown (VOID) { PowerActive = FALSE; return EFI_SUCCESS; }
VOID EFIAPI MotoG20UsbPlatformGetDiagnostic (MOTO_G20_USB_PLATFORM_STAGE *Stage, EFI_STATUS *Status, UINT16 *State, BOOLEAN *Vbus) {
  *Stage = MotoUsbStageRunning; *Status = EFI_SUCCESS; *State = RoleAllowed ? 5 : 2; *Vbus = PowerActive;
}
static EFI_TPL EFIAPI Raise (EFI_TPL New) { EFI_TPL Old = CurrentTpl; assert (New >= Old); CurrentTpl = New; return Old; }
static EFI_STATUS EFIAPI OnReport (VOID *Data, UINTN Length, VOID *Context, UINT32 Result);
static VOID EFIAPI Restore (EFI_TPL Old) {
  CurrentTpl = Old;
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
  PowerActive = RoleAllowed = TRUE; CancelInCallback = FALSE; PrepareStatus = EFI_SUCCESS;
  CancelOnRestore = RecreateOnRestore = FALSE; RoleLossAtUs = 0;
  CurrentTpl = TPL_APPLICATION;
  FifoConfigWrites = EndpointWrites = 0;
  CheckStoppedSession = BlockSessionStop = FALSE;
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
static EFI_STATUS EFIAPI OnReport (VOID *Data, UINTN Length, VOID *Context, UINT32 Result) {
  UINT8 Toggle = 0;
  ++Callbacks;
  if (Result) { ++CallbackErrors; }
  if (Length) { memcpy (CallbackData, Data, Length); }
  if (CancelInCallback) {
    assert (AsyncInterrupt (&mUsb.Hc, 1, 0x81, EFI_USB_SPEED_FULL, 8, FALSE, &Toggle, 0, 0, NULL, NULL, NULL) == EFI_SUCCESS);
    assert (Toggle == 1);
  }
  return EFI_SUCCESS;
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
      assert (ConfigureController () == EFI_ACCESS_DENIED);
      assert (FifoConfigWrites == 0 && EndpointWrites == 0);
    }
  }
  for (unsigned Ep = 5; Ep <= 15; ++Ep) {
    const unsigned Bits[] = {0x0001, 0x0020, 0x4000};
    for (unsigned Bit = 0; Bit < 3; ++Bit) {
      ResetModel (); EpReg[Ep][6] = (unsigned char)Bits[Bit]; EpReg[Ep][7] = (unsigned char)(Bits[Bit] >> 8);
      assert (ConfigureController () == EFI_ACCESS_DENIED);
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
  assert (ConfigureController () == EFI_SUCCESS && FifoConfigWrites > 0);
  assert ((Reg[MU_DEVCTL] & MU_DEV_SESSION) && (Reg[MU_OTG_EXT_CSR] & MU_HOST_FORCE));
  ResetModel (); Reg[MU_OTG_EXT_CSR] = MU_HOST_FORCE; BlockSessionStop = TRUE;
  assert (ConfigureController () == EFI_DEVICE_ERROR);
  assert (FifoConfigWrites == 0 && EndpointWrites == 0);
  puts ("PASS session is stopped before FIFO reassignment and failed stop prevents takeover");

  ResetModel (); Reg[MU_DMA_EN_STATUS] = 1;
  assert (ConfigureController () == EFI_ACCESS_DENIED && mDiagnostic.ControllerStep == 2);
  ResetModel (); assert (ConfigureController () == EFI_SUCCESS);
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
  assert (Installs == 1 && !mUsb.Published && mUsb.Fatal && mDiagnostic.ControllerStep == 4 && mDiagnostic.Status == EFI_NOT_READY);
  puts ("PASS publication requires real controller qualification; power-off persists and clears host session");
  return 0;
}
