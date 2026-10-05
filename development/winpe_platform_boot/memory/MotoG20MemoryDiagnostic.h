/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MOTO_G20_MEMORY_DIAGNOSTIC_H
#define MOTO_G20_MEMORY_DIAGNOSTIC_H
#include <Uefi.h>
#define MOTO_G20_MEMORY_DIAGNOSTIC_GUID \
  { 0x615d4cb2, 0x12c4, 0x4916, { 0x89, 0x69, 0x5a, 0x1c, 0x8f, 0x84, 0x5e, 0x31 } }
typedef struct {
  UINT32 Revision, Stage;
  EFI_STATUS Status;
  UINT32 PlanResult, ReservedRanges, PublishedRegions;
  UINT64 PlannedBytes, MappedBytes, PublishedBytes;
} MOTO_G20_MEMORY_DIAGNOSTIC;
#endif
