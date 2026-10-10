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
  if (offset == 0x24) return (f->inhibit ? 3u : 0u) | ((f->fifo_ready && f->fifo < 128) ? 0x800u : 0u);
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
  assert(offset == 0 || offset == 4 || offset == 8 || offset == 12 || offset == 0x30 || offset == 0x34 || offset == 0x38);
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
  f->regs[offset / 4] = value;
  if (offset != 12) return;
  cmd = value >> 24;
  assert(cmd == 0 || cmd == 2 || cmd == 3 || cmd == 7 || cmd == 8 || cmd == 9 || cmd == 13 || cmd == 16 || cmd == 17 || cmd == 41 || cmd == 55);
  assert((value & 1u) == 0); /* no DMA */
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
      assert(f->last_transfer == 0x10 || f->last_transfer == 0x12);
      f->launched_count = ((f->regs[0xfc / 4] >> 16) & 255u) >= 3u ? f->regs[0] : f->regs[1] >> 16;
      f->fifo = 0;
      f->fifo_ready = 0;
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
