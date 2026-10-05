[Defines]
  PLATFORM_NAME                  = MMU2ATranslationTableCompileProbe
  PLATFORM_GUID                  = 9D4FBC32-0A53-4D13-8E55-2E22E8A7426F
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x0001001B
  OUTPUT_DIRECTORY               = Build/MMU2ATranslationTableCompileProbe
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG
  SKUID_IDENTIFIER               = DEFAULT

[LibraryClasses]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  CompilerIntrinsicsLib|MdePkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
  MemoryAllocationLib|MotoG20Pkg/Library/MotoG20PrePiMemoryAllocationLib/MotoG20PrePiMemoryAllocationLib.inf
  MotoG20TranslationTableLib|MotoG20Pkg/Library/MotoG20TranslationTableLib/MotoG20TranslationTableLib.inf
  PeCoffExtraActionLib|MdePkg/Library/BasePeCoffExtraActionLibNull/BasePeCoffExtraActionLibNull.inf
  PeCoffLib|MdePkg/Library/BasePeCoffLib/BasePeCoffLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PrePiHobListPointerLib|MotoG20Pkg/Library/MotoG20HobListPointerLib/MotoG20HobListPointerLib.inf
  SafeIntLib|MdePkg/Library/BaseSafeIntLib/BaseSafeIntLib.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf

[Components]
  MotoG20Pkg/Tests/MMU2ATranslationTableCompileProbe/MMU2ATranslationTableCompileProbe.inf
