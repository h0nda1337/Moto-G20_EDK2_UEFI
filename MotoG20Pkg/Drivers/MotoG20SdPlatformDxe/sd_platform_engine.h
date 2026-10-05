/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MOTO_SD_PLATFORM_ENGINE_H
#define MOTO_SD_PLATFORM_ENGINE_H
#include "sdhci_ro.h"

#define SDP_SD 0x71100000u
#define SDP_AP 0x71000000u
#define SDP_CLK 0x20200000u
#define SDP_AON 0x327d0000u
#define SDP_ADI 0x32100000u
#define SDP_PMIC 0x32120000u
#define SDP_EIC 0x32230000u
#define SDP_PIN 0x32450000u

enum sdp_stage { SDP_NEW, SDP_MAP, SDP_INFRA, SDP_DETECT, SDP_GATES,
  SDP_RESET, SDP_PINS, SDP_RAILS, SDP_HOTPLUG, SDP_TIMING, SDP_CLOCK,
  SDP_READY, SDP_REMOVED, SDP_STOPPED };
struct sdp_ops {
  void *context;
  int (*map)(void *, sd_u32, sd_u32);
  sd_u32 (*read)(void *, sd_u32);
  void (*write)(void *, sd_u32, sd_u32);
  void (*delay)(void *, sd_u32);
};
struct sdp_save { sd_u32 address, mask, old; int gate; };
struct sdp_diag {
  sd_u32 stage, status, cleanup_status, attempted, ready, removed, stopped;
  sd_u32 last_address, expected, actual, elapsed_us;
  sd_u32 pin_common[6], pin_misc[6], pins_inherited;
  sd_u32 rail_old[4], rail_new[4], detect, mux, clock, timeout;
  sd_u32 host_version, host_capabilities, host_present, dll;
};
struct sdp_state {
  struct sdp_ops ops;
  struct sdp_diag diag;
  struct sdp_save saved[12];
  sd_u32 saved_count, mapped, gates, rails_saved, rails_touched;
  sd_u32 key_saved, key_old, key_unlocked, attempted, ready, removed, stopped;
};
/* Fresh state must be zero initialized. One preparation attempt per boot.
 * No pins are changed: observed inherited settings remain an explicit hypothesis
 * until the upper layer identifies the card. No write/erase SD commands exist.
 */
enum sdro_status sdp_prepare(struct sdp_state *, const struct sdp_ops *);
enum sdro_status sdp_stop(struct sdp_state *);
int sdp_access_ready(struct sdp_state *);
int sdp_card_present(struct sdp_state *);
sd_u32 sdp_read32(void *, sd_u32);
void sdp_write32(void *, sd_u32, sd_u32);
void sdp_delay_us(void *, sd_u32);
#endif
