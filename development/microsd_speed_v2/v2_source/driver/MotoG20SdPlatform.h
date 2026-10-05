/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MOTO_G20_SD_PLATFORM_H
#define MOTO_G20_SD_PLATFORM_H
#include <Uefi.h>
#include "sdhci_ro.h"

#define MOTO_G20_SD_PLATFORM_GUID \
  {0x0ad366d1, 0x94dd, 0x47c2, {0xbd, 0x56, 0x53, 0x0e, 0x01, 0x87, 0x76, 0x4d}}
#define MOTO_G20_SD_PLATFORM_REVISION 2u
#define MOTO_G20_SD_BASE 0x71100000ULL

typedef struct {
  UINT32 SupplyMicrovolts;
  UINT32 SignalMicrovolts;
  UINT32 ClockHz;
  UINT32 BusWidth;
  struct sdro_io Io;
} MOTO_G20_SD_CONFIGURATION;

typedef struct _MOTO_G20_SD_PLATFORM MOTO_G20_SD_PLATFORM;
struct _MOTO_G20_SD_PLATFORM {
  UINT32 Revision;
  UINT64 ControllerBase;
  /* Required provider, not implemented by the read-only core. Must initialize
   * only the removable SD controller: MMIO map, clocks, reset, EIC detection,
   * PMIC 3.0V rails, pinmux and legacy timing. Returns failure on any unverified
   * setup. Io callbacks expose only bounded, aligned SDHC offsets. Firmware must
   * quiesce the host on ExitBootServices and prevent use after physical removal.
   */
  EFI_STATUS (EFIAPI *Prepare)(MOTO_G20_SD_PLATFORM *, MOTO_G20_SD_CONFIGURATION *);
  /* Revision2: caller serializes and certifies selected card limits. No card
   * commands are issued here. Non-success must not be silently ignored. */
  EFI_STATUS (EFIAPI *SetTransferClock)(MOTO_G20_SD_PLATFORM *, UINT32, UINT32 *);
  EFI_STATUS (EFIAPI *Stop)(MOTO_G20_SD_PLATFORM *);
};
#endif
