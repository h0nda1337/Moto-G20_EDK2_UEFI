/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MOTO_SD_SPEED_VERIFY_H
#define MOTO_SD_SPEED_VERIFY_H
#include "sdhci_ro.h"
struct sd_speed_ops {
  void *context;
  enum sdro_status (*read)(void *,sd_u64,sd_u8 *);
  enum sdro_status (*card_status)(void *);
  enum sdro_status (*change_clock)(void *,sd_u32,sd_u32 *);
  enum sdro_status (*stop)(void *);
};
struct sd_speed_diag {
  sd_u32 stage,status,stop_status,target_hz,actual_hz,before_mask,after_mask;
  sd_u64 failed_lba;
};
enum sdro_status sd_verify_transfer_clock(struct sdro_host *,const struct sd_speed_ops *,struct sd_speed_diag *);
#endif
