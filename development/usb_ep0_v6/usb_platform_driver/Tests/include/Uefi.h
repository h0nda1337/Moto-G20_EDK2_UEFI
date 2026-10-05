#ifndef TEST_UEFI_H
#define TEST_UEFI_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef void VOID;
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uintptr_t UINTN;
typedef uint64_t EFI_STATUS;
typedef bool BOOLEAN;
typedef UINTN EFI_TPL;
typedef void *EFI_EVENT;
#define STATIC static
#define CONST const
#define EFIAPI
#define OUT
#define IN
#define TRUE true
#define FALSE false
#define EFI_ERROR(x) ((x) != 0)
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
#define EFI_UNSUPPORTED 3
#define EFI_NOT_READY 6
#define EFI_DEVICE_ERROR 7
#define EFI_OUT_OF_RESOURCES 9
#define EFI_NO_RESPONSE 16
#define EFI_ACCESS_DENIED 15
#define EFI_TIMEOUT 18
#define EFI_COMPROMISED_DATA 33
#define TPL_APPLICATION 4
#define TPL_CALLBACK 8
#define TPL_NOTIFY 16
#define EVT_TIMER 1
#define EVT_NOTIFY_SIGNAL 2
#define TimerCancel 0
#define TimerPeriodic 1
#define EFI_MEMORY_UC 1
#define EFI_MEMORY_WC 2
#define EFI_MEMORY_WT 4
#define EFI_MEMORY_WB 8
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define MIN(x,y) ((x)<(y)?(x):(y))
#define BIT0 (1U<<0)
#define BIT1 (1U<<1)
#define BIT2 (1U<<2)
#define BIT3 (1U<<3)
#define BIT4 (1U<<4)
#define BIT5 (1U<<5)
#define BIT6 (1U<<6)
#define BIT8 (1U<<8)
#define BIT9 (1U<<9)
#define BIT10 (1U<<10)
#define BIT11 (1U<<11)
#define BIT12 (1U<<12)
#define BIT16 (1U<<16)
#define BIT20 (1U<<20)
#define BIT24 (1U<<24)
#define BIT28 (1U<<28)
#define BIT30 (1U<<30)
#define BIT31 (1U<<31)
typedef enum { EfiGcdMemoryTypeNonExistent, EfiGcdMemoryTypeReserved, EfiGcdMemoryTypeMemoryMappedIo } TEST_GCD;
typedef struct { UINT64 BaseAddress, Length; TEST_GCD GcdMemoryType; UINT64 Attributes; } EFI_GCD_MEMORY_SPACE_DESCRIPTOR;
typedef struct {
 EFI_STATUS (*GetMemorySpaceDescriptor)(UINT64,EFI_GCD_MEMORY_SPACE_DESCRIPTOR*);
 EFI_STATUS (*AddMemorySpace)(TEST_GCD,UINT64,UINT64,UINT64);
 EFI_STATUS (*SetMemorySpaceAttributes)(UINT64,UINT64,UINT64);
} EFI_DXE_SERVICES;
typedef VOID (*EFI_EVENT_NOTIFY)(EFI_EVENT,VOID*);
typedef struct {
 EFI_STATUS (*CreateEvent)(UINTN,EFI_TPL,EFI_EVENT_NOTIFY,VOID*,EFI_EVENT*);
 EFI_STATUS (*SetTimer)(EFI_EVENT,UINTN,UINT64);
 EFI_TPL (*RaiseTPL)(EFI_TPL);
 VOID (*RestoreTPL)(EFI_TPL);
 EFI_STATUS (*CloseEvent)(EFI_EVENT);
} EFI_BOOT_SERVICES;
extern EFI_DXE_SERVICES *gDS;
extern EFI_BOOT_SERVICES *gBS;
UINT32 MmioRead32(UINTN);
UINT32 MmioWrite32(UINTN,UINT32);
UINT8 MmioRead8(UINTN);
UINT8 MmioWrite8(UINTN,UINT8);
VOID MicroSecondDelay(UINTN);
UINT64 GetPerformanceCounter(VOID);
UINT64 GetTimeInNanoSecond(UINT64);
#endif
