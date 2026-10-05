/* SPDX-License-Identifier: GPL-2.0-only
 * Register semantics derived from Motorola kernel-sprd commit
 * 261390cdfff66efef9082bd70c1af0abd21d77f2 (see SOURCE_MANIFEST.json).
 * Bounded, polled, single-owner adaptation; no DMA and no interrupt generation.
 */
#include "sd_platform_engine.h"
#define B(n) ((sd_u32)1u << (n))
#define AP_GATES (B(22) | B(26))
#define AON_GATES (B(2) | B(3))
#define INFRA_GATES (B(9) | B(10))
#define KEY 0x1bd0u
static const sd_u32 rail_regs[4] = {0x19acu,0x19b0u,0x19b8u,0x19bcu};
static const sd_u32 rail_masks[4] = {1u,255u,1u,255u};
static sd_u32 rd(struct sdp_state *s, sd_u32 a) { return s->ops.read(s->ops.context,a); }
static void wr(struct sdp_state *s, sd_u32 a, sd_u32 v) { s->ops.write(s->ops.context,a,v); }
static void delay(struct sdp_state *s, sd_u32 u) {
  s->ops.delay(s->ops.context,u); s->diag.elapsed_us += u;
}
static enum sdro_status checked(struct sdp_state *s,sd_u32 a,sd_u32 mask,sd_u32 value) {
  sd_u32 old=rd(s,a); wr(s,a,(old & ~mask)|(value & mask));
  s->diag.last_address=a; s->diag.expected=value & mask; s->diag.actual=rd(s,a)&mask;
  return s->diag.actual==s->diag.expected ? SDRO_OK : SDRO_IO_ERROR;
}
static enum sdro_status wait_bits(struct sdp_state *s,sd_u32 a,sd_u32 mask,sd_u32 value,sd_u32 timeout) {
  sd_u32 i;
  for(i=0;i<timeout;i++) {
    s->diag.last_address=a; s->diag.expected=value; s->diag.actual=rd(s,a)&mask;
    if(s->diag.actual==value) return SDRO_OK;
    delay(s,1u);
  }
  return SDRO_TIMEOUT;
}
static enum sdro_status save_update(struct sdp_state *s,sd_u32 a,sd_u32 mask,sd_u32 value,int gate) {
  struct sdp_save *v;
  if(s->saved_count>=12u) return SDRO_RANGE;
  v=&s->saved[s->saved_count++]; v->address=a; v->mask=mask; v->old=rd(s,a)&mask; v->gate=gate;
  if(!gate) return checked(s,a,mask,value);
  wr(s,a+0x1000u,mask);
  s->diag.last_address=a; s->diag.expected=mask; s->diag.actual=rd(s,a)&mask;
  return s->diag.actual==mask ? SDRO_OK : SDRO_IO_ERROR;
}
static enum sdro_status adi_read(struct sdp_state *s,sd_u32 reg,sd_u32 *v) {
  enum sdro_status r; sd_u32 actual;
  wr(s,SDP_ADI+0x28u,SDP_PMIC+reg);
  r=wait_bits(s,SDP_ADI+0x2cu,B(31),0u,2000u); if(r!=SDRO_OK) return r;
  actual=rd(s,SDP_ADI+0x2cu);
  s->diag.last_address=SDP_PMIC+reg; s->diag.expected=reg>>2; s->diag.actual=(actual>>16)&0x7fffu;
  if(s->diag.actual!=s->diag.expected) return SDRO_IO_ERROR;
  *v=actual&0xffffu; return SDRO_OK;
}
static enum sdro_status adi_write(struct sdp_state *s,sd_u32 reg,sd_u32 value) {
  enum sdro_status r=wait_bits(s,SDP_ADI+0x30u,B(10)|B(11),B(10),1000u);
  if(r!=SDRO_OK) return r;
  wr(s,SDP_PMIC+reg,value&0xffffu);
  return wait_bits(s,SDP_ADI+0x30u,B(10),B(10),1000u);
}
static enum sdro_status adi_update(struct sdp_state *s,sd_u32 reg,sd_u32 mask,sd_u32 value) {
  sd_u32 old=0u,actual=0u; enum sdro_status r=adi_read(s,reg,&old);
  if(r!=SDRO_OK) return r;
  r=adi_write(s,reg,(old&~mask)|(value&mask)); if(r!=SDRO_OK) return r;
  r=adi_read(s,reg,&actual); if(r!=SDRO_OK) return r;
  s->diag.last_address=SDP_PMIC+reg; s->diag.expected=value&mask; s->diag.actual=actual&mask;
  return s->diag.actual==s->diag.expected ? SDRO_OK : SDRO_IO_ERROR;
}
static int gates_on(struct sdp_state *s) {
  return s->mapped && s->gates &&
    (rd(s,SDP_AP)&AP_GATES)==AP_GATES &&
    (rd(s,SDP_AON+0x13cu)&AON_GATES)==AON_GATES &&
    (rd(s,SDP_AON+8u)&INFRA_GATES)==INFRA_GATES;
}
int sdp_card_present(struct sdp_state *s) {
  if(!s || !s->mapped || s->stopped || s->removed) return 0;
  if((rd(s,SDP_AON+8u)&B(10))==0u) return -1;
  s->diag.detect=rd(s,SDP_EIC+0xdcu);
  if((s->diag.detect&B(3))!=0u) return 1;
  if(s->ready) { s->removed=1u; s->ready=0u; s->diag.removed=1u; s->diag.ready=0u; s->diag.stage=SDP_REMOVED; s->diag.status=SDRO_NO_CARD; }
  return 0;
}
int sdp_access_ready(struct sdp_state *s) {
  if(!s || !s->ready || s->stopped || s->removed) return 0;
  if(!gates_on(s) || (rd(s,SDP_CLK+0x80u)&7u)!=1u) { s->ready=0u; s->diag.ready=0u; s->diag.status=SDRO_NOT_READY; return 0; }
  if(sdp_card_present(s)!=1) return 0;
  /* Reading host is permitted only after the independent gate checks. */
  if((rd(s,SDP_SD+0x2cu)&0xffc7u)!=0x1107u) { s->ready=0u; s->diag.ready=0u; s->diag.status=SDRO_NOT_READY; return 0; }
  return 1;
}
static enum sdro_status restore_key(struct sdp_state *s) {
  enum sdro_status r;
  if(!s->key_unlocked || !s->key_saved) return SDRO_OK;
  /* Key is a protection register: no fabricated readback success is asserted.
   * Individual SD rail writes are read back separately. */
  r=adi_write(s,KEY,s->key_old); if(r==SDRO_OK) s->key_unlocked=0u; return r;
}
enum sdro_status sdp_stop(struct sdp_state *s) {
  enum sdro_status r=SDRO_OK,t; sd_u32 i;
  if(!s || !s->attempted) return SDRO_ARGUMENT;
  if(s->stopped) return (enum sdro_status)s->diag.cleanup_status;
  s->ready=0u; s->diag.ready=0u;
  if(gates_on(s)) {
    wr(s,SDP_SD+0x38u,0u); wr(s,SDP_SD+0x34u,0u);
    /* Do not restore an inherited DMA/clock session after taking ownership. */
    t=checked(s,SDP_SD+0x2cu,0xffffu,0u); if(t!=SDRO_OK) r=t;
  }
  if(s->mapped && (rd(s,SDP_AON+8u)&B(9))==0u && (s->rails_touched || s->key_unlocked)) {
    /* ADI became inaccessible; do not dereference a gated peripheral even for
     * rollback. Report incomplete restoration and still restore gate ownership. */
    r=SDRO_NOT_READY;
  } else if(s->rails_saved && s->rails_touched) {
    s->key_unlocked=1u;
    t=adi_write(s,KEY,0x6e7fu); if(t!=SDRO_OK) r=t;
    else {
      s->key_unlocked=1u;
      /* Avoid restoring a voltage while the rail is actively driving pins. */
      t=adi_update(s,rail_regs[2],1u,1u); if(t!=SDRO_OK) r=t;
      t=adi_update(s,rail_regs[0],1u,1u); if(t!=SDRO_OK) r=t;
      for(i=1u;i<4u;i+=2u) { t=adi_update(s,rail_regs[i],rail_masks[i],s->diag.rail_old[i]); if(t!=SDRO_OK) r=t; }
      for(i=0u;i<4u;i+=2u) { t=adi_update(s,rail_regs[i],rail_masks[i],s->diag.rail_old[i]); if(t!=SDRO_OK) r=t; }
    }
  }
  if(s->mapped && (rd(s,SDP_AON+8u)&B(9))!=0u) {
    t=restore_key(s); if(t!=SDRO_OK) r=t;
  }
  while(s->saved_count) {
    struct sdp_save *v=&s->saved[--s->saved_count];
    if(v->gate) {
      wr(s,v->address+0x2000u,v->mask&~v->old);
      wr(s,v->address+0x1000u,v->old);
      t=((rd(s,v->address)&v->mask)==v->old)?SDRO_OK:SDRO_IO_ERROR;
    } else t=checked(s,v->address,v->mask,v->old);
    if(t!=SDRO_OK) r=t;
  }
  s->stopped=1u; s->diag.stopped=1u; s->diag.cleanup_status=(sd_u32)r;
  return r;
}
static enum sdro_status fail(struct sdp_state *s,enum sdro_status r) {
  sd_u32 a=s->diag.last_address,e=s->diag.expected,v=s->diag.actual;
  s->diag.status=(sd_u32)r;
  (void)sdp_stop(s); s->diag.last_address=a; s->diag.expected=e; s->diag.actual=v;
  return r;
}
#define TRY(expr) do { r=(expr); if(r!=SDRO_OK) return fail(s,r); } while(0)
enum sdro_status sdp_prepare(struct sdp_state *s,const struct sdp_ops *ops) {
  static const sd_u32 regions[][2]={{SDP_SD,0x1000u},{SDP_AP,0x3000u},{SDP_CLK,0x1000u},
    {SDP_AON,0x3000u},{SDP_ADI,0x1000u},{SDP_PMIC,0x2000u},{SDP_EIC,0x1000u},{SDP_PIN,0x1000u}};
  enum sdro_status r; sd_u32 i;
  if(!s || !ops || !ops->map || !ops->read || !ops->write || !ops->delay) return SDRO_ARGUMENT;
  if(s->attempted) return SDRO_NOT_READY;
  s->ops=*ops; s->attempted=1u; s->diag.attempted=1u; s->diag.status=SDRO_NOT_READY;
  s->diag.stage=SDP_MAP;
  for(i=0u;i<8u;i++) if(!ops->map(ops->context,regions[i][0],regions[i][1])) return fail(s,SDRO_NOT_READY);
  s->mapped=1u; s->diag.stage=SDP_INFRA;
  TRY(save_update(s,SDP_AON+8u,INFRA_GATES,INFRA_GATES,1));
  s->diag.stage=SDP_DETECT;
  if(sdp_card_present(s)!=1) return fail(s,SDRO_NO_CARD);
  delay(s,1000u); if(sdp_card_present(s)!=1) return fail(s,SDRO_NO_CARD);
  s->diag.stage=SDP_GATES;
  TRY(save_update(s,SDP_AP,AP_GATES,AP_GATES,1));
  TRY(save_update(s,SDP_AON+0x13cu,AON_GATES,AON_GATES,1));
  s->gates=1u;
  TRY(checked(s,SDP_SD+0x2cu,0xffffu,0u));
  TRY(save_update(s,SDP_CLK+0x80u,7u,1u,0)); s->diag.mux=rd(s,SDP_CLK+0x80u);
  s->diag.stage=SDP_RESET;
  wr(s,SDP_SD+0x38u,0u); wr(s,SDP_SD+0x34u,0u);
  wr(s,SDP_SD+0x2cu,B(24)); TRY(wait_bits(s,SDP_SD+0x2cu,0x07000000u,0u,100000u));
  TRY(checked(s,SDP_SD+0x2cu,0x000f0000u,0x000e0000u));
  s->diag.timeout=(rd(s,SDP_SD+0x2cu)>>16)&15u;
  s->diag.host_version=rd(s,SDP_SD+0xfcu); s->diag.host_capabilities=rd(s,SDP_SD+0x40u);
  if(s->diag.host_version==0xffffffffu || s->diag.host_capabilities==0xffffffffu) return fail(s,SDRO_IO_ERROR);
  s->diag.stage=SDP_PINS;
  for(i=0u;i<6u;i++) {
    s->diag.pin_common[i]=rd(s,SDP_PIN+0xbcu+i*4u); s->diag.pin_misc[i]=rd(s,SDP_PIN+0x4bcu+i*4u);
    if(s->diag.pin_common[i]==0xffffffffu || s->diag.pin_misc[i]==0xffffffffu) return fail(s,SDRO_IO_ERROR);
  }
  s->diag.pins_inherited=1u; /* Does not certify AF, pull or drive correctness. */
  s->diag.stage=SDP_RAILS;
  TRY(adi_read(s,KEY,&s->key_old)); s->key_saved=1u;
  for(i=0u;i<4u;i++) TRY(adi_read(s,rail_regs[i],&s->diag.rail_old[i]));
  s->rails_saved=1u;
  if((s->diag.rail_old[0]&1u) || (s->diag.rail_old[2]&1u) ||
     (s->diag.rail_old[1]&255u)!=180u || (s->diag.rail_old[3]&255u)!=180u) {
    s->key_unlocked=1u; TRY(adi_write(s,KEY,0x6e7fu)); s->rails_touched=1u;
    TRY(adi_update(s,rail_regs[2],1u,1u)); TRY(adi_update(s,rail_regs[0],1u,1u)); delay(s,5000u);
    TRY(adi_update(s,rail_regs[1],255u,180u)); TRY(adi_update(s,rail_regs[3],255u,180u));
    TRY(adi_update(s,rail_regs[0],1u,0u)); delay(s,200u); TRY(adi_update(s,rail_regs[2],1u,0u)); delay(s,5000u);
    TRY(restore_key(s));
  }
  for(i=0u;i<4u;i++) { TRY(adi_read(s,rail_regs[i],&s->diag.rail_new[i])); if((s->diag.rail_new[i]&rail_masks[i])!=((i&1u)?180u:0u)) return fail(s,SDRO_IO_ERROR); }
  s->diag.stage=SDP_HOTPLUG;
  /* Motorola stores raw cd-gpios DT flags here (0 on this device), not
   * MMC_CAP2_CD_ACTIVE_HIGH. fast_hotplug_enable sets detect_pol for flags=0. */
  TRY(save_update(s,SDP_AON+0x444u,B(2)|B(3)|B(7)|0x00ff0000u,B(2)|B(3)|B(7)|0x00030000u,0));
  s->diag.stage=SDP_TIMING;
  TRY(checked(s,SDP_SD+0x28u,0x3eu,0u)); /* one bit, legacy, no DMA mode */
  TRY(checked(s,SDP_SD+0x3cu,0x000f0000u,0u)); /* SDR12 and 3.0 V */
  TRY(checked(s,SDP_SD+0x200u,0x0f250000u,0u)); /* DLL off */
  TRY(checked(s,SDP_SD+0x204u,0xffffffffu,0u));
  TRY(checked(s,SDP_SD+0x208u,B(13)|B(21),B(13)|B(21))); s->diag.dll=rd(s,SDP_SD+0x208u);
  TRY(checked(s,SDP_SD+0x250u,B(24)|B(25),0u)); /* no automatic clock gating */
  wr(s,SDP_SD+0x38u,0u); wr(s,SDP_SD+0x34u,0u); wr(s,SDP_SD+0x30u,0xffffffffu);
  s->diag.stage=SDP_CLOCK;
  TRY(checked(s,SDP_SD+0x2cu,0xfffdu,0x1101u));
  TRY(wait_bits(s,SDP_SD+0x2cu,B(1),B(1),100000u));
  TRY(checked(s,SDP_SD+0x2cu,B(2),B(2))); delay(s,1000u);
  s->diag.clock=rd(s,SDP_SD+0x2cu); s->diag.host_present=rd(s,SDP_SD+0x24u);
  if(sdp_card_present(s)!=1) return fail(s,SDRO_NO_CARD);
  s->ready=1u; if(!sdp_access_ready(s)) return fail(s,SDRO_NOT_READY);
  s->diag.ready=1u; s->diag.stage=SDP_READY; s->diag.status=SDRO_OK;
  return SDRO_OK;
}
sd_u32 sdp_read32(void *context,sd_u32 offset) {
  struct sdp_state *s=context;
  if((offset&3u) || offset>=0x1000u || !sdp_access_ready(s)) return 0xffffffffu;
  return rd(s,SDP_SD+offset);
}
void sdp_write32(void *context,sd_u32 offset,sd_u32 value) {
  struct sdp_state *s=context;
  if((offset&3u) || offset>=0x1000u || !sdp_access_ready(s)) return;
  wr(s,SDP_SD+offset,value);
}
void sdp_delay_us(void *context,sd_u32 value) { struct sdp_state *s=context; if(s && !s->stopped) delay(s,value); }
