#ifndef MOTO_G20_FOUNDATION_BOOTSTRAP_H_
#define MOTO_G20_FOUNDATION_BOOTSTRAP_H_

#include <Base.h>
#include <Uefi/UefiBaseType.h>

#define MOTO_G20_FOUNDATION_DTB_ADDRESS              0x82000000ULL
#define MOTO_G20_FOUNDATION_DTB_MAX_SIZE             0x00200000ULL
#define MOTO_G20_FOUNDATION_DRAM_BASE                0x80000000ULL
#define MOTO_G20_FOUNDATION_DRAM_END                 0x17FFFF000ULL
#define MOTO_G20_FOUNDATION_LOW_RESERVED_END         0x82A0B000ULL

// The UMS512 bootloader publishes the boot-v2 ramdisk byte-exactly at this
// physical address.  Keep the accepted source window narrower than the
// surrounding low reserved region; the transport is read-only here and is
// copied to the independently mapped staging range before M=1.
#define MOTO_G20_FOUNDATION_INITRD_SOURCE_BASE        0x82100000ULL
#define MOTO_G20_FOUNDATION_INITRD_SOURCE_END         0x82A0B000ULL

#define MOTO_G20_FOUNDATION_FV_TRANSPORT_MAX         0x008C2314ULL
// The frozen FV header advertises 16-byte alignment (Attributes alignment
// exponent 4).  LOW_RESERVED_END is page-aligned and therefore satisfies the
// header exactly; retaining it also preserves the proven staging placement.
#define MOTO_G20_FOUNDATION_FV_STAGING_BASE          0x82A0B000ULL
#define MOTO_G20_FOUNDATION_FV_STAGING_END           0x83300000ULL

#define MOTO_G20_FOUNDATION_DXE_MEMORY_BASE          0x83300000ULL
#define MOTO_G20_FOUNDATION_DXE_MEMORY_END           0x87500000ULL
#define MOTO_G20_FOUNDATION_DXE_MEMORY_SIZE          0x04200000ULL
#define MOTO_G20_FOUNDATION_DXE_STACK_SIZE           0x00020000ULL

#define MOTO_G20_FOUNDATION_TTBR0                    0x81981000ULL
#define MOTO_G20_FOUNDATION_L1                       0x81980000ULL
#define MOTO_G20_FOUNDATION_L2_80000000              0x8197F000ULL
#define MOTO_G20_FOUNDATION_EXISTING_L3_82A00000     0x8197D000ULL
#define MOTO_G20_FOUNDATION_EXISTING_L3_FRAMEBUFFER  0x8197C000ULL
#define MOTO_G20_FOUNDATION_NEW_L3_87400000          0x81979000ULL

#define MOTO_G20_FOUNDATION_SHA256_SIZE              32U

typedef struct {
  UINT64  TransportSize;
  UINT64  FirmwareVolumeSize;
  UINT8   TransportSha256[MOTO_G20_FOUNDATION_SHA256_SIZE];
} MOTO_G20_FOUNDATION_FV_IDENTITY;

typedef struct {
  UINT64  DtbAddress;
  UINT64  DtbSize;
  UINT64  SourceBase;
  UINT64  SourceSize;
  UINT64  StagingBase;
  UINT64  StagingSize;
  UINT64  FirmwareVolumeSize;
  UINT8   VerifiedSha256[MOTO_G20_FOUNDATION_SHA256_SIZE];
} MOTO_G20_FOUNDATION_FV_PLAN;

typedef struct {
  UINTN  ExistingL3PageWrites;
  UINTN  L2BlockWrites;
  UINTN  NewL3PageWrites;
  UINTN  ParentTableWrites;
  UINTN  TotalDescriptorWrites;
} MOTO_G20_FOUNDATION_MAP_RESULT;

typedef enum {
  MotoG20FoundationStageNone = 0,
  MotoG20FoundationStageIdentity,
  MotoG20FoundationStageFdt,
  MotoG20FoundationStageFvSource,
  MotoG20FoundationStageFvHash,
  MotoG20FoundationStageFvCopy,
  MotoG20FoundationStageFramebufferMap,
  MotoG20FoundationStageMmuProof,
  MotoG20FoundationStageHighMap,
  MotoG20FoundationStageDxeMemoryClear,
  MotoG20FoundationStageHobConstruct,
  MotoG20FoundationStageHobPublish,
  MotoG20FoundationStageFvHob,
  MotoG20FoundationStageDxeCoreLocate,
  MotoG20FoundationStageDxeCoreLoad,
  MotoG20FoundationStageDxeStack,
  MotoG20FoundationStageDxeCoreTransfer,
  MotoG20FoundationStageDxeCoreReturned,
  MotoG20FoundationStageDxeStackBridge,
  MotoG20FoundationStageDxeEntryDispatch
} MOTO_G20_FOUNDATION_STAGE;

typedef struct {
  UINT64      Signature;
  UINT64      Stage;
  EFI_STATUS  Status;
  UINT64      DtbAddress;
  UINT64      SctlrEl2;
} MOTO_G20_FOUNDATION_FAILURE_RECORD;

EFI_STATUS
EFIAPI
MotoG20FoundationPlanAndStageFv (
  IN  UINT64                                  DtbAddress,
  IN  CONST MOTO_G20_FOUNDATION_FV_IDENTITY  *ExpectedIdentity,
  OUT MOTO_G20_FOUNDATION_FV_PLAN             *Plan
  );

EFI_STATUS
EFIAPI
MotoG20FoundationAppendHighMap (
  OUT MOTO_G20_FOUNDATION_MAP_RESULT  *Result
  );

EFI_STATUS
EFIAPI
MotoG20FoundationBuildHobAndLoadDxe (
  IN  CONST MOTO_G20_FOUNDATION_FV_PLAN  *Plan,
  OUT MOTO_G20_FOUNDATION_STAGE          *FailureStage
  );

#endif
