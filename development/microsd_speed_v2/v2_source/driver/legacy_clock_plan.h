/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MOTO_SD_LEGACY_CLOCK_PLAN_H
#define MOTO_SD_LEGACY_CLOCK_PLAN_H
#include "sdhci_ro.h"
struct sd_legacy_clock_plan {
  sd_u32 source_hz, csd_max_hz, requested_hz, actual_hz, encoded_divider;
  sd_u32 bus_width, signal_microvolts, csd_command_classes;
};
/* Pure plan; does not alter clocks, host fields, card registers or protocols.
 * Requires a successfully identified selected card. First proposal deliberately
 * keeps ext-26MHz, 1bit, 3V, SDR12 and cannot exceed 13MHz (source/2).
 */
enum sdro_status sd_plan_legacy_clock(const struct sdro_host *, sd_u32 target_hz, struct sd_legacy_clock_plan *);
#endif
