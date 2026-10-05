#ifndef MOCK_UEFI_H
#define MOCK_UEFI_H
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#define IN
#define OUT
#define OPTIONAL
#define STATIC static
#define CONST const
#define EFIAPI
#define VOID void
#define TRUE 1
#define FALSE 0
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
typedef uint64_t UINT64;
typedef uintptr_t UINTN;
typedef uint64_t EFI_STATUS;
typedef unsigned EFI_TPL;
typedef int BOOLEAN;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef struct { uint32_t Data1; } EFI_GUID;
typedef struct { unsigned unused; } EFI_SYSTEM_TABLE;
typedef void (*EFI_EVENT_NOTIFY)(EFI_EVENT, void *);
#define EFI_SUCCESS 0ULL
#define EFI_INVALID_PARAMETER (0x8000000000000002ULL)
#define EFI_UNSUPPORTED (0x8000000000000003ULL)
#define EFI_NOT_FOUND (0x800000000000000EULL)
#define EFI_OUT_OF_RESOURCES (0x8000000000000009ULL)
#define EFI_DEVICE_ERROR (0x8000000000000007ULL)
#define EFI_ERROR(x) (((x) & 0x8000000000000000ULL) != 0)
#define EFI_NATIVE_INTERFACE 0
#define TPL_APPLICATION 4
#define TPL_CALLBACK 8
#define TPL_NOTIFY 16
typedef struct {
  EFI_TPL (*RaiseTPL)(EFI_TPL);
  void (*RestoreTPL)(EFI_TPL);
  EFI_STATUS (*CloseEvent)(EFI_EVENT);
  EFI_STATUS (*LocateProtocol)(EFI_GUID *, void *, void **);
  EFI_STATUS (*InstallProtocolInterface)(EFI_HANDLE *, EFI_GUID *, unsigned, void *);
} EFI_BOOT_SERVICES;
extern EFI_BOOT_SERVICES *gBS;
extern EFI_GUID gEfiBdsArchProtocolGuid;
extern EFI_GUID gEfiCpuArchProtocolGuid;
extern EFI_GUID gEfiGraphicsOutputProtocolGuid;
extern EFI_GUID gEfiSimpleTextOutProtocolGuid;
extern EFI_GUID gEfiTimerArchProtocolGuid;
extern EFI_GUID gHardwareInterruptProtocolGuid;
typedef enum {
  MotoG20DxeStageProgressMonitor,
  MotoG20DxeStageCpuArch,
  MotoG20DxeStageInterruptArch,
  MotoG20DxeStageTimerArch,
  MotoG20DxeStageGraphicsOutput,
  MotoG20DxeStageTextOutput,
  MotoG20DxeStageBdsArch,
  MotoG20DxeStageFailure
} MOTO_G20_DXE_STAGE;
void MotoG20DxeTelemetryCheckpoint(MOTO_G20_DXE_STAGE, EFI_STATUS, UINT64, UINT64);
void MotoG20DxeTelemetryFailAndReset(MOTO_G20_DXE_STAGE, EFI_STATUS, UINT64, UINT64);
void MotoG20DxeTelemetryReleaseDisplay(void);
EFI_EVENT EfiCreateProtocolNotifyEvent(EFI_GUID *, EFI_TPL, EFI_EVENT_NOTIFY, void *, void **);
#endif
