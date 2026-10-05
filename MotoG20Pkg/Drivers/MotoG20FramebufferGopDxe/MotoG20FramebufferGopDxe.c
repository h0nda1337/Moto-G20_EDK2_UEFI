/** @file
  GOP producer for the bootloader-initialized Moto G20 framebuffer.

  The display controller is deliberately left untouched.  The driver exposes
  the physically proven 720 x 1600 BGRX framebuffer and uses the generic EDK2
  framebuffer BLT implementation.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>

#define MOTO_G20_FB_WIDTH   720U
#define MOTO_G20_FB_HEIGHT  1600U
#define MOTO_G20_FB_STRIDE  720U
#define MOTO_G20_FB_BPP     4U
#define MOTO_G20_FB_BYTES   ((UINT64)MOTO_G20_FB_STRIDE * MOTO_G20_FB_HEIGHT * MOTO_G20_FB_BPP)

#define DP_NODE_LEN(Type)  { (UINT8)sizeof (Type), (UINT8)(sizeof (Type) >> 8) }

STATIC_ASSERT (MOTO_G20_FB_BYTES == 0x465000ULL, "Unexpected Moto G20 framebuffer size");

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} MOTO_G20_FRAMEBUFFER_DEVICE_PATH;
#pragma pack ()

// This is a flat bootloader-initialized controller, with no parent handle or
// child-enumerating bus driver. An ACPI_ADR node makes BmConsole truncate this
// path to a non-existent parent. Vendor + END names the actual published GOP.
STATIC_ASSERT (sizeof (MOTO_G20_FRAMEBUFFER_DEVICE_PATH) == 24U, "Flat GOP path size");

STATIC MOTO_G20_FRAMEBUFFER_DEVICE_PATH  mFramebufferDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, DP_NODE_LEN (VENDOR_DEVICE_PATH) },
    { 0x3fa80f1d, 0x6627, 0x41de, { 0xa6, 0xef, 0x71, 0xca, 0xa9, 0x57, 0x21, 0xd4 } }
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    DP_NODE_LEN (EFI_DEVICE_PATH_PROTOCOL)
  }
};

STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  mModeInfo = {
  0,
  MOTO_G20_FB_WIDTH,
  MOTO_G20_FB_HEIGHT,
  PixelBlueGreenRedReserved8BitPerColor,
  { 0, 0, 0, 0 },
  MOTO_G20_FB_STRIDE
};

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE  mMode = {
  1,
  0,
  &mModeInfo,
  sizeof (mModeInfo),
  0,
  MOTO_G20_FB_BYTES
};

STATIC FRAME_BUFFER_CONFIGURE  *mFrameBufferConfigure;
STATIC UINTN                   mFrameBufferConfigureSize;
STATIC EFI_HANDLE              mFramebufferHandle;

STATIC
EFI_STATUS
EFIAPI
MotoG20FramebufferQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  if ((This == NULL) || (SizeOfInfo == NULL) || (Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }

  *Info = AllocateCopyPool (sizeof (mModeInfo), &mModeInfo);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *SizeOfInfo = sizeof (mModeInfo);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MotoG20FramebufferSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black;
  RETURN_STATUS                  Status;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }

  Status = FrameBufferBltConfigure (
             (VOID *)(UINTN)mMode.FrameBufferBase,
             &mModeInfo,
             mFrameBufferConfigure,
             &mFrameBufferConfigureSize
             );
  if (Status == RETURN_BUFFER_TOO_SMALL) {
    if (mFrameBufferConfigure != NULL) {
      FreePool (mFrameBufferConfigure);
    }

    mFrameBufferConfigure = AllocatePool (mFrameBufferConfigureSize);
    if (mFrameBufferConfigure == NULL) {
      mFrameBufferConfigureSize = 0;
      return EFI_OUT_OF_RESOURCES;
    }

    Status = FrameBufferBltConfigure (
               (VOID *)(UINTN)mMode.FrameBufferBase,
               &mModeInfo,
               mFrameBufferConfigure,
               &mFrameBufferConfigureSize
               );
  }

  if (RETURN_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  mMode.Mode = ModeNumber;
  mMode.Info = &mModeInfo;

  ZeroMem (&Black, sizeof (Black));
  Status = FrameBufferBlt (
             mFrameBufferConfigure,
             &Black,
             EfiBltVideoFill,
             0,
             0,
             0,
             0,
             mModeInfo.HorizontalResolution,
             mModeInfo.VerticalResolution,
             0
             );

  return RETURN_ERROR (Status) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MotoG20FramebufferBlt (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer OPTIONAL,
  IN  EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN  UINTN                              SourceX,
  IN  UINTN                              SourceY,
  IN  UINTN                              DestinationX,
  IN  UINTN                              DestinationY,
  IN  UINTN                              Width,
  IN  UINTN                              Height,
  IN  UINTN                              Delta OPTIONAL
  )
{
  RETURN_STATUS  Status;

  if ((This == NULL) || (mFrameBufferConfigure == NULL)) {
    return EFI_NOT_READY;
  }

  Status = FrameBufferBlt (
             mFrameBufferConfigure,
             BltBuffer,
             BltOperation,
             SourceX,
             SourceY,
             DestinationX,
             DestinationY,
             Width,
             Height,
             Delta
             );
  return (EFI_STATUS)Status;
}

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  mGraphicsOutput = {
  MotoG20FramebufferQueryMode,
  MotoG20FramebufferSetMode,
  MotoG20FramebufferBlt,
  &mMode
};

STATIC
EFI_STATUS
RegisterFramebufferMemory (
  IN EFI_PHYSICAL_ADDRESS  FrameBufferBase,
  IN UINT64                FrameBufferSize
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;
  EFI_STATUS                       Status;

  Status = gDS->GetMemorySpaceDescriptor (FrameBufferBase, &Descriptor);
  if (EFI_ERROR (Status) || (Descriptor.GcdMemoryType == EfiGcdMemoryTypeNonExistent)) {
    Status = gDS->AddMemorySpace (
                    EfiGcdMemoryTypeMemoryMappedIo,
                    FrameBufferBase,
                    FrameBufferSize,
                    EFI_MEMORY_UC
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = gDS->SetMemorySpaceAttributes (
                  FrameBufferBase,
                  FrameBufferSize,
                  EFI_MEMORY_UC
                  );
  if ((Status == EFI_UNSUPPORTED) || (Status == EFI_ACCESS_DENIED)) {
    // The Foundation page tables already map this range as uncached + XN.
    return EFI_SUCCESS;
  }

  return Status;
}

EFI_STATUS
EFIAPI
MotoG20FramebufferGopDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_PHYSICAL_ADDRESS  FrameBufferBase;
  UINT64                FrameBufferSize;
  EFI_STATUS            Status;

  FrameBufferBase = FixedPcdGet64 (PcdMotoG20FramebufferBase);
  FrameBufferSize = FixedPcdGet64 (PcdMotoG20FramebufferSize);
  if ((FrameBufferBase != 0x9E000000ULL) ||
      (FrameBufferSize != MOTO_G20_FB_BYTES) ||
      ((FrameBufferBase & EFI_PAGE_MASK) != 0) ||
      ((FrameBufferSize & EFI_PAGE_MASK) != 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  Status = RegisterFramebufferMemory (FrameBufferBase, FrameBufferSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mMode.FrameBufferBase = FrameBufferBase;
  mMode.FrameBufferSize = (UINTN)FrameBufferSize;
  Status = MotoG20FramebufferSetMode (&mGraphicsOutput, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return gBS->InstallMultipleProtocolInterfaces (
                &mFramebufferHandle,
                &gEfiDevicePathProtocolGuid,
                &mFramebufferDevicePath,
                &gEfiGraphicsOutputProtocolGuid,
                &mGraphicsOutput,
                NULL
                );
}
