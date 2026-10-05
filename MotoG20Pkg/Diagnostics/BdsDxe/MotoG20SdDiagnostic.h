/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MOTO_G20_SD_DIAGNOSTIC_H
#define MOTO_G20_SD_DIAGNOSTIC_H
#include <Uefi.h>
#define MOTO_G20_SD_DIAGNOSTIC_GUID \
  {0xe96573df, 0x2092, 0x4a43, {0xa1, 0x61, 0x1e, 0x03, 0x82, 0x5b, 0x15, 0x96}}
typedef struct {
  UINT32 Revision;
  UINT32 Stage; /* 0 missing platform, 1 prepare, 2 identify, 3 block I/O, 4 fault */
  EFI_STATUS Status;
  UINT32 CoreStatus;
  UINT32 LastCommand;
  UINT32 LastIrq;
  UINT32 LastResponse;
  UINT32 Commands;
  UINT32 BlocksRead;
  UINT64 CapacityBlocks;
  BOOLEAN Published;
} MOTO_G20_SD_DIAGNOSTIC;
#endif
