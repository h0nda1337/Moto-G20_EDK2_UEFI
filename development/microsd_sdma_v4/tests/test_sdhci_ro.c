/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "sdhci_ro.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake {
  sd_u32 regs[256], elapsed, reads, writes, fifo, argument;
  sd_u32 last_transfer, launched_count, count_reads, size_reads;
  sd_u32 count_writes, size_writes;
  sd_u8 csd[16], commands[256];
  unsigned command_count, ready_attempts;
  int access, present, timeout, inhibit, error_cmd, error_data;
  int no_buffer, no_complete, card_error, bad_echo, bad_app, bad_rca;
  int bad_transfer_state, busy_forever, low_capacity, voltage_mismatch;
  int drop_at_fifo, lose_at_fifo;
  int ignore_count, ignore_size, premature_complete, fifo_ready;
  sd_u8 dma_buffer[512];
  unsigned prepared, completed;
  int prepare_error, complete_error, dma_boundary, drop_dma, lose_dma;
  int ignore_dma_address, ignore_dma_high, ignore_dma_mode, ignore_dma_64;
  int ignore_dma_enable, dma_inhibit, reenter, last_dma;
  struct sdro_host *host;
};

static void set_bits(sd_u8 *p, unsigned msb, unsigned lsb, sd_u32 value) {
  unsigned bit;
  for (bit = lsb; bit <= msb; ++bit) {
    unsigned index = (127 - bit) / 8, mask = 1u << (bit % 8);
    p[index] = (sd_u8)((p[index] & ~mask) | (((value >> (bit - lsb)) & 1u) ? mask : 0));
  }
}
static void set_csd(struct fake *f, int low) {
  memset(f->csd, 0, sizeof(f->csd));
  set_bits(f->csd, 127, 126, low ? 0 : 1);
  set_bits(f->csd, 83, 80, 9);
  if (low) {
    set_bits(f->csd, 73, 62, 4095);
    set_bits(f->csd, 49, 47, 7); /* 1 GiB, 2^21 sectors */
  } else set_bits(f->csd, 69, 48, 32767); /* 16 GiB */
}
static void long_response(struct fake *f, const sd_u8 *p) {
  unsigned i;
  for (i = 0; i < 4; ++i) {
    unsigned j = 11 - 4 * i;
    if (i == 3) f->regs[(0x10 + 4 * i) / 4] = (sd_u32)p[0] << 16 | (sd_u32)p[1] << 8 | p[2];
    else f->regs[(0x10 + 4 * i) / 4] = (sd_u32)p[j] << 24 | (sd_u32)p[j + 1] << 16 | (sd_u32)p[j + 2] << 8 | p[j + 3];
  }
}
static sd_u32 rd(void *ctx, sd_u32 offset) {
  struct fake *f = ctx;
  assert(!(offset & 3) && offset < sizeof(f->regs));
  assert(f->access == 1);
  f->reads++;
  if (offset == 0) f->count_reads++;
  if (offset == 4) f->size_reads++;
  if (offset == 0x24) return ((f->inhibit || (f->last_dma && f->dma_inhibit)) ? 3u : 0u) | ((f->fifo_ready && f->fifo < 128) ? 0x800u : 0u);
  if (offset == 0x20) {
    sd_u32 i = f->fifo++ * 4;
    assert(f->fifo_ready);
    assert(f->fifo <= 128);
    if (f->drop_at_fifo && f->fifo == (unsigned)f->drop_at_fifo) f->present = 0;
    if (f->lose_at_fifo && f->fifo == (unsigned)f->lose_at_fifo) f->access = 0;
    if (f->fifo == 128) {
      f->fifo_ready = 0;
      if (((f->regs[0xfc / 4] >> 16) & 255u) >= 3u) f->regs[0] = 0;
      else f->regs[1] &= 0xffffu;
      if (!f->no_complete) f->regs[0x30 / 4] |= f->error_data ? 0x208000 : 2;
    }
    return (i & 255u) | ((i + 1) & 255u) << 8 | ((i + 2) & 255u) << 16 | ((i + 3) & 255u) << 24;
  }
  return f->regs[offset / 4];
}
static void wr(void *ctx, sd_u32 offset, sd_u32 value) {
  struct fake *f = ctx;
  unsigned cmd;
  assert(f->access == 1);
  assert(offset == 0 || offset == 4 || offset == 8 || offset == 12 || offset == 0x30 || offset == 0x34 || offset == 0x38 ||
         offset == 0x28 || offset == 0x3c || offset == 0x58 || offset == 0x5c);
  f->writes++;
  if (offset == 0x30) { f->regs[offset / 4] &= ~value; return; }
  if (offset == 0x38) assert(value == 0); /* never enable physical interrupt lines */
  if (offset == 0) {
    f->count_writes++;
    if (f->ignore_count) return;
  }
  if (offset == 4) {
    f->size_writes++;
    if (f->ignore_size) value = (value & 0xffff0000u) | (f->regs[1] & 0xffffu);
    if (f->ignore_count && ((f->regs[0xfc / 4] >> 16) & 255u) < 3u)
      value = (value & 0xffffu) | (f->regs[1] & 0xffff0000u);
  }
  if ((offset == 0x58 && f->ignore_dma_address) || (offset == 0x5c && f->ignore_dma_high) ||
      (offset == 0x28 && f->ignore_dma_mode) || (offset == 0x3c && f->ignore_dma_64)) return;
  if (offset == 0x3c) {
    assert(!(value & 0xffffu)); /* Never echo the W1C AutoCMD error half. */
    value |= f->regs[offset / 4] & 0xffffu;
  }
  f->regs[offset / 4] = value;
  if (offset != 12) return;
  cmd = value >> 24;
  assert(cmd == 0 || cmd == 2 || cmd == 3 || cmd == 7 || cmd == 8 || cmd == 9 || cmd == 13 || cmd == 16 || cmd == 17 || cmd == 41 || cmd == 55);
  assert(cmd == 17 || (value & 1u) == 0); /* DMA only for read-only CMD17 */
  assert(f->command_count < sizeof(f->commands));
  f->commands[f->command_count++] = (sd_u8)cmd;
  f->argument = f->regs[2];
  f->regs[0x30 / 4] = f->timeout ? 0 : f->error_cmd ? 0x28000 : 1;
  f->regs[0x10 / 4] = 0x900;
  switch (cmd) {
    case 8: assert(f->argument == 0x1aa); f->regs[4] = f->bad_echo ? 0 : 0x1aa; break;
    case 55: f->regs[4] = f->bad_app ? 0 : 0x20; break;
    case 41:
      assert(!(f->argument & 0x01000000)); /* no 1.8V request */
      f->regs[4] = (f->voltage_mismatch ? 0x00800000u : 0x00040000u) | (f->low_capacity ? 0 : 0x40000000u);
      if (++f->ready_attempts >= 2 && !f->busy_forever) f->regs[4] |= 0x80000000u;
      break;
    case 2: long_response(f, f->csd); break;
    case 3: f->regs[4] = f->bad_rca ? 0 : 0x12340700; break;
    case 9: assert(f->argument == 0x12340000); long_response(f, f->csd); break;
    case 7: assert(f->argument == 0x12340000); f->regs[0x30 / 4] |= 2; break;
    case 13: if (f->bad_transfer_state) f->regs[4] = 0x700; break;
    case 16: assert(f->low_capacity && f->argument == 512); break;
    case 17:
      /* Both modes are representable so the old implementation fails for
       * its missing v4 count, not an assertion duplicating the new code. */
      f->last_transfer = value & 0xffffu;
      assert(f->last_transfer == 0x10 || f->last_transfer == 0x12 || f->last_transfer == 0x11);
      f->launched_count = ((f->regs[0xfc / 4] >> 16) & 255u) >= 3u ? f->regs[0] : f->regs[1] >> 16;
      f->fifo = 0;
      f->fifo_ready = 0;
      f->last_dma = (value & 1u) != 0;
      if (f->last_dma) {
        unsigned i;
        assert(f->prepared > f->completed);
        assert(f->launched_count == 1 && (f->regs[1] & 0xfffu) == 512);
        assert(!(f->regs[0x28 / 4] & 0x18u));
        assert(!(f->regs[0x3c / 4] & 0x20000000u));
        assert(f->regs[0x58 / 4] == 0x85000200u && f->regs[0x5c / 4] == 0);
        assert(value == 0x113a0011u);
        for (i=0; i<512; ++i) f->dma_buffer[i]=(sd_u8)(i + f->argument + (f->argument >> 8));
        f->regs[0x58 / 4] += 512u; /* completed DMA may advance its cursor */
        f->regs[0] = 0;
        if (f->error_data) f->regs[0x30 / 4] |= 0x208000u;
        if (!f->no_complete) f->regs[0x30 / 4] |= 2u;
        if (f->dma_boundary) f->regs[0x30 / 4] |= 8u;
        if (f->ignore_dma_enable) f->regs[0x0c / 4] &= ~1u;
        if (f->card_error) f->regs[4] |= 0x80000000u;
        if (f->drop_dma) f->present=0;
        if (f->lose_dma) f->access=0;
        break;
      }
      if (f->launched_count != 1 || (f->regs[1] & 0xfffu) != 512 || f->premature_complete) {
        /* Behavioral fault model: transfer completes without usable FIFO
         * data. This models the photographed IRQ2, not a hardware proof. */
        f->regs[0x30 / 4] |= 2;
      } else if (!f->no_buffer) {
        f->fifo_ready = 1;
        f->regs[0x30 / 4] |= 0x20;
      }
      if (f->card_error) f->regs[4] |= 0x80000000u;
      break;
  }
}
static void delay(void *ctx, sd_u32 us) { ((struct fake *)ctx)->elapsed += us; }
static int ready(void *ctx) { return ((struct fake *)ctx)->access; }
static int present(void *ctx) { return ((struct fake *)ctx)->present; }
static void fresh(struct fake *f, struct sdro_host *h) {
  struct sdro_io io = { f, rd, wr, delay, ready, present };
  memset(f, 0, sizeof(*f));
  f->access = f->present = 1;
  f->host=h;
  f->regs[0xfc / 4] = 0x00020000; /* SDHCI 3.00 baseline uses legacy count. */
  set_csd(f, 0);
  assert(sdro_init(h, &io, 399590, 0x40000) == SDRO_OK);
}
static void selected(struct fake *f, struct sdro_host *h) {
  fresh(f, h);
  assert(sdro_identify(h) == SDRO_OK);
  assert(h->card.selected && h->card.rca == 0x1234 && h->card.blocks == 33554432);
  assert(memcmp(h->card.csd, f->csd, 15) == 0);
}
static void unchanged(const sd_u8 *p) {
  unsigned i;
  for (i = 0; i < 512; ++i) assert(p[i] == 0xa5);
}
static unsigned tests;
#define DONE() (++tests)
static int dma_prepare(void *context);
static int dma_complete(void *context, sd_u8 *data);
static int dma_prepare(void *context) {
  struct fake *f=context;
  sd_u8 untouched[512];
  struct sdro_dma dma={ f,0x85000200u,dma_prepare,dma_complete };
  assert(f->host->transfer_active);
  ++f->prepared;
  memset(f->dma_buffer,0xce,sizeof(f->dma_buffer));
  if (f->reenter) {
    memset(untouched,0xa5,sizeof(untouched));
    assert(sdro_set_dma(f->host,&dma)==SDRO_NOT_READY);
    assert(sdro_read_block(f->host,8,untouched,sizeof(untouched))==SDRO_NOT_READY);
    unchanged(untouched);
  }
  return f->prepare_error ? 0 : 1;
}
static int dma_complete(void *context, sd_u8 *data) {
  struct fake *f=context;
  assert(f->host->transfer_active && f->prepared == f->completed+1);
  ++f->completed;
  if (f->complete_error) { memset(data,0xee,37); return 0; }
  memcpy(data,f->dma_buffer,512);
  return 1;
}
static void selected_dma(struct fake *f, struct sdro_host *h) {
  struct sdro_dma dma={ f,0x85000200u,dma_prepare,dma_complete };
  selected(f,h);
  f->regs[0xfc/4]=0x00040000;
  f->regs[0x40/4]=0x1e5d0080;
  f->regs[0x28/4]=0x1234561fu;
  f->regs[0x3c/4]=0xa3691234u;
  assert(sdro_set_dma(h,&dma)==SDRO_OK);
}
static void dma_regressions(void) {
  struct fake f;
  struct sdro_host h;
  struct sdro_dma dma={ &f,0x85000200u,dma_prepare,dma_complete };
  sd_u8 data[512];
  sd_u32 old_reads, old_writes;
  unsigned i, mode, before;
  const sd_u32 lbas[3]={0,2048,99072};
  enum sdro_status expected;

  selected_dma(&f,&h); f.reenter=1;
  for (mode=0;mode<3;++mode) {
    memset(data,0xa5,sizeof(data));
    assert(sdro_read_block(&h,lbas[mode],data,sizeof(data))==SDRO_OK);
    for (i=0;i<512;++i) assert(data[i]==(sd_u8)(i+lbas[mode]+(lbas[mode]>>8)));
    assert(f.fifo==0 && !f.fifo_ready && f.last_transfer==0x11);
    assert(f.prepared==mode+1 && f.completed==mode+1 && h.blocks_read==mode+1);
    assert(!h.transfer_active && !h.faulted);
    assert(f.regs[0x28/4]==0x12345607u && f.regs[0x3c/4]==0x83691234u);
    assert(h.snapshot_phase==SDRO_SNAPSHOT_TRANSFER && h.snapshot_valid==SDRO_SNAPSHOT_ALL);
    assert((h.snapshot_irq&2u) && h.snapshot_command==0x113a0011u);
    assert(h.snapshot_dma_low==0x85000400u && h.snapshot_dma_high==0);
    assert(h.snapshot_host_control2==0x8369 && h.snapshot_count==0);
    assert(!(h.snapshot_irq_enable&0x20u) && (h.snapshot_irq_enable&8u));
    DONE();
  }
  for (mode=0;mode<18;++mode) {
    selected_dma(&f,&h); memset(data,0xa5,sizeof(data));
    expected=SDRO_IO_ERROR;
    switch(mode) {
      case 0: f.no_complete=1; expected=SDRO_TIMEOUT; break;
      case 1: f.error_cmd=1; break;
      case 2: f.error_data=1; break;
      case 3: f.drop_dma=1; expected=SDRO_NO_CARD; break;
      case 4: f.lose_dma=1; expected=SDRO_NOT_READY; break;
      case 5: f.ignore_dma_address=1; break;
      case 6: f.regs[0x5c/4]=1; f.ignore_dma_high=1; break;
      case 7: f.ignore_dma_enable=1; break;
      case 8: f.ignore_count=1; break;
      case 9: f.ignore_size=1; break;
      case 10: f.prepare_error=1; break;
      case 11: f.complete_error=1; break;
      case 12: f.dma_boundary=1; break;
      case 13: f.ignore_dma_mode=1; break;
      case 14: f.ignore_dma_64=1; break;
      case 15: f.dma_inhibit=1; expected=SDRO_TIMEOUT; break;
      case 16: f.timeout=1; expected=SDRO_TIMEOUT; break;
      default: f.card_error=1; expected=SDRO_CARD_ERROR; break;
    }
    assert(sdro_read_block(&h,0,data,sizeof(data))==expected);
    assert(h.faulted && !h.transfer_active && !h.card.selected && h.blocks_read==0 && f.fifo==0);
    assert(f.completed==(mode==11 ? 1u : 0u));
    assert(h.snapshot_phase==SDRO_SNAPSHOT_FAILURE);
    if (mode==3 || mode==4) assert(h.snapshot_valid==0);
    else assert(h.snapshot_valid==SDRO_SNAPSHOT_ALL);
    unchanged(data);
    old_reads=f.reads; old_writes=f.writes;
    assert(sdro_set_dma(&h,&dma)==SDRO_NOT_READY);
    assert(sdro_read_block(&h,1,data,sizeof(data))==SDRO_NOT_READY);
    assert(f.reads==old_reads && f.writes==old_writes);
    unchanged(data); DONE();
  }
  selected(&f,&h); /* Reject legacy layout without mutating configuration. */
  before=f.command_count;
  assert(sdro_set_dma(&h,&dma)==SDRO_UNSUPPORTED && !h.dma_enabled && !h.faulted);
  assert(f.command_count==before); DONE();
  for (mode=0;mode<4;++mode) {
    selected(&f,&h); f.regs[0xfc/4]=0x00040000;
    f.regs[0x40/4]=mode==0 ? 0 : mode==1 ? 0xffffffffu : 0x00400000u;
    if (mode==2) f.regs[0xfc/4]=0xffffffffu;
    if (mode==3) f.regs[0xfc/4]=0x00ff0000u;
    assert(sdro_set_dma(&h,&dma)==(mode==0 ? SDRO_UNSUPPORTED : SDRO_IO_ERROR));
    assert(!h.dma_enabled && !h.faulted); DONE();
  }
  for (mode=0;mode<8;++mode) {
    selected_dma(&f,&h);
    dma.context=&f; dma.address=0x85000200u; dma.prepare=dma_prepare; dma.complete=dma_complete;
    expected=SDRO_ARGUMENT;
    switch(mode) {
      case 0: dma.address=0; break;
      case 1: dma.address=0xfffffff0u; break;
      case 2: dma.address=0x85000201u; break;
      case 3: dma.prepare=0; break;
      case 4: dma.complete=0; break;
      case 5: h.initialized=0; expected=SDRO_NOT_READY; break;
      case 6: h.transfer_active=1; expected=SDRO_NOT_READY; break;
      default: h.card.selected=0; expected=SDRO_NOT_READY; break;
    }
    old_reads=f.reads; old_writes=f.writes;
    assert(sdro_set_dma(&h,&dma)==expected);
    assert(f.reads==old_reads && f.writes==old_writes);
    assert(h.dma.address==0x85000200u && h.dma.prepare==dma_prepare && h.dma.complete==dma_complete);
    DONE();
  }
  dma.prepare=dma_prepare; dma.complete=dma_complete; dma.address=0xfffffe00u;
  selected(&f,&h); f.regs[0xfc/4]=0x00030000u; f.regs[0x40/4]=0x00400000u;
  assert(sdro_set_dma(&h,&dma)==SDRO_OK && h.dma.address==0xfffffe00u); DONE();
  printf("PASS: native SDMA completed/corrupted/timeout/removal/configuration scenarios.\n");
}
static void counter_regressions(void) {
  struct fake f;
  struct sdro_host h;
  sd_u8 data[512];
  enum sdro_status status;
  unsigned before, spec, i;
  sd_u32 count_writes, size_writes;

  selected(&f, &h); f.regs[0xfc / 4] = 0x00040000;
  memset(data, 0xa5, sizeof(data));
  status = sdro_read_block(&h, 0, data, sizeof(data));
  if (status != SDRO_OK) {
    fprintf(stderr, "REGRESSION v4.10: status=%u irq=%08x effective-count=%u fifo=%u blocks=%u\n",
            (unsigned)status, h.last_irq, f.launched_count, f.fifo, h.blocks_read);
    unchanged(data);
  }
  assert(status == SDRO_OK && h.blocks_read == 1 && f.fifo == 128);
  for (i=0;i<512;i++) assert(data[i] == (sd_u8)i);
  assert(f.launched_count == 1 && f.last_transfer == 0x10);
  assert(f.regs[1] == 512 && f.regs[0] == 0);
  assert(f.count_reads > 0 && f.size_reads > 0);
  DONE();

  /* The controller consumes its count. A second read must program it again. */
  count_writes = f.count_writes; size_writes = f.size_writes;
  memset(data, 0xa5, sizeof(data));
  assert(sdro_read_block(&h, 99072, data, sizeof(data)) == SDRO_OK);
  assert(f.count_writes == count_writes + 1 && f.size_writes == size_writes + 1);
  assert(f.launched_count == 1 && f.regs[0] == 0 && h.blocks_read == 2 && f.argument == 99072);
  for (i=0;i<512;i++) assert(data[i] == (sd_u8)i);
  DONE();

  for (spec=0;spec<=4;spec++) {
    selected(&f, &h); f.regs[0xfc / 4] = ((sd_u32)spec << 16) | 0x3456u;
    f.regs[0] = 0x12345678; f.regs[1] = 0x98760abcu;
    assert(sdro_read_block(&h, 2048, data, sizeof(data)) == SDRO_OK);
    assert(f.launched_count == 1 && f.last_transfer == 0x10);
    if (spec >= 3) {
      assert(f.count_writes == 1 && f.regs[0] == 0 && f.regs[1] == 512);
    } else {
      assert(f.count_writes == 0 && f.regs[0] == 0x12345678 && f.regs[1] == 512);
    }
    DONE();
  }
  selected(&f, &h); f.regs[0xfc / 4] = 0xffffffffu;
  before=f.command_count; memset(data,0xa5,sizeof(data));
  assert(sdro_read_block(&h,0,data,sizeof(data)) != SDRO_OK);
  assert(f.command_count == before && f.fifo == 0 && h.blocks_read == 0 && h.faulted);
  unchanged(data); DONE();
  selected(&f, &h); f.regs[0xfc / 4] = 0x00ff5678;
  before=f.command_count; memset(data,0xa5,sizeof(data));
  assert(sdro_read_block(&h,0,data,sizeof(data)) != SDRO_OK);
  assert(f.command_count == before && f.fifo == 0 && h.blocks_read == 0 && h.faulted);
  unchanged(data); DONE();

  for (spec=2;spec<=4;spec++) {
    selected(&f,&h); f.regs[0xfc / 4] = (sd_u32)spec << 16;
    f.ignore_count = 1; before=f.command_count; memset(data,0xa5,sizeof(data));
    assert(sdro_read_block(&h,0,data,sizeof(data)) == SDRO_IO_ERROR);
    assert(f.command_count == before && f.fifo == 0 && h.blocks_read == 0 && h.faulted);
    unchanged(data); DONE();
    selected(&f,&h); f.regs[0xfc / 4] = (sd_u32)spec << 16;
    f.ignore_size = 1; before=f.command_count; memset(data,0xa5,sizeof(data));
    assert(sdro_read_block(&h,0,data,sizeof(data)) == SDRO_IO_ERROR);
    assert(f.command_count == before && f.fifo == 0 && h.blocks_read == 0 && h.faulted);
    unchanged(data); DONE();
  }
  selected(&f,&h); f.regs[0xfc / 4] = 0x00040000; f.premature_complete = 1;
  before=f.command_count; memset(data,0xa5,sizeof(data));
  assert(sdro_read_block(&h,0,data,sizeof(data)) == SDRO_IO_ERROR);
  assert(f.command_count == before+1 && h.last_irq == 2 && f.fifo == 0 && h.blocks_read == 0 && h.faulted);
  unchanged(data); DONE();
  printf("PASS: %u version/count and premature-completion regressions.\n",tests);
}
int main(int argc, char **argv) {
  struct fake f;
  struct sdro_host h;
  sd_u8 data[512], csd[16];
  sd_u64 blocks;
  sd_u32 encoded, actual, reads, writes;
  unsigned i;

  counter_regressions();
  if (argc == 2 && strcmp(argv[1], "--counter-regressions") == 0) return 0;
  assert(argc == 1);
  dma_regressions();

  assert(sdro_init(0, 0, 0, 0) == SDRO_ARGUMENT); DONE();
  fresh(&f, &h); f.access = 0;
  assert(sdro_identify(&h) == SDRO_NOT_READY && !f.reads && !f.writes); DONE();
  fresh(&f, &h); f.present = 0;
  assert(sdro_identify(&h) == SDRO_NO_CARD && !f.reads && !f.writes); DONE();
  fresh(&f, &h); f.present = -1;
  assert(sdro_identify(&h) == SDRO_NOT_READY && !f.reads && !f.writes); DONE();
  fresh(&f, &h); f.inhibit = 1;
  assert(sdro_identify(&h) == SDRO_TIMEOUT && f.elapsed == 100000 && !f.writes); DONE();
  fresh(&f, &h); f.timeout = 1;
  assert(sdro_identify(&h) == SDRO_TIMEOUT && f.elapsed == 100000 && f.command_count == 1); DONE();
  fresh(&f, &h); f.error_cmd = 1;
  assert(sdro_identify(&h) == SDRO_IO_ERROR && h.last_irq == 0x28000); DONE();
  fresh(&f, &h); f.bad_echo = 1;
  assert(sdro_identify(&h) == SDRO_UNSUPPORTED); DONE();
  fresh(&f, &h); f.bad_app = 1;
  assert(sdro_identify(&h) == SDRO_CARD_ERROR && f.command_count == 3); DONE();
  fresh(&f, &h); f.bad_rca = 1;
  assert(sdro_identify(&h) == SDRO_CARD_ERROR); DONE();
  fresh(&f, &h); f.voltage_mismatch = 1;
  assert(sdro_identify(&h) == SDRO_UNSUPPORTED); DONE();
  fresh(&f, &h); f.busy_forever = 1;
  assert(sdro_identify(&h) == SDRO_TIMEOUT && f.ready_attempts == 100 && f.elapsed == 1002000); DONE();
  fresh(&f, &h); f.bad_transfer_state = 1;
  assert(sdro_identify(&h) == SDRO_CARD_ERROR && !h.card.selected); DONE();
  selected(&f, &h); DONE();
  assert(sdro_identify(&h) == SDRO_NOT_READY); DONE();
  memset(data, 0xa5, sizeof(data));
  assert(sdro_read_block(&h, 23, data, 512) == SDRO_OK && f.argument == 23 && h.blocks_read == 1);
  for (i = 0; i < 512; ++i) assert(data[i] == (sd_u8)i);
  DONE();
  reads = f.reads; writes = f.writes;
  assert(sdro_read_block(&h, h.card.blocks, data, 512) == SDRO_RANGE && reads == f.reads && writes == f.writes); DONE();
  assert(sdro_read_block(&h, 0, data, 511) == SDRO_ARGUMENT && reads == f.reads && writes == f.writes); DONE();
  fresh(&f, &h); f.low_capacity = 1; set_csd(&f, 1);
  assert(sdro_identify(&h) == SDRO_OK && !h.card.block_addressed && h.card.blocks == 2097152);
  assert(sdro_read_block(&h, 2, data, 512) == SDRO_OK && f.argument == 1024); DONE();
  selected(&f, &h); f.card_error = 1; memset(data, 0xa5, sizeof(data));
  assert(sdro_read_block(&h, 0, data, 512) == SDRO_CARD_ERROR); unchanged(data); DONE();
  selected(&f, &h); f.no_buffer = 1;
  assert(sdro_read_block(&h, 0, data, 512) == SDRO_TIMEOUT); unchanged(data); DONE();
  selected(&f, &h); f.no_complete = 1;
  assert(sdro_read_block(&h, 0, data, 512) == SDRO_TIMEOUT); unchanged(data); DONE();
  selected(&f, &h); f.error_data = 1;
  assert(sdro_read_block(&h, 0, data, 512) == SDRO_IO_ERROR); unchanged(data); DONE();
  selected(&f, &h); f.drop_at_fifo = 64;
  assert(sdro_read_block(&h, 0, data, 512) == SDRO_NO_CARD && f.fifo == 64); unchanged(data); DONE();
  selected(&f, &h); f.lose_at_fifo = 64;
  assert(sdro_read_block(&h, 0, data, 512) == SDRO_NOT_READY && f.fifo == 64); unchanged(data); DONE();
  reads = f.reads; writes = f.writes;
  assert(sdro_read_block(&h, 0, data, 512) == SDRO_NOT_READY && reads == f.reads && writes == f.writes); DONE();
  memset(csd, 0, 16); set_bits(csd, 127, 126, 1); set_bits(csd, 83, 80, 9); set_bits(csd, 69, 48, 0x3fffff);
  assert(sdro_parse_csd(csd, 1, &blocks) == SDRO_OK && blocks == 0x100000000ULL); DONE();
  assert(sdro_parse_csd(csd, 0, &blocks) == SDRO_UNSUPPORTED && blocks == 0); DONE();
  set_bits(csd, 127, 126, 2);
  assert(sdro_parse_csd(csd, 1, &blocks) == SDRO_UNSUPPORTED); DONE();
  assert(sdro_unisoc_divider(390000000, 400000, &encoded, &actual) == SDRO_OK && encoded == 0xf400 && actual == 399590); DONE();
  assert(sdro_unisoc_divider(390000000, 25000000, &encoded, &actual) == SDRO_OK && encoded == 0x400 && actual == 24375000); DONE();
  assert(sdro_unisoc_divider(390000000, 100, &encoded, &actual) == SDRO_RANGE); DONE();
  assert(sdro_unisoc_divider(0, 400000, &encoded, &actual) == SDRO_ARGUMENT); DONE();
  /* Real card observed through Android, mmc1:1234 SA32G. Check the parser
   * against independent kernel capacity (60456960 sectors), not our encoder. */
  {
    const sd_u8 android_csd[16] = {0x40,0x0e,0x00,0x32,0x5b,0x59,0x00,0x00,
                                   0xe6,0x9f,0x7f,0x80,0x0a,0x40,0x00,0x00};
    assert(sdro_parse_csd(android_csd, 1, &blocks) == SDRO_OK && blocks == 60456960ULL);
    DONE();
  }
  assert(sdro_unisoc_divider(26000000, 400000, &encoded, &actual) == SDRO_OK && encoded == 0x1100 && actual == 382352); DONE();
  printf("PASS: %u host scenarios. No physical device accessed.\n", tests);
  return 0;
}
