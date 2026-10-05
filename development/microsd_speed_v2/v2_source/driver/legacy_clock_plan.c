/* SPDX-License-Identifier: GPL-2.0-only
 * CSD transfer rate semantics cross-checked against Motorola kernel-sprd
 * 261390cdfff66efef9082bd70c1af0abd21d77f2 drivers/mmc/core/sd.c.
 */
#include "legacy_clock_plan.h"
enum sdro_status sd_plan_legacy_clock(const struct sdro_host *h,sd_u32 target,struct sd_legacy_clock_plan *p) {
  static const sd_u32 exp[]={10000u,100000u,1000000u,10000000u};
  static const sd_u32 mant[]={0u,10u,12u,13u,15u,20u,25u,30u,35u,40u,45u,50u,55u,60u,70u,80u};
  sd_u32 speed,unit,mult,limit,classes; sd_u64 blocks; enum sdro_status r;
  if(!h || !p || !target || target>13000000u) return SDRO_ARGUMENT;
  p->actual_hz=0u; p->encoded_divider=0u;
  if(!h->initialized || h->identifying || h->faulted || !h->card.selected || !h->card.rca ||
     !h->clock_hz || h->clock_hz>400000u) return SDRO_NOT_READY;
  if((h->card.ocr&0x80040000u)!=0x80040000u ||
     ((h->card.ocr&0x40000000u)!=0u)!=(h->card.block_addressed!=0)) return SDRO_UNSUPPORTED;
  r=sdro_parse_csd(h->card.csd,h->card.block_addressed,&blocks);
  if(r!=SDRO_OK) return r;
  if(!blocks || blocks!=h->card.blocks) return SDRO_CARD_ERROR;
  speed=h->card.csd[3]; unit=speed&7u; mult=(speed>>3)&15u;
  if((speed&128u) || unit>=4u || mult==0u) return SDRO_UNSUPPORTED;
  limit=exp[unit]*mant[mult]; if(limit<400000u) return SDRO_UNSUPPORTED;
  classes=((sd_u32)h->card.csd[4]<<4)|((sd_u32)h->card.csd[5]>>4);
  if((classes&5u)!=5u) return SDRO_UNSUPPORTED; /* basic + block read */
  p->source_hz=26000000u; p->csd_max_hz=limit; p->requested_hz=target;
  p->bus_width=1u; p->signal_microvolts=3000000u; p->csd_command_classes=classes;
  if(target<limit) limit=target;
  r=sdro_unisoc_divider(p->source_hz,limit,&p->encoded_divider,&p->actual_hz);
  if(r!=SDRO_OK) return r;
  if(p->actual_hz<=h->clock_hz) { p->actual_hz=0u; return SDRO_NOT_READY; }
  return SDRO_OK;
}
