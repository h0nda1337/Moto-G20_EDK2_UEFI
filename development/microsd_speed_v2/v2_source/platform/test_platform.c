/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sd_platform_engine.h"
#define B(n) ((sd_u32)1u<<(n))
struct reg { sd_u32 a,v; };
struct fake {
  struct reg regs[128]; sd_u32 count,reads,writes,maps,elapsed,host_reads,host_writes;
  sd_u32 ignore_write,fail_map,adi_busy,adi_fifo_bad,adi_echo_bad,reset_stuck,clock_stuck;
  sd_u32 fail_fast_clock, lose_gate_on_delay, remove_on_delay, commands;
};
static sd_u32 *reg(struct fake *f,sd_u32 a) {
  sd_u32 i; for(i=0;i<f->count;i++) if(f->regs[i].a==a) return &f->regs[i].v;
  assert(f->count<128u); f->regs[f->count].a=a; return &f->regs[f->count++].v;
}
static sd_u32 raw(struct fake *f,sd_u32 a) { return *reg(f,a); }
static int host(sd_u32 a) { return a>=SDP_SD&&a<SDP_SD+0x1000u; }
static void check_host(struct fake *f) {
  assert((raw(f,SDP_AP)&(B(22)|B(26)))==(B(22)|B(26)));
  assert((raw(f,SDP_AON+0x13cu)&12u)==12u);
}
static sd_u32 read32(void *c,sd_u32 a) {
  struct fake *f=c; f->reads++; assert((a&3u)==0u);
  if(host(a)) { check_host(f); f->host_reads++; }
  if(a==SDP_ADI+0x2cu&&f->adi_busy) return B(31);
  if(a==SDP_ADI+0x30u) return f->adi_fifo_bad?0u:B(10);
  return raw(f,a);
}
static void write32(void *c,sd_u32 a,sd_u32 v) {
  struct fake *f=c; f->writes++; assert((a&3u)==0u);
  assert(a<0x71200000u || a>=0x71500000u); /* no Wi-Fi/eMMC */
  if(host(a)) { check_host(f); f->host_writes++; }
  if(a==SDP_SD+0x0cu) f->commands++;
  assert(a!=SDP_SD+0x20u); /* Never write card data FIFO. */
  if(f->ignore_write==a) return;
  if(a==SDP_AON+0x1008u||a==SDP_AON+0x113cu||a==SDP_AP+0x1000u) { *reg(f,a-0x1000u)|=v; return; }
  if(a==SDP_AON+0x2008u||a==SDP_AON+0x213cu||a==SDP_AP+0x2000u) { *reg(f,a-0x2000u)&=~v; return; }
  if(a==SDP_ADI+0x28u) { *reg(f,SDP_ADI+0x2cu)=(((v-SDP_PMIC)>>2)<<16)|raw(f,v); if(f->adi_echo_bad) *reg(f,SDP_ADI+0x2cu)^=B(16); return; }
  if(a>=SDP_PMIC&&a<SDP_PMIC+0x2000u) {
    assert(a==SDP_PMIC+0x1bd0u||a==SDP_PMIC+0x19acu||a==SDP_PMIC+0x19b0u||a==SDP_PMIC+0x19b8u||a==SDP_PMIC+0x19bcu);
    if(a!=SDP_PMIC+0x1bd0u) assert(raw(f,SDP_PMIC+0x1bd0u)==0x6e7fu);
  }
  if(a==SDP_SD+0x2cu) {
    if((v&B(24))&&!f->reset_stuck) v=0u;
    v&=~B(1); if((v&B(0))&&!f->clock_stuck&&!(f->fail_fast_clock&&(v&0xffc0u)==0u)) v|=B(1);
  }
  if(a==SDP_SD+0x30u) { *reg(f,a)&=~v; return; }
  *reg(f,a)=v;
}
static int map(void *c,sd_u32 a,sd_u32 n) {
  struct fake *f=c; f->maps++; assert(n==0x1000u||n==0x2000u||n==0x3000u);
  assert(a==SDP_SD||a==SDP_AP||a==SDP_CLK||a==SDP_AON||a==SDP_ADI||a==SDP_PMIC||a==SDP_EIC||a==SDP_PIN);
  return f->fail_map!=a;
}
static void pause_us(void *c,sd_u32 u) {
  struct fake *f=c; f->elapsed+=u; assert(f->elapsed<500000u);
  if(f->lose_gate_on_delay) *reg(f,SDP_AP)&=~B(22);
  if(f->remove_on_delay) *reg(f,SDP_EIC+0xdcu)=0u;
}
static void init(struct fake *f,struct sdp_state *s,struct sdp_ops *o) {
  memset(f,0,sizeof(*f)); memset(s,0,sizeof(*s));
  o->context=f; o->map=map; o->read=read32; o->write=write32; o->delay=pause_us;
  *reg(f,SDP_EIC+0xdcu)=8u;
  *reg(f,SDP_AP)=B(5); *reg(f,SDP_AON+8u)=B(2); *reg(f,SDP_AON+0x13cu)=B(8);
  *reg(f,SDP_CLK+0x80u)=0x104u; *reg(f,SDP_AON+0x444u)=0x010a0044u;
  *reg(f,SDP_PMIC+0x1bd0u)=0u;
  *reg(f,SDP_PMIC+0x19acu)=0x21u; *reg(f,SDP_PMIC+0x19b0u)=0x1123u;
  *reg(f,SDP_PMIC+0x19b8u)=0x41u; *reg(f,SDP_PMIC+0x19bcu)=0x2256u;
}
static void restored(struct fake *f) {
  assert(raw(f,SDP_AP)==B(5)); assert(raw(f,SDP_AON+8u)==B(2));
  assert(raw(f,SDP_AON+0x13cu)==B(8)); assert(raw(f,SDP_CLK+0x80u)==0x104u);
  assert(raw(f,SDP_AON+0x444u)==0x010a0044u);
  assert(raw(f,SDP_PMIC+0x19acu)==0x21u); assert(raw(f,SDP_PMIC+0x19b0u)==0x1123u);
  assert(raw(f,SDP_PMIC+0x19b8u)==0x41u); assert(raw(f,SDP_PMIC+0x19bcu)==0x2256u);
  assert(raw(f,SDP_PMIC+0x1bd0u)==0u);
}
int main(void) {
  struct fake f; struct sdp_state s; struct sdp_ops o; sd_u32 n,i,hr,hz;
  unsigned tests=0;
  init(&f,&s,&o); assert(sdp_prepare(NULL,&o)==SDRO_ARGUMENT); assert(f.writes==0u); tests++;
  init(&f,&s,&o); o.map=NULL; assert(sdp_prepare(&s,&o)==SDRO_ARGUMENT); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK); assert(s.diag.ready==1u);
  assert(s.diag.timeout==14u); assert(s.diag.pins_inherited==1u); assert(sdp_access_ready(&s)==1);
  assert(raw(&f,SDP_SD+0x2cu)==0xe1107u); assert(raw(&f,SDP_SD+0x38u)==0u);
  assert(raw(&f,SDP_CLK+0x80u)==0x101u); assert(raw(&f,SDP_PMIC+0x19b0u)==0x11b4u);
  /* DT cd-gpios flags=0: Motorola's fast-hotplug block sets detect_pol. */
  assert(raw(&f,SDP_AON+0x444u)==0x010300ccu);
  n=f.writes; assert(sdp_prepare(&s,&o)==SDRO_NOT_READY); assert(f.writes==n);
  assert(sdp_stop(&s)==SDRO_OK); restored(&f); n=f.writes; assert(sdp_stop(&s)==SDRO_OK&&f.writes==n); tests++;
  init(&f,&s,&o); f.fail_map=SDP_PIN; assert(sdp_prepare(&s,&o)==SDRO_NOT_READY); assert(f.reads==0u&&f.writes==0u); tests++;
  init(&f,&s,&o); *reg(&f,SDP_EIC+0xdcu)=0u; assert(sdp_prepare(&s,&o)==SDRO_NO_CARD); assert(f.host_reads==0u); restored(&f); tests++;
  init(&f,&s,&o); f.ignore_write=SDP_AON+0x1008u; assert(sdp_prepare(&s,&o)==SDRO_IO_ERROR); assert(f.host_reads==0u); tests++;
  init(&f,&s,&o); f.ignore_write=SDP_AP+0x1000u; assert(sdp_prepare(&s,&o)==SDRO_IO_ERROR); assert(f.host_reads==0u); restored(&f); tests++;
  init(&f,&s,&o); f.ignore_write=SDP_CLK+0x80u; assert(sdp_prepare(&s,&o)==SDRO_IO_ERROR); restored(&f); tests++;
  init(&f,&s,&o); f.reset_stuck=1u; assert(sdp_prepare(&s,&o)==SDRO_TIMEOUT); assert(f.elapsed>=100000u); restored(&f); tests++;
  init(&f,&s,&o); f.clock_stuck=1u; assert(sdp_prepare(&s,&o)==SDRO_TIMEOUT); assert(s.diag.stage==SDP_CLOCK); restored(&f); tests++;
  init(&f,&s,&o); f.adi_busy=1u; assert(sdp_prepare(&s,&o)==SDRO_TIMEOUT); restored(&f); tests++;
  init(&f,&s,&o); f.adi_echo_bad=1u; assert(sdp_prepare(&s,&o)==SDRO_IO_ERROR); restored(&f); tests++;
  init(&f,&s,&o); f.adi_fifo_bad=1u; assert(sdp_prepare(&s,&o)==SDRO_TIMEOUT); restored(&f); tests++;
  init(&f,&s,&o); f.ignore_write=SDP_PMIC+0x19b0u; assert(sdp_prepare(&s,&o)==SDRO_IO_ERROR); restored(&f); tests++;
  init(&f,&s,&o); f.ignore_write=SDP_AON+0x444u; assert(sdp_prepare(&s,&o)==SDRO_IO_ERROR); restored(&f); tests++;
  init(&f,&s,&o); *reg(&f,SDP_SD+0x204u)=1u; f.ignore_write=SDP_SD+0x204u;
  assert(sdp_prepare(&s,&o)==SDRO_IO_ERROR); restored(&f); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK); hr=f.host_reads;
  *reg(&f,SDP_AP)&=~B(22); assert(sdp_access_ready(&s)==0); assert(f.host_reads==hr); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK); *reg(&f,SDP_EIC+0xdcu)=0u;
  assert(sdp_card_present(&s)==0); assert(s.removed==1u); *reg(&f,SDP_EIC+0xdcu)=8u;
  assert(sdp_access_ready(&s)==0); assert(sdp_card_present(&s)==0); assert(sdp_stop(&s)==SDRO_OK); restored(&f); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK); n=f.writes; hr=f.host_reads;
  sdp_write32(&s,1u,0u); sdp_write32(&s,0x1000u,0u); assert(sdp_read32(&s,3u)==0xffffffffu);
  assert(f.writes==n&&f.host_reads==hr); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK); *reg(&f,SDP_CLK+0x80u)=0u;
  hr=f.host_reads; assert(sdp_access_ready(&s)==0); assert(f.host_reads==hr); tests++;
  init(&f,&s,&o); *reg(&f,SDP_PMIC+0x19acu)=0u; *reg(&f,SDP_PMIC+0x19b8u)=0u;
  *reg(&f,SDP_PMIC+0x19b0u)=180u; *reg(&f,SDP_PMIC+0x19bcu)=180u;
  assert(sdp_prepare(&s,&o)==SDRO_OK); assert(s.rails_touched==0u); assert(sdp_stop(&s)==SDRO_OK);
  assert(raw(&f,SDP_PMIC+0x19b0u)==180u&&raw(&f,SDP_PMIC+0x19acu)==0u); tests++;
  init(&f,&s,&o); for(i=0;i<6u;i++) { *reg(&f,SDP_PIN+0xbcu+i*4u)=0x30u+i; *reg(&f,SDP_PIN+0x4bcu+i*4u)=0xc0u+i; }
  assert(sdp_prepare(&s,&o)==SDRO_OK); for(i=0;i<6u;i++) assert(s.diag.pin_common[i]==0x30u+i&&s.diag.pin_misc[i]==0xc0u+i);
  assert(sdp_stop(&s)==SDRO_OK); tests++;
  init(&f,&s,&o); *reg(&f,SDP_SD+0xfcu)=0xffffffffu;
  assert(sdp_prepare(&s,&o)==SDRO_IO_ERROR); restored(&f); tests++;
  init(&f,&s,&o); *reg(&f,SDP_PIN+0xc0u)=0xffffffffu;
  assert(sdp_prepare(&s,&o)==SDRO_IO_ERROR); assert(s.diag.stage==SDP_PINS); restored(&f); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK); *reg(&f,SDP_AON+8u)&=~B(9);
  assert(sdp_stop(&s)==SDRO_NOT_READY); assert(s.diag.cleanup_status==SDRO_NOT_READY); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK);
  assert(sdp_set_transfer_clock(&s,13000000u,&hz)==SDRO_OK&&hz==13000000u);
  assert(s.active_clock_bits==7u&&s.clock_hz==13000000u&&sdp_access_ready(&s)==1);
  assert(raw(&f,SDP_SD+0x2cu)==0xe0007u&&f.commands==0u);
  assert(sdp_stop(&s)==SDRO_OK); restored(&f); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK);
  assert(sdp_set_transfer_clock(&s,6500000u,&hz)==SDRO_OK&&hz==6500000u);
  assert(s.active_clock_bits==0x107u&&sdp_access_ready(&s)==1&&f.commands==0u); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK); n=f.writes;
  assert(sdp_set_transfer_clock(&s,26000000u,&hz)==SDRO_ARGUMENT&&f.writes==n); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK); *reg(&f,SDP_SD+0x24u)=3u; n=f.writes;
  assert(sdp_set_transfer_clock(&s,13000000u,&hz)==SDRO_TIMEOUT&&f.writes==n);
  assert(sdp_access_ready(&s)==1&&s.clock_hz==382352u); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK); f.fail_fast_clock=1u;
  assert(sdp_set_transfer_clock(&s,13000000u,&hz)==SDRO_TIMEOUT&&hz==0u);
  assert(s.diag.clock_rollback_status==SDRO_OK&&sdp_access_ready(&s)==1&&s.clock_hz==382352u&&f.commands==0u); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK); f.clock_stuck=1u;
  assert(sdp_set_transfer_clock(&s,13000000u,&hz)==SDRO_TIMEOUT&&hz==0u);
  assert(s.diag.clock_rollback_status==SDRO_TIMEOUT&&sdp_access_ready(&s)==0&&s.clock_hz==0u); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK); f.fail_fast_clock=1u; f.lose_gate_on_delay=1u;
  assert(sdp_set_transfer_clock(&s,13000000u,&hz)==SDRO_NOT_READY&&hz==0u);
  assert(s.diag.clock_rollback_status==SDRO_NOT_READY&&s.ready==0u); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK); f.fail_fast_clock=1u; f.remove_on_delay=1u;
  assert(sdp_set_transfer_clock(&s,13000000u,&hz)==SDRO_NO_CARD&&hz==0u);
  assert(s.removed==1u&&s.ready==0u&&sdp_access_ready(&s)==0); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK); s.transitioning=1u; n=f.writes;
  assert(sdp_set_transfer_clock(&s,13000000u,&hz)==SDRO_NOT_READY&&f.writes==n); tests++;
  init(&f,&s,&o); assert(sdp_prepare(&s,&o)==SDRO_OK); *reg(&f,SDP_EIC+0xdcu)=0u; n=f.writes;
  assert(sdp_set_transfer_clock(&s,13000000u,&hz)==SDRO_NOT_READY&&f.writes==n); tests++;
  printf("PASS: %u platform scenarios (simulated MMIO; no physical validation)\n",tests);
  return 0;
}
