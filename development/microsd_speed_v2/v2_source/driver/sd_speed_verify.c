/* SPDX-License-Identifier: GPL-2.0-only */
#include "sd_speed_verify.h"
#include "legacy_clock_plan.h"
static sd_u32 le16(const sd_u8 *p) { return (sd_u32)p[0]|((sd_u32)p[1]<<8); }
static sd_u32 le32(const sd_u8 *p) { return le16(p)|(le16(p+2)<<16); }
static int same(const sd_u8 *a,const sd_u8 *b,sd_u32 n) { sd_u32 i; for(i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
static enum sdro_status fail_verify(struct sdro_host *h,const struct sd_speed_ops *o,struct sd_speed_diag *d,enum sdro_status r) {
  d->status=(sd_u32)r; d->stop_status=(sd_u32)o->stop(o->context);
  h->faulted=1; h->card.selected=0; return r;
}
enum sdro_status sd_verify_transfer_clock(struct sdro_host *h,const struct sd_speed_ops *o,struct sd_speed_diag *d) {
  static const sd_u64 lbas[3]={0u,2048u,99072u};
  static const sd_u8 marker[]="MOTO G20 MICROSD READ TEST V1\r\n";
  sd_u8 before[3][512],after[512]; sd_u32 i,actual=0u,total,partition,spc,reserved,fat_count,fat_size;
  sd_u64 data_start; struct sd_legacy_clock_plan plan; enum sdro_status r;
  if(!h||!o||!d||!o->read||!o->card_status||!o->change_clock||!o->stop) return SDRO_ARGUMENT;
  d->stage=1u; d->status=SDRO_NOT_READY; d->stop_status=SDRO_NOT_READY;
  d->target_hz=0u; d->actual_hz=h->clock_hz; d->before_mask=0u; d->after_mask=0u; d->failed_lba=0u;
  r=o->card_status(o->context); if(r!=SDRO_OK) return fail_verify(h,o,d,r);
  d->stage=2u;
  for(i=0u;i<3u;i++) {
    d->failed_lba=lbas[i]; r=o->read(o->context,lbas[i],before[i]);
    if(r!=SDRO_OK) return fail_verify(h,o,d,r);
    d->before_mask|=1u<<i;
  }
  /* This trial deliberately targets the already prepared FAT32 card layout.
   * A changed layout must be recaptured instead of silently trusting hardcoded
   * marker placement. Full before/after sector comparisons follow below. */
  if(before[0][510]!=0x55u||before[0][511]!=0xaau||
     (before[0][450]!=0x0bu&&before[0][450]!=0x0cu)||le32(before[0]+454)!=2048u||
     before[1][510]!=0x55u||before[1][511]!=0xaau||le16(before[1]+11)!=512u||
     le16(before[1]+17)!=0u||le16(before[1]+22)!=0u||le32(before[1]+28)!=2048u||
     !same(before[2],marker,(sd_u32)sizeof(marker)-1u)) return fail_verify(h,o,d,SDRO_CARD_ERROR);
  partition=le32(before[0]+458); total=le32(before[1]+32); spc=before[1][13];
  reserved=le16(before[1]+14); fat_count=before[1][16]; fat_size=le32(before[1]+36);
  data_start=2048u+(sd_u64)reserved+(sd_u64)fat_count*fat_size;
  if(!partition||!total||total>partition||(sd_u64)partition+2048u>h->card.blocks||
     !spc||(spc&(spc-1u))||spc>128u||!reserved||!fat_count||fat_count>2u||!fat_size||
     data_start>99072u||99072u>=(sd_u64)2048u+total) return fail_verify(h,o,d,SDRO_CARD_ERROR);
  d->stage=3u; r=sd_plan_legacy_clock(h,13000000u,&plan);
  if(r!=SDRO_OK) return fail_verify(h,o,d,r);
  d->target_hz=plan.actual_hz;
  d->stage=4u; r=o->change_clock(o->context,plan.actual_hz,&actual);
  if(r!=SDRO_OK) return fail_verify(h,o,d,r);
  if(actual!=plan.actual_hz||actual>plan.csd_max_hz) return fail_verify(h,o,d,SDRO_IO_ERROR);
  h->clock_hz=actual; d->actual_hz=actual;
  d->stage=5u; r=o->card_status(o->context);
  if(r!=SDRO_OK) return fail_verify(h,o,d,r);
  d->stage=6u;
  for(i=0u;i<3u;i++) {
    d->failed_lba=lbas[i]; r=o->read(o->context,lbas[i],after);
    if(r!=SDRO_OK) return fail_verify(h,o,d,r);
    if(!same(before[i],after,512u)) return fail_verify(h,o,d,SDRO_IO_ERROR);
    d->after_mask|=1u<<i;
  }
  r=o->card_status(o->context); if(r!=SDRO_OK) return fail_verify(h,o,d,r);
  d->stage=7u; d->status=SDRO_OK; d->failed_lba=0u; return SDRO_OK;
}
