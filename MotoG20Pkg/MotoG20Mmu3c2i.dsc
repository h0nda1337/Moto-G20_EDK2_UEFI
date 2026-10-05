[Defines]
  PLATFORM_NAME                  = MotoG20Mmu3c2i
  PLATFORM_GUID                  = 372FC2AC-2024-577D-B5EA-2C09D2CB54AD
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x0001001B
  OUTPUT_DIRECTORY               = Build/MotoG20Mmu3c2i
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG
  SKUID_IDENTIFIER               = DEFAULT

[LibraryClasses]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  CompilerIntrinsicsLib|MdePkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  SafeIntLib|MdePkg/Library/BaseSafeIntLib/BaseSafeIntLib.inf
  PeCoffExtraActionLib|MdePkg/Library/BasePeCoffExtraActionLibNull/BasePeCoffExtraActionLibNull.inf
  PeCoffLib|MdePkg/Library/BasePeCoffLib/BasePeCoffLib.inf
  HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
  MemoryAllocationLib|MotoG20Pkg/Library/MotoG20PrePiMemoryAllocationLib/MotoG20PrePiMemoryAllocationLib.inf
  MotoG20TranslationTableLib|MotoG20Pkg/Library/MotoG20TranslationTableLib/MotoG20TranslationTableLib.inf
  MotoG20PreOsTelemetryLib|MotoG20Pkg/Library/MotoG20PreOsTelemetryLib/MotoG20PreOsTelemetryLib.inf
  MotoG20PostMmuTelemetryLib|MotoG20Pkg/Library/MotoG20PostMmuTelemetryLib/MotoG20PostMmuTelemetryLib.inf
  PrePiHobListPointerLib|MotoG20Pkg/Library/MotoG20HobListPointerLib/MotoG20HobListPointerLib.inf

[PcdsFeatureFlag]
  gEmbeddedTokenSpaceGuid.PcdPrePiProduceMemoryTypeInformationHob|FALSE

[PcdsFixedAtBuild]
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiACPIReclaimMemory|0
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiACPIMemoryNVS|0
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiReservedMemoryType|0
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesData|0
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesCode|0

[Components]
  MotoG20Pkg/PrePi/MMU3C2I/MotoG20Mmu3c2iIntegration.inf
