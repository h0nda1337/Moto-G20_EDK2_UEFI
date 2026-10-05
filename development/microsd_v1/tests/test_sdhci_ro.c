/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "sdhci_ro.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake {
  sd_u32 regs[256], elapsed, reads, writes, fifo, argument;
  sd_u8 csd[16], commands[256];
  unsigned command_count, ready_attempts;
  int access, present, timeout, inhibit, error_cmd, error_data;
  int no_buffer, no_complete, card_error, bad_echo, bad_app, bad_rca;
  int bad_transfer_state, busy_forever, low_capacity, voltage_mismatch;
  int drop_at_fifo, lose_at_fifo;
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
  if (offset == 0x24) return (f->inhibit ? 3u : 0u) | ((f->fifo < 128 && !f->no_buffer) ? 0x800u : 0u);
  if (offset == 0x20) {
    sd_u32 i = f->fifo++ * 4;
    assert(f->fifo <= 128);
    if (f->drop_at_fifo && f->fifo == (unsigned)f->drop_at_fifo) f->present = 0;
    if (f->lose_at_fifo && f->fifo == (unsigned)f->lose_at_fifo) f->access = 0;
    if (f->fifo == 128 && !f->no_complete) f->regs[0x30 / 4] |= f->error_data ? 0x208000 : 2;
    return (i & 255u) | ((i + 1) & 255u) << 8 | ((i + 2) & 255u) << 16 | ((i + 3) & 255u) << 24;
  }
  return f->regs[offset / 4];
}
static void wr(void *ctx, sd_u32 offset, sd_u32 value) {
  struct fake *f = ctx;
  unsigned cmd;
  assert(f->access == 1);
  assert(offset == 4 || offset == 8 || offset == 12 || offset == 0x30 || offset == 0x34 || offset == 0x38);
  f->writes++;
  if (offset == 0x30) { f->regs[offset / 4] &= ~value; return; }
  if (offset == 0x38) assert(value == 0); /* never enable physical interrupt lines */
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
      assert((value & 0xffffu) == 0x12 && f->regs[1] == 0x10200);
      f->fifo = 0;
      if (!f->no_buffer) f->regs[0x30 / 4] |= 0x20;
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
int main(void) {
  struct fake f;
  struct sdro_host h;
  sd_u8 data[512], csd[16];
  sd_u64 blocks;
  sd_u32 encoded, actual, reads, writes;
  unsigned i;

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
