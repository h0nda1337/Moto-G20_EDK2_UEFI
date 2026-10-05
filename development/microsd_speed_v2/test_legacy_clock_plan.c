/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "legacy_clock_plan.h"
static void init(struct sdro_host *h) {
  static const sd_u8 csd[16]={0x40,0x0e,0x00,0x32,0x5b,0x59,0x00,0x00,0xe6,0x9f,0x7f,0x80,0x0a,0x40,0x00,0x00};
  memset(h,0,sizeof(*h)); h->initialized=1; h->clock_hz=26000000u/(4u*17u);
  h->card.selected=1; h->card.rca=0x1234u; h->card.ocr=0xc0040000u; h->card.block_addressed=1;
  memcpy(h->card.csd,csd,16u); h->card.blocks=60456960u;
}
int main(void) {
  struct sdro_host h,before; struct sd_legacy_clock_plan p; unsigned n=0;
  init(&h); before=h; assert(sd_plan_legacy_clock(&h,13000000u,&p)==SDRO_OK);
  assert(p.actual_hz==13000000u&&p.encoded_divider==0u&&p.csd_max_hz==25000000u);
  assert(memcmp(&h,&before,sizeof(h))==0); n++;
  assert(sd_plan_legacy_clock(&h,6500000u,&p)==SDRO_OK&&p.actual_hz==6500000u&&p.encoded_divider==0x100u); n++;
  assert(sd_plan_legacy_clock(&h,1000000u,&p)==SDRO_OK&&p.actual_hz==928571u); n++;
  assert(sd_plan_legacy_clock(&h,25000000u,&p)==SDRO_ARGUMENT); n++;
  assert(sd_plan_legacy_clock(&h,0u,&p)==SDRO_ARGUMENT); n++;
  assert(sd_plan_legacy_clock(NULL,13000000u,&p)==SDRO_ARGUMENT); n++;
  assert(sd_plan_legacy_clock(&h,13000000u,NULL)==SDRO_ARGUMENT); n++;
  init(&h); h.card.selected=0; assert(sd_plan_legacy_clock(&h,13000000u,&p)==SDRO_NOT_READY); n++;
  init(&h); h.faulted=1; assert(sd_plan_legacy_clock(&h,13000000u,&p)==SDRO_NOT_READY); n++;
  init(&h); h.card.ocr=0x40040000u; assert(sd_plan_legacy_clock(&h,13000000u,&p)==SDRO_UNSUPPORTED); n++;
  init(&h); h.card.ocr=0xc0010000u; assert(sd_plan_legacy_clock(&h,13000000u,&p)==SDRO_UNSUPPORTED); n++;
  init(&h); h.card.blocks--; assert(sd_plan_legacy_clock(&h,13000000u,&p)==SDRO_CARD_ERROR); n++;
  init(&h); h.card.csd[3]=0u; assert(sd_plan_legacy_clock(&h,13000000u,&p)==SDRO_UNSUPPORTED); n++;
  init(&h); h.card.csd[3]=0xb2u; assert(sd_plan_legacy_clock(&h,13000000u,&p)==SDRO_UNSUPPORTED); n++;
  init(&h); h.card.csd[3]=0x34u; assert(sd_plan_legacy_clock(&h,13000000u,&p)==SDRO_UNSUPPORTED); n++;
  init(&h); h.card.csd[4]=0; h.card.csd[5]=0x19u; assert(sd_plan_legacy_clock(&h,13000000u,&p)==SDRO_UNSUPPORTED); n++;
  init(&h); h.card.csd[3]=0x11u; assert(sd_plan_legacy_clock(&h,13000000u,&p)==SDRO_OK);
  assert(p.csd_max_hz==1200000u&&p.actual_hz==1083333u); n++;
  init(&h); assert(sd_plan_legacy_clock(&h,400000u,&p)==SDRO_NOT_READY&&p.actual_hz==0u); n++;
  init(&h); h.clock_hz=13000000u; assert(sd_plan_legacy_clock(&h,13000000u,&p)==SDRO_NOT_READY); n++;
  printf("PASS: %u legacy clock plan cases; no MMIO, not integrated in V1\n",n);
  return 0;
}
