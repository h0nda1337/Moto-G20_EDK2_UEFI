/* Actual observer + extracted actual producer; simulated boot services only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#undef NULL
#include "MotoG20UsbConsoleStatus.c"

EFI_GUID gEfiUsbIoProtocolGuid = EFI_USB_IO_PROTOCOL_GUID;
EFI_GUID gEfiSimpleTextInProtocolGuid = EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
EFI_GUID gEfiConsoleInDeviceGuid = EFI_CONSOLE_IN_DEVICE_GUID;
EFI_GUID gEfiEventExitBootServicesGuid = EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
static EFI_BOOT_SERVICES Bs;
static EFI_SYSTEM_TABLE St;
EFI_BOOT_SERVICES *gBS = &Bs;
EFI_SYSTEM_TABLE *gST = &St;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Output;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL Input;
#define REPORT_PRINTS 20U
static char Transcript[131072];
static size_t TranscriptSize;
static unsigned Present, Text, Routed, Prints, Live, Closed, Corrupt, Reads;
static EFI_STATUS RoleResult, LocateResult;
static EFI_TPL CurrentTpl;
static MOTO_G20_USB_DIAGNOSTIC_STATUS Snapshot, mDiagnostic;
static struct { BOOLEAN Ready, Published, Connected, Enabled, WaitingDetach, Fatal, SessionPending; } mUsb;
static MOTO_G20_USB_DIAGNOSTIC_PROTOCOL mDiagnosticProtocol;
static EFI_HANDLE Physical = (void *)1, Virtual = (void *)2;
#define MOCK_INTERFACES 8U
typedef struct {
  EFI_USB_DEVICE_DESCRIPTOR Device;
  EFI_USB_INTERFACE_DESCRIPTOR Interface;
  EFI_STATUS IoStatus, DeviceStatus, InterfaceStatus, TextStatus, RouteStatus;
  BOOLEAN Text, Routed;
} MOCK_INTERFACE;
static MOCK_INTERFACE Devices[MOCK_INTERFACES];
static EFI_USB_IO_PROTOCOL UsbProtocols[MOCK_INTERFACES];
static EFI_STATUS InventoryLocateResult;
static unsigned ReverseHandles, InventoryReads, DescriptorReads, DuplicateRoutes;

static void *Allocate(UINTN Size) { void *P = malloc(Size); assert(P); ++Live; return P; }
VOID EFIAPI FreePool(VOID *P) { assert(P && Live); --Live; free(P); }
VOID *EFIAPI ZeroMem(VOID *P, UINTN N) { return memset(P, 0, N); }
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D, S, N); }
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN N) { return memcmp(A, B, N); }
static void Append(const char *Text) {
  size_t Length = strlen(Text);
  assert(TranscriptSize + Length < sizeof Transcript);
  memcpy(Transcript + TranscriptSize, Text, Length + 1); TranscriptSize += Length;
}
/* Decode only the actual observer's UEFI formats; libc wchar_t is not CHAR16. */
UINTN EFIAPI Print(CONST CHAR16 *Format, ...) {
  va_list Args;
  char Buffer[128];
  assert(Format); ++Prints; va_start(Args, Format);
  while (*Format != 0) {
    unsigned Width = 0, Long = 0;
    if (*Format != '%') { Buffer[0] = (char)*Format++; Buffer[1] = 0; Append(Buffer); continue; }
    ++Format;
    while (*Format >= '0' && *Format <= '9') { Width = Width * 10 + *Format++ - '0'; }
    if (*Format == 'L') { Long = 1; ++Format; }
    switch (*Format++) {
      case 'u':
        if (Long) snprintf(Buffer, sizeof Buffer, "%llu", (unsigned long long)va_arg(Args, UINT64));
        else snprintf(Buffer, sizeof Buffer, "%u", va_arg(Args, UINT32));
        Append(Buffer); break;
      case 'x': snprintf(Buffer, sizeof Buffer, "%0*x", (int)Width, va_arg(Args, UINT32)); Append(Buffer); break;
      case 'r': snprintf(Buffer, sizeof Buffer, "status-%llx", (unsigned long long)va_arg(Args, EFI_STATUS)); Append(Buffer); break;
      case 's': {
        CONST CHAR16 *String = va_arg(Args, CONST CHAR16 *);
        while (*String != 0) { Buffer[0] = (char)*String++; Buffer[1] = 0; Append(Buffer); }
        break;
      }
      default: assert(!"unsupported observer format");
    }
  }
  va_end(Args); return 1;
}
static EFI_STATUS EFIAPI ReadKey(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key) {
  (void)This; (void)Key; assert(!"observer must not consume keys"); return EFI_NOT_READY;
}
static EFI_TPL EFIAPI Raise(EFI_TPL NewTpl) {
  EFI_TPL Old = CurrentTpl; assert(NewTpl >= Old); CurrentTpl = NewTpl; return Old;
}
static VOID EFIAPI Restore(EFI_TPL OldTpl) { assert(OldTpl <= CurrentTpl); CurrentTpl = OldTpl; }
VOID EFIAPI MotoG20UsbPlatformGetDiagnostic(MOTO_G20_USB_PLATFORM_STAGE *Stage,
  EFI_STATUS *Status, UINT16 *State, BOOLEAN *Vbus) {
  assert(CurrentTpl == TPL_NOTIFY);
  *Stage = (MOTO_G20_USB_PLATFORM_STAGE)Snapshot.PlatformStage;
  *Status = Snapshot.PlatformStatus; *State = Snapshot.TypecState; *Vbus = Snapshot.VbusPresent;
}
EFI_STATUS EFIAPI MotoG20UsbPlatformGetRoleDiagnostic(MOTO_G20_USB_ROLE_DIAGNOSTIC *Role) {
  assert(CurrentTpl == TPL_NOTIFY); *Role = Snapshot.Role; return RoleResult;
}
#include "producer_under_test.h"
static EFI_STATUS EFIAPI GetStatus(MOTO_G20_USB_DIAGNOSTIC_PROTOCOL *This,
  MOTO_G20_USB_DIAGNOSTIC_STATUS *Status) {
  EFI_STATUS Result;
  ++Reads; mDiagnostic = Snapshot;
  mUsb.Ready = Snapshot.HostReady; mUsb.Published = Snapshot.HcPublished;
  mUsb.Connected = Snapshot.PortConnected; mUsb.Enabled = Snapshot.PortEnabled;
  mUsb.WaitingDetach = Snapshot.RecoveryWaiting; mUsb.Fatal = Snapshot.FatalLatched;
  mUsb.SessionPending = Snapshot.SessionPending;
  Result = ReadDiagnostic(This, Status);
  assert(CurrentTpl == TPL_CALLBACK);
  if (Corrupt == 1) Status->Revision = 1;
  if (Corrupt == 2) --Status->Size;
  if (Corrupt == 3) Status->Role.Revision = 0;
  if (Corrupt == 4) --Status->Role.Size;
  return Result;
}
static EFI_STATUS EFIAPI LocateHandles(EFI_LOCATE_SEARCH_TYPE Search, EFI_GUID *Guid,
  VOID *Reg, UINTN *Count, EFI_HANDLE **Handles) {
  UINTN I;
  assert(Search == ByProtocol && Guid == &gEfiUsbIoProtocolGuid && Reg == NULL);
  ++InventoryReads;
  if (EFI_ERROR(InventoryLocateResult)) return InventoryLocateResult;
  if (!Present) return EFI_NOT_FOUND;
  assert(Present <= MOCK_INTERFACES);
  *Count = Present; *Handles = Allocate(Present * sizeof(EFI_HANDLE));
  for (I = 0; I < Present; ++I) (*Handles)[I] = (void *)((UINTN)Physical + (ReverseHandles ? Present - 1 - I : I));
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI DeviceDescriptor(EFI_USB_IO_PROTOCOL *This, EFI_USB_DEVICE_DESCRIPTOR *Descriptor) {
  UINTN I = This - UsbProtocols; assert(I < Present); ++DescriptorReads;
  if (EFI_ERROR(Devices[I].DeviceStatus)) return Devices[I].DeviceStatus;
  *Descriptor = Devices[I].Device; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI InterfaceDescriptor(EFI_USB_IO_PROTOCOL *This, EFI_USB_INTERFACE_DESCRIPTOR *Descriptor) {
  UINTN I = This - UsbProtocols; assert(I < Present); ++DescriptorReads;
  if (EFI_ERROR(Devices[I].InterfaceStatus)) return Devices[I].InterfaceStatus;
  *Descriptor = Devices[I].Interface; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H, EFI_GUID *Guid, VOID **Interface) {
  UINTN I = (UINTN)H - (UINTN)Physical; assert(I < Present);
  if (Guid == &gEfiUsbIoProtocolGuid) {
    if (EFI_ERROR(Devices[I].IoStatus)) return Devices[I].IoStatus;
    *Interface = &UsbProtocols[I]; return EFI_SUCCESS;
  }
  assert(Guid == &gEfiSimpleTextInProtocolGuid);
  if (EFI_ERROR(Devices[I].TextStatus)) return Devices[I].TextStatus;
  *Interface = &Input; return (Text || Devices[I].Text) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI OpenInfo(EFI_HANDLE H, EFI_GUID *Guid,
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **Info, UINTN *Count) {
  UINTN I = (UINTN)H - (UINTN)Physical, Entry; assert(I < Present);
  assert(Guid == &gEfiConsoleInDeviceGuid);
  if (EFI_ERROR(Devices[I].RouteStatus)) return Devices[I].RouteStatus;
  *Count = DuplicateRoutes ? 2 : 1;
  *Info = Allocate(*Count * sizeof **Info); memset(*Info, 0, *Count * sizeof **Info);
  for (Entry = 0; Entry < *Count; ++Entry) {
    (*Info)[Entry].ControllerHandle = (Routed || Devices[I].Routed) ? Virtual : (void *)4;
    (*Info)[Entry].Attributes = EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid, VOID *Reg, VOID **Interface) {
  assert(Guid == &mDiagnosticGuid && Reg == NULL);
  *Interface = &mDiagnosticProtocol; return LocateResult;
}
static EFI_STATUS EFIAPI Close(EFI_EVENT Event) { assert(Event); ++Closed; return EFI_SUCCESS; }
static void Tick(void) { ReportUsbStatus(NULL, NULL); assert(Live == 0 && CurrentTpl == TPL_CALLBACK); }
static void Reset(void) {
  UINTN I;
  assert(Live == 0);
  memset(&Bs, 0, sizeof Bs); memset(&St, 0, sizeof St);
  memset(&Snapshot, 0, sizeof Snapshot); memset(&mPrevious, 0, sizeof mPrevious);
  memset(&mUsb, 0, sizeof mUsb); Transcript[0] = 0; TranscriptSize = 0;
  memset(&mPreviousInventory, 0, sizeof mPreviousInventory);
  memset(Devices, 0, sizeof Devices); memset(UsbProtocols, 0, sizeof UsbProtocols);
  for (I = 0; I < MOCK_INTERFACES; ++I) {
    UsbProtocols[I].UsbGetDeviceDescriptor = DeviceDescriptor;
    UsbProtocols[I].UsbGetInterfaceDescriptor = InterfaceDescriptor;
  }
  InventoryLocateResult = EFI_SUCCESS; ReverseHandles = InventoryReads = DescriptorReads = DuplicateRoutes = 0;
  Present = Text = Routed = Prints = Closed = Corrupt = Reads = 0;
  mStatusTimer = mExitEvent = NULL; mEnded = mHaveStatus = FALSE;
  mTicks = mFaultKind = mLastReportTick = 0; mFaultStatus = 0;
  Bs.LocateHandleBuffer = LocateHandles; Bs.HandleProtocol = Handle;
  Bs.OpenProtocolInformation = OpenInfo; Bs.LocateProtocol = Locate; Bs.CloseEvent = Close;
  Bs.RaiseTPL = Raise; Bs.RestoreTPL = Restore;
  Input.ReadKeyStroke = ReadKey; St.ConIn = &Input;
  St.ConOut = &Output; St.ConsoleInHandle = Virtual; CurrentTpl = TPL_CALLBACK;
  mDiagnosticProtocol.Revision = MOTO_G20_USB_DIAGNOSTIC_REVISION;
  mDiagnosticProtocol.GetStatus = GetStatus; RoleResult = LocateResult = EFI_SUCCESS;
  Snapshot.Revision = MOTO_G20_USB_DIAGNOSTIC_REVISION; Snapshot.Size = sizeof Snapshot;
  Snapshot.Role.Revision = MOTO_G20_USB_ROLE_DIAGNOSTIC_REVISION;
  Snapshot.Role.Size = sizeof Snapshot.Role;
}
static void Waiting(void) {
  Snapshot.Stage = MotoUsbHcStageWaitRole; Snapshot.Status = EFI_NOT_READY;
  Snapshot.PlatformStage = MotoUsbStageTypec; Snapshot.PlatformStatus = EFI_NOT_READY;
  Snapshot.Role.ValidMask = MOTO_G20_USB_ROLE_ALL_VALID;
}
int main(void) {
  unsigned I, Before;
  size_t Mark;
  USB_INVENTORY Inventory, Previous;
  Reset(); ReadUsbInventory(&Inventory); assert(Inventory.RoutedInputs == 0 && Live == 0);
  Present = 1; ReadUsbInventory(&Inventory); assert(Inventory.Interfaces == 1 && Inventory.RoutedInputs == 0 && Live == 0);
  Text = 1; ReadUsbInventory(&Inventory); assert(Inventory.TextInputs == 1 && Inventory.RoutedInputs == 0 && Live == 0);
  Routed = 1; DuplicateRoutes = 1; ReadUsbInventory(&Inventory);
  assert(Inventory.RoutedInputs == 1 && Live == 0);
  puts("PASS physical input requires USB + TextIn + actual ConIn child routing");
  Reset(); Present = 5;
  Devices[0].Interface.InterfaceClass = 9;
  Devices[1].Interface.InterfaceClass = 3;
  Devices[2].Interface.InterfaceClass = Devices[3].Interface.InterfaceClass = 3;
  Devices[2].Interface.InterfaceSubClass = Devices[3].Interface.InterfaceSubClass = 1;
  Devices[2].Interface.InterfaceProtocol = Devices[3].Interface.InterfaceProtocol = 1;
  Devices[2].Text = Devices[2].Routed = TRUE;
  Devices[2].Device.IdVendor = Devices[3].Device.IdVendor = 0x046d;
  Devices[2].Device.IdProduct = Devices[3].Device.IdProduct = 0xc31c;
  Devices[2].Interface.InterfaceNumber = 1; Devices[3].Interface.InterfaceNumber = 2;
  ReadUsbInventory(&Inventory);
  assert(Inventory.Interfaces == 5 && Inventory.Hubs == 1 && Inventory.Hid == 3);
  assert(Inventory.BootKeyboards == 2 && Inventory.TextInputs == 1 && Inventory.RoutedInputs == 1);
  assert(Inventory.ErrorCount == 0 && Inventory.DetailCount == 4 && DescriptorReads == 10 && Live == 0);
  PrintUsbInventory(&Inventory);
  assert(strstr(Transcript, "interfaces=5 hubs=1 HID=3 boot-kbd=2"));
  assert(strstr(Transcript, "id=046D:C31C n=0001 cls/sub/proto=0003/0001/0001 text=1 routed=1 err=00"));
  puts("PASS passive inventory separates hub, HID, boot keyboard, TextIn and current ConIn routing");
  Previous = Inventory; ReverseHandles = 1; ReadUsbInventory(&Inventory);
  assert(memcmp(&Inventory, &Previous, sizeof Inventory) == 0);
  Devices[4].Device.IdProduct = 0x1234; ReadUsbInventory(&Inventory);
  assert(Inventory.Fingerprint != Previous.Fingerprint);
  assert(memcmp(Inventory.Detail, Previous.Detail, sizeof Inventory.Detail) == 0 && Live == 0);
  puts("PASS bounded detail list is order independent and omitted interfaces still affect change detection");
  Reset(); Waiting(); mTicks = 4; Tick(); Before = Prints;
  assert(mPreviousInventory.LocateStatus == EFI_NOT_FOUND && mPreviousInventory.Interfaces == 0);
  InventoryLocateResult = EFI_OUT_OF_RESOURCES; Mark = TranscriptSize; Tick();
  assert(Prints == Before + REPORT_PRINTS && mPreviousInventory.LocateStatus == EFI_OUT_OF_RESOURCES);
  Tick(); assert(Prints == Before + REPORT_PRINTS);
  InventoryLocateResult = EFI_SUCCESS; Tick(); assert(Prints == Before + 2 * REPORT_PRINTS);
  assert(mPreviousInventory.LocateStatus == EFI_NOT_FOUND);
  puts("PASS no UsbIo handles differs from inventory failure; failure and recovery bypass waiting throttle");
  Reset(); Present = 4;
  Devices[0].IoStatus = EFI_DEVICE_ERROR;
  Devices[1].DeviceStatus = EFI_TIMEOUT; Devices[1].InterfaceStatus = EFI_NOT_FOUND;
  Devices[2].TextStatus = EFI_DEVICE_ERROR;
  Devices[3].Text = TRUE; Devices[3].RouteStatus = EFI_OUT_OF_RESOURCES;
  ReadUsbInventory(&Inventory);
  assert(Inventory.ErrorCount == 5 && Inventory.ErrorMask == 0x1f && Inventory.Interfaces == 4);
  assert(Inventory.TextInputs == 1 && Inventory.RoutedInputs == 0 && Live == 0);
  PrintUsbInventory(&Inventory);
  assert(strstr(Transcript, "id=----:---- n=---- cls/sub/proto=----/----/----"));
  assert(strstr(Transcript, "id=0000:0000 n=0000 cls/sub/proto=0000/0000/0000"));
  Previous = Inventory; ReverseHandles = 1; ReadUsbInventory(&Inventory);
  assert(memcmp(&Inventory, &Previous, sizeof Inventory) == 0);
  puts("PASS every inventory query error is explicit; unavailable descriptors differ from valid zero");
  Reset(); Waiting(); Present = 1; mTicks = 4; Tick(); Before = Prints;
  Devices[0].DeviceStatus = EFI_DEVICE_ERROR; Tick(); assert(Prints == Before + REPORT_PRINTS + 1);
  Devices[0].DeviceStatus = EFI_TIMEOUT; Tick(); assert(Prints == Before + 2 * (REPORT_PRINTS + 1));
  Devices[0].DeviceStatus = EFI_SUCCESS; Tick(); assert(Prints == Before + 3 * (REPORT_PRINTS + 1));
  Devices[0].Text = TRUE; Devices[0].RouteStatus = EFI_NOT_FOUND; Tick();
  assert(mPreviousInventory.TextInputs == 1 && mPreviousInventory.RoutedInputs == 0 && mPreviousInventory.ErrorCount == 0);
  Devices[0].RouteStatus = EFI_DEVICE_ERROR; Tick(); assert(mPreviousInventory.ErrorMask == USB_QUERY_ROUTE);
  Devices[0].RouteStatus = EFI_SUCCESS; Devices[0].Routed = TRUE; Tick();
  assert(mPreviousInventory.RoutedInputs == 1 && mPreviousInventory.ErrorCount == 0);
  puts("PASS descriptor and routing failures, changed error codes and identical recovery remain observable");
  Before = Prints;
  for (I = 0; I < 500; ++I) Tick();
  assert(Prints == Before && Live == 0);
  Devices[0].Device.IdVendor = 0xabcd; Tick(); assert(Prints == Before + REPORT_PRINTS + 1);
  puts("PASS repeated passive scans never print per poll; identity changes remain visible at unchanged counts");
  Reset(); for (I = 0; I < 4; ++I) Tick(); assert(Prints == 0 && Reads == 0);
  Tick(); assert(Prints == REPORT_PRINTS && Reads == 1); Tick(); assert(Prints == REPORT_PRINTS);
  Snapshot.InterruptReports = 1; Tick(); assert(Prints == 2 * REPORT_PRINTS);
  Snapshot.InterruptReports = 2; Tick(); assert(Prints == 2 * REPORT_PRINTS);
  Present = Text = Routed = 1; Tick(); assert(Prints == 3 * REPORT_PRINTS + 1);
  puts("PASS first report delayed, first IN and routing visible, repeated IN suppressed");
  Reset(); Waiting(); mTicks = 4; Tick(); assert(Prints == REPORT_PRINTS);
  for (I = 0; I < 1230; ++I) {
    static const UINT16 States[] = {9, 3, 0};
    Snapshot.TypecState = States[I % 3]; Snapshot.Role.TypecStatus = Snapshot.TypecState;
    Snapshot.Role.SeenStates |= 1U << Snapshot.TypecState;
    ++Snapshot.Role.Samples; ++Snapshot.Role.SnapshotAttempts;
    Snapshot.VbusPresent = (BOOLEAN)(I & 1);
    Snapshot.PlatformStage = (I & 1) ? MotoUsbStageExternalVbus : MotoUsbStageTypec;
    Tick(); if (I < 29) assert(Prints == REPORT_PRINTS);
  }
  assert(Prints == 2 * REPORT_PRINTS && mPrevious.Role.SeenStates == ((1U << 9) | (1U << 3) | 1U));
  puts("PASS 1230 waiting ticks: two snapshots total, DRP history kept without scroll flood");
  Snapshot.Role.RoleReadStatus = EFI_DEVICE_ERROR; Tick(); assert(Prints == 3 * REPORT_PRINTS);
  Snapshot.Role.RoleReadStatus = EFI_SUCCESS; Tick(); assert(Prints == 4 * REPORT_PRINTS);
  Snapshot.Role.ValidMask &= ~MOTO_G20_USB_ROLE_STATUS_VALID;
  Snapshot.Role.ErrorMask = MOTO_G20_USB_ROLE_STATUS_VALID;
  Snapshot.Role.SnapshotStatus = EFI_TIMEOUT; Tick(); assert(Prints == 5 * REPORT_PRINTS);
  puts("PASS role read failure, recovery and invalid cached fields bypass waiting throttle");
  Snapshot.Stage = MotoUsbHcStageConnect; Snapshot.Status = EFI_SUCCESS;
  Snapshot.HostReady = Snapshot.HcPublished = Snapshot.PortConnected = TRUE;
  Snapshot.Role.SourceSeen = 2; Snapshot.Role.BoostAttempts = 1;
  Tick(); assert(Prints == 6 * REPORT_PRINTS && mPrevious.HostReady && mPrevious.Role.BoostAttempts);
  puts("PASS host transition immediately visible after extended waiting");
  Reset(); Waiting(); mTicks = 4; Tick(); assert(Prints == REPORT_PRINTS);
  Snapshot.Role.IntRaw = Snapshot.Role.IntMask = 1;
  for (I = 0; I < 29; ++I) Tick();
  assert(Prints == REPORT_PRINTS); Tick(); assert(Prints == 2 * REPORT_PRINTS && mPrevious.Role.IntRaw == 1);
  puts("PASS RAW/MASK-only change retained and printed at waiting throttle deadline");
  {
    CHAR16 Hex[5]; CONST CHAR16 *Result = CachedHex(0, FALSE, Hex);
    assert(Result[0] == '-' && Result[1] == '-' && Result[2] == '-' && Result[3] == '-' && Result[4] == 0);
    Result = CachedHex(0, TRUE, Hex); assert(Result == Hex && Hex[0] == '0' && Hex[3] == '0' && Hex[4] == 0);
    Result = CachedHex(0x31ee, TRUE, Hex); assert(Hex[0] == '3' && Hex[1] == '1' && Hex[2] == 'E' && Hex[3] == 'E');
  }
  puts("PASS invalid cached values explicitly distinguished from valid zero");
  Reset(); mTicks = 4; Tick(); Before = Prints;
  RoleResult = EFI_DEVICE_ERROR; Tick(); Tick(); assert(Prints == Before + 1 && !mHaveStatus);
  RoleResult = EFI_SUCCESS; Tick(); assert(Prints == Before + 1 + REPORT_PRINTS && mHaveStatus);
  puts("PASS producer getter error invalidates epoch; identical successful recovery prints");
  for (I = 1; I <= 4; ++I) {
    Reset(); mTicks = 4; Corrupt = I; Tick(); Tick(); assert(Prints == 1 && !mHaveStatus);
    Corrupt = 0; Tick(); assert(Prints == 1 + REPORT_PRINTS && mHaveStatus);
  }
  puts("PASS consumer rejects outer/role revision and size mismatches; recovery visible");
  Reset(); mTicks = 4; Snapshot.Role.Revision = 99; Tick(); assert(Prints == 1 && !mHaveStatus);
  Snapshot.Role.Revision = MOTO_G20_USB_ROLE_DIAGNOSTIC_REVISION; Tick(); assert(Prints == 1 + REPORT_PRINTS);
  mDiagnosticProtocol.Revision = 1; Tick(); Tick(); assert(Prints == 2 + REPORT_PRINTS && !mHaveStatus);
  mDiagnosticProtocol.Revision = MOTO_G20_USB_DIAGNOSTIC_REVISION; Tick(); assert(Prints == 2 + 2 * REPORT_PRINTS);
  LocateResult = EFI_NOT_FOUND; Tick(); Tick(); assert(Prints == 3 + 2 * REPORT_PRINTS && !mHaveStatus);
  LocateResult = EFI_SUCCESS; Tick(); assert(Prints == 3 + 3 * REPORT_PRINTS);
  puts("PASS actual producer rejects role contract; old/absent protocol never reports USB success");
  Reset(); Waiting(); mTicks = 4;
  Snapshot.SessionAttempts = 1; Snapshot.SessionWaitUs = 250000;
  Snapshot.SessionStatus = EFI_TIMEOUT; Snapshot.CleanupStatus = EFI_SUCCESS;
  Snapshot.SessionSnapshotValidMask = MOTO_G20_USB_SESSION_ALL_VALID;
  Snapshot.SessionDevCtlBefore = 0x80; Snapshot.SessionDevCtlStart = 0x81;
  Snapshot.SessionDevCtlEnd = 0x81; Snapshot.SessionPower = 0x21; Snapshot.SessionOtg = 0x43;
  Snapshot.Role.SnapshotAttempts = 9;
  Snapshot.Role.DriverActive = Snapshot.Role.BoostRequested = TRUE;
  Tick(); Mark = TranscriptSize;
  Snapshot.Role.DriverActive = Snapshot.Role.BoostRequested = FALSE;
  Snapshot.Role.StopAttempts = 1; Snapshot.Role.LastStopStatus = EFI_SUCCESS;
  Snapshot.RecoveryWaiting = TRUE; Snapshot.RecoveryProbeStatus = EFI_NOT_READY;
  Tick(); assert(Prints == 2 * REPORT_PRINTS);
  assert(strstr(Transcript + Mark, "driver active=0 boost-requested=0 stops=1") != NULL);
  assert(strstr(Transcript + Mark, "HC cached-before-OFF valid=1f DEVCTL=0080/0081/0081 POWER=0021 OTG=0043") != NULL);
  assert(strstr(Transcript + Mark, "recovery waiting=1 fatal=0 rearms=0") != NULL);
  assert(strstr(Transcript + Mark, "recovery sample valid=0 TypeC=---- EIC=---- (cached while OFF)") != NULL);
  assert(mPrevious.RecoveryWaiting && !mPrevious.FatalLatched && mPrevious.SessionLimit == 0);
  puts("PASS stopped software ownership and historical HC samples remain distinct in printed output");
  Snapshot.RecoverySampleValid = TRUE; Snapshot.RecoveryTypecState = 0;
  Snapshot.RecoveryProbeStatus = EFI_SUCCESS; Mark = TranscriptSize;
  Tick(); assert(Prints == 3 * REPORT_PRINTS);
  assert(strstr(Transcript + Mark, "recovery sample valid=1 TypeC=0000 EIC=0000") != NULL);
  Before = Prints;
  for (I = 0; I < 499; ++I) {
    Snapshot.DetachStableMs = I;
    Snapshot.RecoveryTypecState = (I & 1) ? 0 : 3;
    Snapshot.RecoveryVbus = (BOOLEAN)(I & 1); ++Snapshot.Role.Samples;
    ++Snapshot.Role.StopAttempts; ++Snapshot.Role.SnapshotAttempts;
    Tick();
  }
  assert(Prints == Before);
  Snapshot.DetachStableMs = MOTO_G20_USB_RECOVERY_DETACH_MS; Tick();
  assert(Prints == Before + REPORT_PRINTS && mPrevious.DetachStableMs == 1);
  for (I = 0; I < 100; ++I) { ++Snapshot.DetachStableMs; Tick(); }
  assert(Prints == Before + REPORT_PRINTS);
  puts("PASS recovery debounce, DRP samples and repeated counters never flood console");
  Before = Prints;
  Snapshot.RecoveryProbeStatus = EFI_DEVICE_ERROR; Snapshot.RecoverySampleValid = FALSE;
  Mark = TranscriptSize; Tick(); assert(Prints == Before + REPORT_PRINTS);
  assert(strstr(Transcript + Mark, "recovery sample valid=0 TypeC=---- EIC=----") != NULL);
  Tick(); assert(Prints == Before + REPORT_PRINTS);
  Snapshot.RecoveryProbeStatus = EFI_SUCCESS; Snapshot.RecoverySampleValid = TRUE;
  Tick(); assert(Prints == Before + 2 * REPORT_PRINTS);
  Snapshot.CleanupStatus = EFI_DEVICE_ERROR; Tick(); assert(Prints == Before + 3 * REPORT_PRINTS);
  Snapshot.Role.LastStopStatus = EFI_DEVICE_ERROR; Snapshot.Role.BoostRequested = TRUE;
  Tick(); assert(Prints == Before + 4 * REPORT_PRINTS);
  puts("PASS recovery read failures, immediate recovery and uncertain OFF are visible without deduplication hiding errors");
  Before = Prints; Snapshot.RecoveryWaiting = FALSE; Snapshot.Rearms = 1;
  Snapshot.RecoverySampleValid = FALSE; Snapshot.DetachStableMs = 0;
  Snapshot.SessionAttempts = 2; Tick(); assert(Prints == Before + REPORT_PRINTS);
  Snapshot.SessionAttempts = 4; Mark = TranscriptSize;
  Tick(); assert(Prints == Before + 2 * REPORT_PRINTS);
  assert(strstr(Transcript + Mark, "session attempt=4 retry=after-detach") != NULL);
  assert(strstr(Transcript + Mark, "recovery waiting=0 fatal=0 rearms=1") != NULL);
  assert(strstr(Transcript + Mark, "exhausted=") == NULL);
  Snapshot.FatalLatched = TRUE; Tick(); assert(Prints == Before + 3 * REPORT_PRINTS);
  for (I = 0; I < 500; ++I) Tick();
  assert(Prints == Before + 3 * REPORT_PRINTS);
  puts("PASS rearm and unrestricted attempts remain visible; real fatal latch still reports once");
  Reset(); Waiting(); mTicks = 4; Mark = TranscriptSize;
  Snapshot.SessionSnapshotValidMask = MOTO_G20_USB_SESSION_DEVCTL_BEFORE_VALID;
  Snapshot.SessionDevCtlBefore = 0; Snapshot.SessionDevCtlStart = 0xaa;
  Snapshot.SessionDevCtlEnd = 0xbb; Snapshot.SessionPower = 0xcc; Snapshot.SessionOtg = 0xdd;
  Snapshot.Role.ValidMask = 0; Snapshot.Role.EfuseValid = FALSE;
  Tick();
  assert(strstr(Transcript + Mark, "HC cached-before-OFF valid=01 DEVCTL=0000/----/---- POWER=---- OTG=----") != NULL);
  assert(strstr(Transcript + Mark, "EN=---- MODE=---- IE=---- RAW=---- MASK=---- STATUS=----") != NULL);
  assert(strstr(Transcript + Mark, "efuse=---- valid=0 trim=----") != NULL);
  Before = Prints;
  for (I = 0; I < 100; ++I) {
    ++Snapshot.SessionDevCtlStart; ++Snapshot.SessionDevCtlEnd;
    ++Snapshot.SessionPower; ++Snapshot.SessionOtg; Tick();
  }
  assert(Prints == Before);
  Snapshot.SessionSnapshotValidMask = MOTO_G20_USB_SESSION_ALL_VALID; Tick();
  assert(Prints == Before + REPORT_PRINTS);
  puts("PASS invalid session and platform samples print dashes; invalid session bytes do not generate fake changes");
  Reset(); Waiting(); mTicks = 4; Snapshot.Stage = MotoUsbHcStageController;
  Snapshot.SessionPending = TRUE; Snapshot.SessionWaitUs = 100000;
  Snapshot.PeerSnapshotValidMask = MOTO_G20_USB_PEER_INITIAL_VALID | MOTO_G20_USB_PEER_100MS_VALID;
  Snapshot.SessionPowerInitial = 0x20; Snapshot.SessionTestModeInitial = 0;
  Snapshot.PeerDevCtl100Ms = 0x19; Snapshot.PeerPower100Ms = 0x60; Snapshot.PeerOtg100Ms = 1;
  Tick(); Before = Prints;
  assert(Prints == 0 && !mHaveStatus && Reads == 1 && InventoryReads == 0);
  for (I = 0; I < 100; ++I) { Snapshot.SessionWaitUs += 1000; Tick(); }
  assert(Prints == Before && InventoryReads == 0);
  Snapshot.PeerSnapshotValidMask |= MOTO_G20_USB_PEER_FIRST_HOST_VALID;
  Snapshot.FirstHostUs = 700000; Snapshot.SessionPending = FALSE;
  Snapshot.HostReady = TRUE; Mark = TranscriptSize; Tick();
  assert(Prints == Before + REPORT_PRINTS);
  assert(strstr(Transcript + Mark, "first-HM-us=700000 first-FSLS-us=----"));
  assert(strstr(Transcript + Mark, "peer pending=0 deadline-us=1100000 valid=07"));
  assert(strstr(Transcript + Mark, "peer cached POWER0=0020 TEST0=0000 at100ms DEV=0019 POWER=0060 OTG=0001"));
  puts("PASS console rendering deferred during pending connection; completed result preserves initial/100ms/timing samples");
  Reset(); Waiting(); mTicks = 4; Snapshot.FirstHostUs = 999999;
  Snapshot.PeerDevCtl100Ms = 0xff; Snapshot.SessionPowerInitial = 0xff; Tick();
  assert(strstr(Transcript, "first-HM-us=---- first-FSLS-us=----"));
  assert(strstr(Transcript, "peer cached POWER0=---- TEST0=---- at100ms DEV=---- POWER=---- OTG=----"));
  Before = Prints;
  for (I = 0; I < 50; ++I) { ++Snapshot.FirstHostUs; ++Snapshot.PeerDevCtl100Ms; Tick(); }
  assert(Prints == Before);
  Snapshot.PeerSnapshotValidMask = MOTO_G20_USB_PEER_FIRST_HOST_VALID;
  Snapshot.FirstHostUs = 0; Mark = TranscriptSize; Tick();
  assert(Prints == Before + REPORT_PRINTS && strstr(Transcript + Mark, "first-HM-us=0 first-FSLS-us=----"));
  puts("PASS invalid peer values suppressed; valid zero time differs from absent observation");

  Reset(); Waiting(); mTicks = 4;
  Snapshot.LastFailure.Status = EFI_DEVICE_ERROR;
  Snapshot.LastFailure.RequestType = 0xab;
  Snapshot.LastInterrupt.Address = 63; Snapshot.LastInterrupt.Length = 777;
  Snapshot.LastInterrupt.CallbackStatus = EFI_TIMEOUT;
  Tick(); Before = Prints;
  assert(strstr(Transcript, "last failure (history)=none"));
  assert(strstr(Transcript, "last IN=none"));
  ++Snapshot.LastFailure.RequestType; ++Snapshot.LastInterrupt.Length; Tick();
  assert(Prints == Before);
  puts("PASS invalid transfer history prints none and stale bytes cannot invent a change");

  Reset(); Waiting(); mTicks = 4;
  Snapshot.ControlCalls = Snapshot.ControlFailures = 1;
  Snapshot.LastFailure.Valid = TRUE; Snapshot.LastFailure.Kind = 1;
  Snapshot.LastFailure.Phase = 2; Snapshot.LastFailure.Address = 3;
  Snapshot.LastFailure.HubAddress = 1; Snapshot.LastFailure.HubPort = 2;
  Snapshot.LastFailure.RequestValid = Snapshot.LastFailure.ResultValid = TRUE;
  Snapshot.LastFailure.RequestType = 0x80; Snapshot.LastFailure.Request = 6;
  Snapshot.LastFailure.Value = 0x100; Snapshot.LastFailure.Length = 18;
  Snapshot.LastFailure.Result = EFI_USB_ERR_TIMEOUT; Snapshot.LastFailure.Status = EFI_TIMEOUT;
  Tick(); Before = Prints; Mark = TranscriptSize;
  Snapshot.Status = EFI_SUCCESS; Snapshot.Stage = MotoUsbHcStageInterruptReport;
  Snapshot.InterruptReports = 1; Snapshot.LastTransferResult = 0;
  Snapshot.LastInterrupt.Valid = Snapshot.LastInterrupt.CallbackStatusValid = TRUE;
  Snapshot.LastInterrupt.Address = 1; Snapshot.LastInterrupt.Endpoint = 0x81;
  Snapshot.LastInterrupt.Length = 1; Tick();
  assert(Prints == Before + REPORT_PRINTS + 1);
  assert(strstr(Transcript + Mark, "last failure (history) kind=1 phase=2 a=3 ep=00 speed=0 via=1/2"));
  assert(strstr(Transcript + Mark, "failed request valid=1 rt=0080 req=0006 value=0100 index=0000 len=0012"));
  assert(strstr(Transcript + Mark, "last IN a=1 ep=81 len=1 result=00000000 callback=status-0"));
  assert(mPrevious.LastFailure.Valid && mPrevious.LastFailure.Status == EFI_TIMEOUT);
  puts("PASS a successful IN still displays the cached prior control failure explicitly as history");

  Reset(); Waiting(); mTicks = 4;
  Snapshot.LastFailure.Valid = TRUE; Snapshot.LastFailure.Kind = 2;
  Snapshot.LastFailure.RequestType = 0xff; Snapshot.LastFailure.Result = 0xffffffff;
  Snapshot.LastInterrupt.Valid = TRUE; Snapshot.LastInterrupt.CallbackStatus = EFI_TIMEOUT;
  Tick(); Before = Prints;
  assert(strstr(Transcript, "failed request valid=0 rt=---- req=---- value=---- index=---- len=---- result=----"));
  assert(strstr(Transcript, "last IN a=0 ep=00 len=0 result=00000000 callback=----"));
  Snapshot.LastFailure.Result = 0; Snapshot.LastFailure.RequestType = 0;
  Snapshot.LastInterrupt.CallbackStatus = EFI_SUCCESS; Tick(); assert(Prints == Before);
  Snapshot.LastFailure.RequestValid = Snapshot.LastFailure.ResultValid = TRUE;
  Snapshot.LastInterrupt.CallbackStatusValid = TRUE; Mark = TranscriptSize; Tick();
  assert(Prints == Before + REPORT_PRINTS + 1);
  assert(strstr(Transcript + Mark, "rt=0000 req=0000 value=0000 index=0000 len=0000 result=00000000"));
  assert(strstr(Transcript + Mark, "callback=status-0"));
  puts("PASS absent request, result and callback differ from valid zero and valid Success");

  Reset(); Waiting(); mTicks = 4;
  Snapshot.ControlCalls = Snapshot.ControlFailures = Snapshot.AsyncFailures = Snapshot.InterruptFailures = 1;
  Snapshot.InterruptReports = Snapshot.ZeroLengthInterruptReports = 1;
  Snapshot.LastInterrupt.Valid = Snapshot.LastInterrupt.CallbackStatusValid = TRUE;
  Snapshot.LastInterrupt.Length = 1; Tick(); Before = Prints;
  for (I = 0; I < 500; ++I) {
    ++Snapshot.ControlCalls; ++Snapshot.ControlFailures; ++Snapshot.AsyncFailures;
    ++Snapshot.InterruptFailures; ++Snapshot.InterruptReports; ++Snapshot.ZeroLengthInterruptReports;
    Snapshot.LastInterrupt.Address = (UINT8)(1 + I % 2);
    Snapshot.LastInterrupt.Endpoint = (UINT8)(0x81 + I % 2);
    Snapshot.LastInterrupt.Length = 1 + I % 8; Tick();
  }
  assert(Prints == Before);
  Snapshot.LastInterrupt.Length = 0; Tick(); assert(Prints == Before + REPORT_PRINTS);
  Snapshot.LastInterrupt.CallbackStatus = EFI_DEVICE_ERROR; Tick();
  assert(Prints == Before + 2 * REPORT_PRINTS);
  puts("PASS traffic counts and nonzero IN sizes do not scroll; zero-length and callback errors remain observable");

  Reset(); Waiting(); Snapshot.HcPublished = TRUE; mTicks = 4; Tick();
  assert(Prints == REPORT_PRINTS);
  for (I = 0; I < 1230; ++I) {
    static const UINT16 States[] = {9, 3, 0};
    Snapshot.TypecState = States[I % 3]; Snapshot.Role.TypecStatus = Snapshot.TypecState;
    Snapshot.Role.SeenStates |= 1U << Snapshot.TypecState;
    ++Snapshot.Role.Samples; ++Snapshot.Role.SnapshotAttempts;
    Snapshot.VbusPresent = (BOOLEAN)(I & 1);
    Snapshot.PlatformStage = (I & 1) ? MotoUsbStageExternalVbus : MotoUsbStageTypec;
    Tick(); if (I < 29) assert(Prints == REPORT_PRINTS);
  }
  assert(Prints == 2 * REPORT_PRINTS && mPrevious.HcPublished && !mPrevious.HostReady);
  assert(mPrevious.Role.SeenStates == ((1U << 9) | (1U << 3) | 1U));
  puts("PASS published controller waiting for role: 1230 DRP ticks still produce only two snapshots");

  Reset(); Waiting(); Snapshot.HcPublished = TRUE; mTicks = 4; Tick();
  Snapshot.Role.IntRaw = Snapshot.Role.IntMask = 1;
  for (I = 0; I < 29; ++I) Tick();
  assert(Prints == REPORT_PRINTS); Tick(); assert(Prints == 2 * REPORT_PRINTS);
  Snapshot.Role.RoleReadStatus = EFI_DEVICE_ERROR; Tick(); assert(Prints == 3 * REPORT_PRINTS);
  Snapshot.Role.RoleReadStatus = EFI_SUCCESS; Tick(); assert(Prints == 4 * REPORT_PRINTS);
  InventoryLocateResult = EFI_OUT_OF_RESOURCES; Tick(); assert(Prints == 5 * REPORT_PRINTS);
  InventoryLocateResult = EFI_SUCCESS; Tick(); assert(Prints == 6 * REPORT_PRINTS);
  Snapshot.PortConnected = TRUE; Tick(); assert(Prints == 7 * REPORT_PRINTS);
  Snapshot.PortEnabled = TRUE; Tick(); assert(Prints == 8 * REPORT_PRINTS);
  puts("PASS published waiting keeps RAW/MASK throttle while errors, recovery and port changes report immediately");

  Reset(); Snapshot.HcPublished = Snapshot.HostReady = TRUE;
  Snapshot.PortConnected = Snapshot.PortEnabled = TRUE;
  Snapshot.Stage = MotoUsbHcStageConnect; Snapshot.Status = EFI_SUCCESS; mTicks = 4; Tick();
  Before = Prints; Waiting(); Snapshot.HostReady = Snapshot.PortConnected = Snapshot.PortEnabled = FALSE;
  Snapshot.Role.SeenStates = (1U << 9) | (1U << 3) | 1U;
  Tick(); assert(Prints == Before + REPORT_PRINTS); Before = Prints;
  for (I = 0; I < 90; ++I) {
    static const UINT16 States[] = {9, 3, 0};
    Snapshot.TypecState = Snapshot.Role.TypecStatus = States[I % 3]; Tick();
  }
  assert(Prints == Before);
  Snapshot.Stage = MotoUsbHcStageConnect; Snapshot.Status = EFI_SUCCESS;
  Snapshot.HostReady = Snapshot.PortConnected = Snapshot.PortEnabled = TRUE;
  Tick(); assert(Prints == Before + REPORT_PRINTS);
  puts("PASS detach is reported once with publication preserved; known DRP cycles stay quiet and reattach reports immediately");

  Reset(); Waiting(); Snapshot.HcPublished = TRUE; mTicks = 4;
  Snapshot.LastFailure.Valid = TRUE; Snapshot.LastFailure.Status = EFI_DEVICE_ERROR;
  Snapshot.LastFailure.Ep0.Cause = MOTO_G20_USB_EP0_CAUSE_STALL;
  Snapshot.LastFailure.Ep0.ValidMask = 7; Snapshot.LastFailure.Ep0.Csr0 = 0xffff;
  Snapshot.LastFailure.Ep0.Polls = Snapshot.LastFailure.Ep0.DelayUs = 99;
  Snapshot.RootResetDevCtl = Snapshot.RootResetPower = 0xff;
  Tick(); Before = Prints;
  assert(strstr(Transcript, "EP0 (failure history)=unavailable"));
  assert(strstr(Transcript, "post-reset (history) valid=0 DEV=---- POWER=----"));
  ++Snapshot.LastFailure.Ep0.Polls; ++Snapshot.LastFailure.Ep0.Csr0;
  --Snapshot.RootResetDevCtl; --Snapshot.RootResetPower; Tick(); assert(Prints == Before);
  Snapshot.LastFailure.Ep0.Valid = TRUE; Snapshot.LastFailure.Ep0.Cause = MOTO_G20_USB_EP0_CAUSE_NONE;
  Snapshot.LastFailure.Ep0.Csr0 = Snapshot.LastFailure.Ep0.IntrTx = Snapshot.LastFailure.Ep0.DevCtl = 0;
  Snapshot.LastFailure.Ep0.Polls = Snapshot.LastFailure.Ep0.DelayUs = 0;
  Snapshot.RootResetValid = TRUE; Snapshot.RootResetDevCtl = Snapshot.RootResetPower = 0;
  Mark = TranscriptSize; Tick(); assert(Prints == Before + REPORT_PRINTS + 2);
  assert(strstr(Transcript + Mark, "cause=0/no-Wait-failure access-lost=0 polls=0 delay-only-us=0"));
  assert(strstr(Transcript + Mark, "valid=07 CSR=0000 IRQ=0000 DEV=0000 (historical)"));
  assert(strstr(Transcript + Mark, "post-reset (history) valid=1 DEV=0000 POWER=0000"));
  puts("PASS unavailable EP0/reset bytes cannot invent observations; valid zero and no-Wait-failure are explicit");

  for (I = MOTO_G20_USB_EP0_CAUSE_ROLE_LOST; I <= MOTO_G20_USB_EP0_CAUSE_TIMEOUT; ++I) {
    static const char *Causes[] = {"", "1/role-lost", "2/CSR-error", "3/host/VBUS-lost", "4/stall", "5/NAK", "6/poll-budget"};
    Snapshot.LastFailure.Ep0.Cause = (UINT8)I;
    Snapshot.LastFailure.Ep0.HardwareAccessLost = I == MOTO_G20_USB_EP0_CAUSE_ROLE_LOST;
    Snapshot.LastFailure.Ep0.ValidMask = I == MOTO_G20_USB_EP0_CAUSE_CSR_ERROR ? 3 : 7;
    Snapshot.LastFailure.Ep0.Csr0 = 0x0080; Snapshot.LastFailure.Ep0.IntrTx = 1;
    Snapshot.LastFailure.Ep0.DevCtl = 0x5d;
    Snapshot.LastFailure.Ep0.Polls = 2; Snapshot.LastFailure.Ep0.DelayUs = 10;
    Before = Prints; Mark = TranscriptSize; Tick();
    assert(Prints == Before + REPORT_PRINTS + 2 && strstr(Transcript + Mark, Causes[I]));
    assert(strstr(Transcript + Mark, "polls=2 delay-only-us=10"));
    if (I == MOTO_G20_USB_EP0_CAUSE_ROLE_LOST) {
      assert(strstr(Transcript + Mark, "access-lost=1"));
      assert(strstr(Transcript + Mark, "CSR=0080 IRQ=0001 DEV=005D (historical)"));
    }
    if (I == MOTO_G20_USB_EP0_CAUSE_CSR_ERROR) {
      assert(strstr(Transcript + Mark, "valid=03 CSR=0080 IRQ=0001 DEV=---- (historical)"));
      assert(mPrevious.LastFailure.Ep0.DevCtl == 0);
    }
  }
  puts("PASS all six EP0 failure branches report immediately with per-register validity and historical access-loss labeling");

  Before = Prints;
  for (I = 0; I < 500; ++I) {
    ++Snapshot.LastFailure.Ep0.Polls; Snapshot.LastFailure.Ep0.DelayUs += 10; Tick();
  }
  assert(Prints == Before);
  Snapshot.LastFailure.Ep0.ValidMask = 0; Tick(); Before = Prints;
  for (I = 0; I < 30; ++I) {
    ++Snapshot.LastFailure.Ep0.Csr0; ++Snapshot.LastFailure.Ep0.IntrTx; ++Snapshot.LastFailure.Ep0.DevCtl; Tick();
  }
  assert(Prints == Before);
  Snapshot.LastFailure.Ep0.HardwareAccessLost = TRUE; Tick(); assert(Prints == Before + REPORT_PRINTS + 2);
  Before = Prints; Snapshot.LastFailure.Ep0.Cause = 99; Mark = TranscriptSize; Tick();
  assert(Prints == Before + REPORT_PRINTS + 2 && strstr(Transcript + Mark, "cause=99/unknown"));
  puts("PASS EP0 timing-only repetition and invalid raw bytes stay quiet; access loss and unknown causes remain visible");

  Reset(); Waiting(); Snapshot.HcPublished = TRUE; mTicks = 4; Tick(); Before = Prints;
  Snapshot.RootResetValid = TRUE; Snapshot.RootResetDevCtl = 0x5d; Snapshot.RootResetPower = 0x60;
  Mark = TranscriptSize; Tick(); assert(Prints == Before + REPORT_PRINTS);
  assert(strstr(Transcript + Mark, "post-reset (history) valid=1 DEV=005D POWER=0060"));
  Before = Prints; Snapshot.RootResetDevCtl = 0x19; Tick(); assert(Prints == Before + REPORT_PRINTS);
  Before = Prints; Snapshot.RootResetValid = FALSE; Mark = TranscriptSize; Tick();
  assert(Prints == Before + REPORT_PRINTS && strstr(Transcript + Mark, "post-reset (history) valid=0 DEV=---- POWER=----"));
  puts("PASS post-reset cache changes and reset-start invalidation bypass waiting throttle without claiming current bus state");
  mStatusTimer = (void *)5; Before = Prints; StopUsbStatus(NULL, NULL);
  assert(Closed == 1 && mStatusTimer == NULL); Snapshot.Status = EFI_DEVICE_ERROR; Tick();
  assert(Prints == Before);
  puts("PASS observer stops at ExitBootServices without consuming keys");
  puts("PASS 35 host-only producer/observer regression groups; hardware NOT validated");
  return 0;
}

