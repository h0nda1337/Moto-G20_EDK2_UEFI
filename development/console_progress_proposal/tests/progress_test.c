/* Host control-flow test. Includes the actual proposed ProgressDxe source. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mock_uefi.h"
#include "../proposed/MotoG20Pkg/Drivers/MotoG20DxeProgressDxe/MotoG20DxeProgressDxe.c"

EFI_GUID gEfiBdsArchProtocolGuid = {6};
EFI_GUID gEfiCpuArchProtocolGuid = {1};
EFI_GUID gEfiGraphicsOutputProtocolGuid = {4};
EFI_GUID gEfiSimpleTextOutProtocolGuid = {5};
EFI_GUID gEfiTimerArchProtocolGuid = {3};
EFI_GUID gHardwareInterruptProtocolGuid = {2};
EFI_GUID gMotoG20DxeProgressControlProtocolGuid = {7};

typedef struct {
  EFI_EVENT_NOTIFY Notify;
  void *Context;
  int Active;
  int Queued;
} MOCK_EVENT;
static MOCK_EVENT Events[6];
static unsigned EventCount, Checkpoints, Releases, Closes, LocateCalls;
static int CloseFailureIndex, ReleaseState;
static EFI_TPL CurrentTpl;
static void *Installed;
static unsigned Interfaces[7];
static UINT64 AvailableBitmap;

void MotoG20DxeTelemetryCheckpoint(MOTO_G20_DXE_STAGE Stage, EFI_STATUS Status, UINT64 A, UINT64 B) {
  (void)Stage; (void)Status; (void)A; (void)B;
  assert(!ReleaseState);
  ++Checkpoints;
}
void MotoG20DxeTelemetryFailAndReset(MOTO_G20_DXE_STAGE Stage, EFI_STATUS Status, UINT64 A, UINT64 B) {
  (void)Stage; (void)Status; (void)A; (void)B;
  fputs("Unexpected failure/reset\n", stderr);
  abort();
}
void MotoG20DxeTelemetryReleaseDisplay(void) {
  assert(CurrentTpl == TPL_NOTIFY);
  ReleaseState = 1;
  ++Releases;
}
static EFI_TPL Raise(EFI_TPL New) {
  EFI_TPL Old = CurrentTpl;
  assert(New >= Old);
  CurrentTpl = New;
  return Old;
}
static void Restore(EFI_TPL Old) {
  unsigned Index;
  assert(Old <= CurrentTpl);
  CurrentTpl = Old;
  for (Index = 0; Index < EventCount; ++Index) {
    if (Events[Index].Active && Events[Index].Queued) {
      Events[Index].Queued = 0;
      CurrentTpl = TPL_CALLBACK;
      Events[Index].Notify(&Events[Index], Events[Index].Context);
      CurrentTpl = Old;
    }
  }
}
static EFI_STATUS Close(EFI_EVENT Event) {
  MOCK_EVENT *E = Event;
  assert(CurrentTpl == TPL_NOTIFY);
  assert(mQuiesced && ReleaseState);
  assert(E->Active);
  ++Closes;
  if (E - Events == CloseFailureIndex) return EFI_DEVICE_ERROR;
  E->Active = 0;
  E->Queued = 0;
  return EFI_SUCCESS;
}
static EFI_STATUS Locate(EFI_GUID *Guid, void *Registration, void **Interface) {
  assert(Registration == NULL);
  ++LocateCalls;
  assert(Guid->Data1 >= 1 && Guid->Data1 <= 6);
  if (!(AvailableBitmap & (1ULL << (Guid->Data1 - 1)))) return EFI_NOT_FOUND;
  *Interface = &Interfaces[Guid->Data1];
  return EFI_SUCCESS;
}
static EFI_STATUS Install(EFI_HANDLE *Handle, EFI_GUID *Guid, unsigned Native, void *Interface) {
  (void)Handle;
  assert(EventCount == 6);
  assert(Guid == &gMotoG20DxeProgressControlProtocolGuid);
  assert(Native == EFI_NATIVE_INTERFACE);
  Installed = Interface;
  return EFI_SUCCESS;
}
static EFI_BOOT_SERVICES Services = {Raise, Restore, Close, Locate, Install};
EFI_BOOT_SERVICES *gBS = &Services;
EFI_EVENT EfiCreateProtocolNotifyEvent(EFI_GUID *Guid, EFI_TPL Tpl, EFI_EVENT_NOTIFY Notify, void *Context, void **Registration) {
  MOCK_EVENT *E = &Events[EventCount++];
  EFI_TPL Old = CurrentTpl;
  (void)Guid;
  assert(EventCount <= 6 && Tpl == TPL_CALLBACK);
  E->Notify = Notify; E->Context = Context; E->Active = 1;
  *Registration = E;
  CurrentTpl = TPL_CALLBACK;
  Notify(E, Context); /* UefiLib can signal immediately during registration. */
  CurrentTpl = Old;
  return E;
}

static void SetupMask(UINT64 Mask) {
  unsigned Bits = 0;
  UINT64 Value;
  memset(Events, 0, sizeof Events);
  memset(mEvents, 0, sizeof mEvents);
  memset(mRegistrations, 0, sizeof mRegistrations);
  EventCount = Checkpoints = Releases = Closes = LocateCalls = 0;
  mProtocolBitmap = 0;
  mQuiesced = FALSE;
  ReleaseState = 0;
  CloseFailureIndex = -1;
  CurrentTpl = TPL_APPLICATION;
  Installed = NULL;
  AvailableBitmap = Mask;
  assert(MotoG20DxeProgressEntryPoint((EFI_HANDLE)1, NULL) == EFI_SUCCESS);
  assert(Installed == &mProgressControl);
  for (Value = Mask; Value; Value >>= 1) Bits += Value & 1;
  assert(mProtocolBitmap == Mask && Checkpoints == 1 + Bits && LocateCalls == 6);
}
static void Setup(void) {
  SetupMask(0x3F);
}
static void TestDuplicate(void) {
  Setup();
  Events[4].Notify(&Events[4], Events[4].Context);
  assert(Checkpoints == 7 && LocateCalls == 6 && mProtocolBitmap == 0x3F);
  puts("PASS duplicate notification does not redraw or re-locate");
}
static void TestQuiesce(void) {
  UINT64 Bitmap = 0;
  unsigned Index;
  Setup();
  for (Index = 0; Index < EventCount; ++Index) Events[Index].Queued = 1;
  assert(mProgressControl.Quiesce(&mProgressControl, &Bitmap) == EFI_SUCCESS);
  assert(Bitmap == 0x3F && Releases == 1 && Closes == 6 && mQuiesced);
  assert(CurrentTpl == TPL_APPLICATION && Checkpoints == 7);
  for (Index = 0; Index < EventCount; ++Index) {
    assert(!Events[Index].Active && mEvents[Index] == NULL);
    assert(mRegistrations[Index] == NULL);
  }
  puts("PASS quiesce releases the monitor instance and closes all events at protected TPL");
  /* Defensive invocation even though CloseEvent removed the pending notify. */
  ProtocolInstalled(NULL, (VOID *)&mWatches[4]);
  assert(Checkpoints == 7 && LocateCalls == 6);
  puts("PASS a stale/queued callback after quiesce cannot redraw");
  assert(mProgressControl.Quiesce(&mProgressControl, &Bitmap) == EFI_SUCCESS);
  assert(Bitmap == 0x3F && Closes == 6 && Releases == 2);
  puts("PASS repeated quiesce does not close already closed events");
}
static void TestUnseenAfterQuiesce(void) {
  UINT64 Bitmap;
  SetupMask(0x2F); /* A genuinely new text protocol, not masked by deduplication. */
  assert(QuiesceProgress(&mProgressControl, &Bitmap) == EFI_SUCCESS);
  AvailableBitmap = 0x3F;
  ProtocolInstalled(NULL, (VOID *)&mWatches[4]);
  assert(Bitmap == 0x2F && mProtocolBitmap == 0x2F);
  assert(Checkpoints == 6 && LocateCalls == 6);
  puts("PASS a previously unseen protocol after handoff cannot redraw or change evidence");
}
static void TestCloseError(void) {
  UINT64 Bitmap;
  SetupMask(0x2F);
  CloseFailureIndex = 4;
  Events[4].Queued = 1;
  AvailableBitmap = 0x3F;
  assert(mProgressControl.Quiesce(&mProgressControl, &Bitmap) == EFI_DEVICE_ERROR);
  assert(mQuiesced && ReleaseState && Bitmap == 0x2F);
  assert(Events[4].Active && !Events[4].Queued);
  assert(CurrentTpl == TPL_APPLICATION && Checkpoints == 6 && LocateCalls == 6);
  puts("PASS CloseEvent failure leaves suppression latched during RestoreTPL callback");
  CloseFailureIndex = -1;
  assert(mProgressControl.Quiesce(&mProgressControl, &Bitmap) == EFI_SUCCESS);
  assert(Closes == 7 && !Events[4].Active);
  puts("PASS repeated quiesce retries only the unclosed event");
}
static void TestInvalid(void) {
  UINT64 Bitmap;
  Setup();
  assert(QuiesceProgress(NULL, &Bitmap) == EFI_INVALID_PARAMETER);
  assert(QuiesceProgress(&mProgressControl, NULL) == EFI_INVALID_PARAMETER);
  assert(!mQuiesced && !ReleaseState && Closes == 0 && CurrentTpl == TPL_APPLICATION);
  puts("PASS invalid protocol arguments leave ownership unchanged");
}
int main(void) {
  TestDuplicate(); TestQuiesce(); TestUnseenAfterQuiesce(); TestCloseError(); TestInvalid();
  return 0;
}
