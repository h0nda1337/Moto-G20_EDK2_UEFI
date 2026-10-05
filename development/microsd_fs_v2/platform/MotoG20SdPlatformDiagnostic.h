/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MOTO_G20_SD_PLATFORM_DIAGNOSTIC_H
#define MOTO_G20_SD_PLATFORM_DIAGNOSTIC_H
#include <Uefi.h>
#include "sd_platform_engine.h"
#define MOTO_G20_SD_PLATFORM_DIAGNOSTIC_GUID \
  {0x153ab53f,0x4e1c,0x475e,{0x89,0xa9,0x12,0x44,0x1d,0x7c,0x3e,0x62}}
typedef struct {
  UINT32 Revision;
  UINT32 Size;
  EFI_STATUS MappingStatus;
  EFI_STATUS PrepareStatus;
  EFI_STATUS ExitStatus;
  struct sdp_diag Hardware;
} MOTO_G20_SD_PLATFORM_DIAGNOSTIC;
#endif
