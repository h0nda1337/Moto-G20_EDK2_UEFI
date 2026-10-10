/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Read-only SD protocol over SDHCI PIO or native Unisoc SDMA. Definitions checked with
 * Motorola kernel-sprd 261390cdfff66efef9082bd70c1af0abd21d77f2, sprd-sdhcr11.h.
 * Platform setup is intentionally separate and mandatory.
 */
#include "sdhci_ro.h"

#define REG_BLOCK_COUNT32 0x00u
#define REG_BLOCK       0x04u
#define REG_ARG         0x08u
#define REG_COMMAND     0x0cu
#define REG_RESPONSE    0x10u
#define REG_BUFFER      0x20u
#define REG_PRESENT     0x24u
#define REG_IRQ         0x30u
#define REG_IRQ_ENABLE  0x34u
#define REG_IRQ_SIGNAL  0x38u
#define REG_VERSION_PAIR 0xfcu
#define REG_HOST_CONTROL1 0x28u
#define REG_HOST_CONTROL2_PAIR 0x3cu
#define REG_DMA_LOW 0x58u
#define REG_DMA_HIGH 0x5cu
#define IRQ_COMMAND    0x00000001u
#define IRQ_TRANSFER   0x00000002u
#define IRQ_READ       0x00000020u
#define IRQ_DMA_END    0x00000008u
#define IRQ_ERRORS     0x03ff8000u
#define IRQ_USED       (IRQ_COMMAND | IRQ_TRANSFER | IRQ_READ | IRQ_ERRORS)
#define IRQ_DMA_USED   (IRQ_COMMAND | IRQ_TRANSFER | IRQ_DMA_END | IRQ_ERRORS)
#define R1_ERRORS      0xfff9a008u
#define POLL_US        100u
#define COMMAND_US     100000u
#define DATA_US        500000u
#define OCR_VOLTAGES   0x00ff8000u

static void zero_bytes(void *p, sd_u32 n) {
  sd_u8 *b = (sd_u8 *)p;
  while (n--) *b++ = 0;
}

static enum sdro_status available(struct sdro_host *h) {
  int present;
  if (!h || !h->initialized || h->faulted) return SDRO_NOT_READY;
  if (h->io.access_ready(h->io.context) != 1) return SDRO_NOT_READY;
  present = h->io.card_present(h->io.context);
  if (present < 0) return SDRO_NOT_READY;
  return present == 1 ? SDRO_OK : SDRO_NO_CARD;
}

static void snapshot(struct sdro_host *h, sd_u32 phase) {
  static const sd_u32 offsets[10] = { REG_IRQ, REG_IRQ_ENABLE, REG_PRESENT,
    REG_HOST_CONTROL1, REG_HOST_CONTROL2_PAIR, REG_COMMAND, REG_RESPONSE,
    REG_BLOCK_COUNT32, REG_DMA_LOW, REG_DMA_HIGH };
  sd_u32 *fields[10], i;
  if (!h->dma_enabled || h->last_command != 17) return;
  h->snapshot_valid = 0;
  h->snapshot_phase = phase;
  fields[0]=&h->snapshot_irq; fields[1]=&h->snapshot_irq_enable;
  fields[2]=&h->snapshot_present; fields[3]=&h->snapshot_host_control1;
  fields[4]=&h->snapshot_host_control2; fields[5]=&h->snapshot_command;
  fields[6]=&h->snapshot_response; fields[7]=&h->snapshot_count;
  fields[8]=&h->snapshot_dma_low; fields[9]=&h->snapshot_dma_high;
  for (i=0; i<10; ++i) {
    *fields[i] = 0;
    if (available(h) != SDRO_OK) continue;
    *fields[i] = h->io.read32(h->io.context, offsets[i]);
    if (i == 4) *fields[i] >>= 16;
    h->snapshot_valid |= 1u << i;
  }
}

static enum sdro_status fail(struct sdro_host *h, enum sdro_status s) {
  snapshot(h, SDRO_SNAPSHOT_FAILURE);
  h->last_status = s;
  h->card.selected = 0;
  h->faulted = 1;
  h->transfer_active = 0;
  /* No cleanup MMIO: clocks/power may already be inaccessible. A fresh session
   * requires the platform to reset the host, not retries on stale state. */
  return s;
}

static enum sdro_status wait_present(struct sdro_host *h, sd_u32 mask, sd_u32 expected, sd_u32 us) {
  sd_u32 elapsed;
  enum sdro_status s;
  for (elapsed = 0; ; elapsed += POLL_US) {
    s = available(h);
    if (s != SDRO_OK) return s;
    if ((h->io.read32(h->io.context, REG_PRESENT) & mask) == expected) return SDRO_OK;
    if (elapsed >= us) return SDRO_TIMEOUT;
    h->io.delay_us(h->io.context, POLL_US);
  }
}

static enum sdro_status wait_irq(struct sdro_host *h, sd_u32 wanted, sd_u32 us) {
  sd_u32 elapsed, irq;
  enum sdro_status s;
  for (elapsed = 0; ; elapsed += POLL_US) {
    s = available(h);
    if (s != SDRO_OK) return s;
    irq = h->io.read32(h->io.context, REG_IRQ);
    h->last_irq = irq;
    if (irq & IRQ_ERRORS) return SDRO_IO_ERROR;
    if (h->dma_enabled && h->transfer_active && (irq & IRQ_DMA_END)) return SDRO_IO_ERROR;
    /* A completed read with no buffer-ready indication is not successful data. */
    if ((wanted == IRQ_READ) && (irq & IRQ_TRANSFER) && !(irq & IRQ_READ)) return SDRO_IO_ERROR;
    if ((irq & wanted) == wanted) return SDRO_OK;
    if (elapsed >= us) return SDRO_TIMEOUT;
    h->io.delay_us(h->io.context, POLL_US);
  }
}

/* Private command allowlist prevents callers from issuing writes/erase. */
static enum sdro_status issue(struct sdro_host *h, sd_u8 cmd, sd_u32 arg, sd_u8 *long_response) {
  sd_u32 response_type, transfer = 0, mask = 1u, i, regs[4], word;
  enum sdro_status s;
  switch (cmd) {
    case 0: response_type = 0; break;
    case 2: case 9: response_type = 9; break; /* R2: CRC, no index */
    case 3: case 8: case 13: case 16: case 55: response_type = 0x1a; break;
    case 7: response_type = 0x1b; mask = 3; break; /* R1b */
    case 41: response_type = 2; break; /* R3: no CRC */
    /* Motorola passes if_mult for count-enable. CMD17 is one block: read,
     * no multi-block/count enable or auto command; DMA is explicit opt-in. */
    case 17: response_type = 0x3a; transfer = h->dma_enabled ? 0x11u : 0x10u; mask = 3; break;
    default: return SDRO_UNSUPPORTED;
  }
  s = available(h);
  if (s != SDRO_OK) return fail(h, s);
  h->last_command = cmd;
  h->last_response = 0;
  h->last_irq = 0;
  s = wait_present(h, mask, 0, COMMAND_US);
  if (s != SDRO_OK) return fail(h, s);
  h->io.write32(h->io.context, REG_IRQ_SIGNAL, 0); /* polling, never enable IRQ line */
  word = (cmd == 17 && h->dma_enabled) ? IRQ_DMA_USED : IRQ_USED;
  h->io.write32(h->io.context, REG_IRQ_ENABLE, word);
  h->io.write32(h->io.context, REG_IRQ, word); /* W1C, never read-modify-write */
  if (cmd == 17) {
    sd_u32 version, block;
    h->transfer_version = h->io.read32(h->io.context, REG_VERSION_PAIR);
    version = (h->transfer_version >> 16) & 0xffu;
    h->transfer_mode = transfer;
    if (h->transfer_version == 0xffffffffu || version == 0xffu) return fail(h, SDRO_IO_ERROR);
    if (h->dma_enabled && version < 3u) return fail(h, SDRO_UNSUPPORTED);
    /* sprd-sdhcr11.c programs 32-bit count at 0x00 for spec >= 4.00
     * (encoded >=3); 0x06 is the legacy 16-bit counter. Program it for
     * EVERY command because hardware may consume it to zero. */
    block = version >= 3u ? 0x00000200u : 0x00010200u;
    h->transfer_count_offset = version >= 3u ? REG_BLOCK_COUNT32 : 6u;
    h->io.write32(h->io.context, REG_BLOCK, block);
    if (version >= 3u) h->io.write32(h->io.context, REG_BLOCK_COUNT32, 1u);
    h->transfer_block = h->io.read32(h->io.context, REG_BLOCK);
    h->transfer_count = version >= 3u ? h->io.read32(h->io.context, REG_BLOCK_COUNT32) :
                                      h->transfer_block >> 16;
    /* Capture BEFORE the command: a consumed counter is not a failed setup.
     * Do not send CMD17 if the controller ignored size/count programming. */
    if (h->transfer_block != block || h->transfer_count != 1u) return fail(h, SDRO_IO_ERROR);
    if (h->dma_enabled) {
      /* Motorola sprd_sdmd_mode: native SDMA address is 0x58/0x5c.
       * Keep all unrelated HC1 bits, select SDMA (bits4:3=0). HC2 bit13
       * controls 64-bit addressing: this owner only supplies 32-bit pages.
       * Lower16 at0x3c is W1C auto-command error: write zero, never RMW it. */
      word = h->io.read32(h->io.context, REG_HOST_CONTROL1) & ~0x18u;
      h->io.write32(h->io.context, REG_HOST_CONTROL1, word);
      if (h->io.read32(h->io.context, REG_HOST_CONTROL1) != word) return fail(h, SDRO_IO_ERROR);
      word = h->io.read32(h->io.context, REG_HOST_CONTROL2_PAIR) & 0xdfff0000u;
      h->io.write32(h->io.context, REG_HOST_CONTROL2_PAIR, word);
      if ((h->io.read32(h->io.context, REG_HOST_CONTROL2_PAIR) & 0xffff0000u) != word)
        return fail(h, SDRO_IO_ERROR);
      h->io.write32(h->io.context, REG_DMA_HIGH, 0);
      h->io.write32(h->io.context, REG_DMA_LOW, h->dma.address);
      if (h->io.read32(h->io.context, REG_DMA_LOW) != h->dma.address ||
          h->io.read32(h->io.context, REG_DMA_HIGH) != 0) return fail(h, SDRO_IO_ERROR);
      if (h->dma.prepare(h->dma.context) != 1) return fail(h, SDRO_IO_ERROR);
      s = available(h);
      if (s != SDRO_OK) return fail(h, s);
      snapshot(h, SDRO_SNAPSHOT_SETUP);
    }
  }
  h->io.write32(h->io.context, REG_ARG, arg);
  /* Atomic transfer+command avoids replaying old commands with 32-bit RMW. */
  h->io.write32(h->io.context, REG_COMMAND, ((sd_u32)cmd << 24) | (response_type << 16) | transfer);
  h->commands++;
  s = available(h);
  if (s != SDRO_OK) return fail(h, s);
  if (cmd == 17 && h->dma_enabled &&
      h->io.read32(h->io.context, REG_COMMAND) != (((sd_u32)cmd << 24) | (response_type << 16) | transfer))
    return fail(h, SDRO_IO_ERROR);
  s = wait_irq(h, IRQ_COMMAND, COMMAND_US);
  if (s != SDRO_OK) return fail(h, s);
  if (cmd == 17) snapshot(h, SDRO_SNAPSHOT_COMMAND);
  if (cmd != 0) {
    h->last_response = h->io.read32(h->io.context, REG_RESPONSE);
    if (cmd == 2 || cmd == 9) {
      for (i = 0; i < 4; ++i) regs[i] = h->io.read32(h->io.context, REG_RESPONSE + 4 * i);
      for (i = 0; i < 4; ++i) {
        word = regs[3 - i] << 8;
        if (i != 3) word |= regs[2 - i] >> 24;
        long_response[i * 4] = (sd_u8)(word >> 24);
        long_response[i * 4 + 1] = (sd_u8)(word >> 16);
        long_response[i * 4 + 2] = (sd_u8)(word >> 8);
        long_response[i * 4 + 3] = (sd_u8)word;
      }
    } else if (cmd == 3) {
      if (h->last_response & 0xe000u) return fail(h, SDRO_CARD_ERROR); /* R6 */
    } else if ((cmd != 8) && (cmd != 41) && (h->last_response & R1_ERRORS)) {
      return fail(h, SDRO_CARD_ERROR);
    }
  }
  h->io.write32(h->io.context, REG_IRQ, IRQ_COMMAND);
  if (cmd == 7) {
    s = wait_irq(h, IRQ_TRANSFER, DATA_US);
    if (s != SDRO_OK) return fail(h, s);
    h->io.write32(h->io.context, REG_IRQ, IRQ_TRANSFER);
    s = wait_present(h, 2u, 0, DATA_US);
    if (s != SDRO_OK) return fail(h, s);
  }
  h->last_status = SDRO_OK;
  return SDRO_OK;
}

enum sdro_status sdro_init(struct sdro_host *h, const struct sdro_io *io, sd_u32 hz, sd_u32 ocr) {
  if (!h || !io || !io->read32 || !io->write32 || !io->delay_us ||
      !io->access_ready || !io->card_present || hz == 0 || hz > 400000u ||
      !(ocr & OCR_VOLTAGES) || (ocr & ~OCR_VOLTAGES)) return SDRO_ARGUMENT;
  zero_bytes(h, sizeof(*h));
  h->io = *io;
  h->clock_hz = hz;
  h->voltage_ocr = ocr;
  h->initialized = 1;
  h->last_status = SDRO_NOT_READY;
  return SDRO_OK;
}

enum sdro_status sdro_set_dma(struct sdro_host *h, const struct sdro_dma *dma) {
  sd_u32 version, caps;
  enum sdro_status s;
  if (!h || !dma || !dma->prepare || !dma->complete || !dma->address ||
      (dma->address & 511u) || (sd_u64)dma->address + 511u > 0xffffffffULL)
    return SDRO_ARGUMENT;
  if (!h->initialized || h->faulted || h->identifying || h->transfer_active ||
      !h->card.selected) return SDRO_NOT_READY;
  s = available(h);
  if (s != SDRO_OK) return s;
  version = h->io.read32(h->io.context, REG_VERSION_PAIR);
  if (version == 0xffffffffu || ((version >> 16) & 255u) == 255u) return SDRO_IO_ERROR;
  if (((version >> 16) & 255u) < 3u) return SDRO_UNSUPPORTED;
  s = available(h);
  if (s != SDRO_OK) return s;
  caps = h->io.read32(h->io.context, 0x40u);
  if (caps == 0xffffffffu) return SDRO_IO_ERROR;
  if (!(caps & 0x00400000u)) return SDRO_UNSUPPORTED;
  h->dma = *dma;
  h->dma_enabled = 1;
  return SDRO_OK;
}

static sd_u32 bits(const sd_u8 csd[16], sd_u32 msb, sd_u32 lsb) {
  sd_u32 bit, out = 0;
  for (bit = msb + 1; bit-- > lsb;) out = (out << 1) | (((sd_u32)csd[(127 - bit) / 8] >> (bit % 8)) & 1u);
  return out;
}

enum sdro_status sdro_parse_csd(const sd_u8 csd[16], int addressed, sd_u64 *blocks) {
  sd_u32 version, size, read_len, multiplier;
  sd_u64 bytes;
  if (!csd || !blocks || (addressed != 0 && addressed != 1)) return SDRO_ARGUMENT;
  *blocks = 0;
  version = bits(csd, 127, 126);
  if (version == 1 && addressed) {
    if (bits(csd, 83, 80) != 9) return SDRO_UNSUPPORTED;
    size = bits(csd, 69, 48);
    *blocks = ((sd_u64)size + 1) * 1024;
    return SDRO_OK;
  }
  if (version != 0 || addressed) return SDRO_UNSUPPORTED;
  read_len = bits(csd, 83, 80);
  if (read_len < 9 || read_len > 11) return SDRO_UNSUPPORTED;
  size = bits(csd, 73, 62);
  multiplier = bits(csd, 49, 47);
  bytes = ((sd_u64)size + 1) << (multiplier + 2 + read_len);
  if (bytes == 0 || bytes > 0x100000000ULL) return SDRO_RANGE;
  *blocks = bytes / 512;
  return SDRO_OK;
}

enum sdro_status sdro_identify(struct sdro_host *h) {
  enum sdro_status s;
  sd_u32 attempt;
  if (!h || !h->initialized) return SDRO_ARGUMENT;
  if (h->identifying || h->card.selected || h->clock_hz > 400000u) return SDRO_NOT_READY;
  s = available(h);
  if (s != SDRO_OK) return fail(h, s);
  h->identifying = 1;
  zero_bytes(&h->card, sizeof(h->card));
  s = issue(h, 0, 0, 0);
  if (s != SDRO_OK) return s;
  h->io.delay_us(h->io.context, 2000);
  s = issue(h, 8, 0x1aau, 0);
  if (s != SDRO_OK) return s; /* legacy SD is deliberately not guessed */
  if ((h->last_response & 0xfffu) != 0x1aau) return fail(h, SDRO_UNSUPPORTED);
  for (attempt = 0; attempt < 100; ++attempt) {
    s = issue(h, 55, 0, 0);
    if (s != SDRO_OK) return s;
    if (!(h->last_response & 0x20u)) return fail(h, SDRO_CARD_ERROR);
    s = issue(h, 41, h->voltage_ocr | 0x40000000u, 0); /* HCS, never S18R */
    if (s != SDRO_OK) return s;
    if (h->last_response & 0x80000000u) break;
    h->io.delay_us(h->io.context, 10000);
  }
  if (attempt == 100) return fail(h, SDRO_TIMEOUT);
  h->card.ocr = h->last_response;
  if (!(h->card.ocr & h->voltage_ocr)) return fail(h, SDRO_UNSUPPORTED);
  h->card.block_addressed = (h->card.ocr & 0x40000000u) != 0;
  s = issue(h, 2, 0, h->card.cid);
  if (s != SDRO_OK) return s;
  s = issue(h, 3, 0, 0);
  if (s != SDRO_OK) return s;
  h->card.rca = h->last_response >> 16;
  if (!h->card.rca) return fail(h, SDRO_CARD_ERROR);
  s = issue(h, 9, h->card.rca << 16, h->card.csd);
  if (s != SDRO_OK) return s;
  s = sdro_parse_csd(h->card.csd, h->card.block_addressed, &h->card.blocks);
  if (s != SDRO_OK) return fail(h, s);
  s = issue(h, 7, h->card.rca << 16, 0);
  if (s != SDRO_OK) return s;
  if (!h->card.block_addressed) {
    s = issue(h, 16, 512, 0);
    if (s != SDRO_OK) return s;
  }
  s = issue(h, 13, h->card.rca << 16, 0);
  if (s != SDRO_OK) return s;
  /* Card must report TRANSFER state and READY_FOR_DATA, not merely a response. */
  if ((h->last_response & 0x1f00u) != 0x900u) return fail(h, SDRO_CARD_ERROR);
  h->card.selected = 1;
  h->identifying = 0;
  return SDRO_OK;
}

enum sdro_status sdro_read_block(struct sdro_host *h, sd_u64 lba, sd_u8 *data, sd_u32 length) {
  enum sdro_status s;
  sd_u32 i, value, arg;
  sd_u8 staged[512];
  if (!h || !data || length != 512) return SDRO_ARGUMENT;
  if (!h->initialized || !h->card.selected || !h->clock_hz || h->clock_hz > 25000000u || h->transfer_active)
    return SDRO_NOT_READY;
  if (lba >= h->card.blocks || lba > 0xffffffffULL) return SDRO_RANGE;
  if (!h->card.block_addressed && lba > 0x7fffffULL) return SDRO_RANGE;
  arg = (sd_u32)(h->card.block_addressed ? lba : lba * 512);
  h->transfer_active = 1;
  s = issue(h, 17, arg, 0);
  if (s != SDRO_OK) return s;
  if (h->dma_enabled) {
    s = wait_irq(h, IRQ_TRANSFER, DATA_US);
    if (s != SDRO_OK) return fail(h, s);
    s = wait_present(h, 2u, 0, DATA_US);
    if (s != SDRO_OK) return fail(h, s);
    snapshot(h, SDRO_SNAPSHOT_TRANSFER);
    /* Recheck errors immediately before exposing completion to the owner. */
    s = available(h);
    if (s != SDRO_OK) return fail(h, s);
    value = h->io.read32(h->io.context, REG_IRQ);
    h->last_irq = value;
    if ((value & (IRQ_ERRORS | IRQ_DMA_END)) || !(value & IRQ_TRANSFER)) return fail(h, SDRO_IO_ERROR);
    h->io.write32(h->io.context, REG_IRQ, IRQ_TRANSFER);
    if (h->dma.complete(h->dma.context, staged) != 1) return fail(h, SDRO_IO_ERROR);
    s = available(h);
    if (s != SDRO_OK) return fail(h, s);
    for (i = 0; i < 512; ++i) data[i] = staged[i];
    h->blocks_read++;
    h->last_status = SDRO_OK;
    h->transfer_active = 0;
    return SDRO_OK;
  }
  s = wait_irq(h, IRQ_READ, DATA_US);
  if (s != SDRO_OK) return fail(h, s);
  /* PRESENT_STATE buffer-read-enable must agree with the ready interrupt. */
  s = wait_present(h, 0x800u, 0x800u, DATA_US);
  if (s != SDRO_OK) return fail(h, s);
  for (i = 0; i < 128; ++i) {
    s = available(h);
    if (s != SDRO_OK) return fail(h, s);
    value = h->io.read32(h->io.context, REG_BUFFER);
    staged[4 * i] = (sd_u8)value;
    staged[4 * i + 1] = (sd_u8)(value >> 8);
    staged[4 * i + 2] = (sd_u8)(value >> 16);
    staged[4 * i + 3] = (sd_u8)(value >> 24);
  }
  s = available(h);
  if (s != SDRO_OK) return fail(h, s);
  h->io.write32(h->io.context, REG_IRQ, IRQ_READ);
  s = wait_irq(h, IRQ_TRANSFER, DATA_US);
  if (s != SDRO_OK) return fail(h, s);
  h->io.write32(h->io.context, REG_IRQ, IRQ_TRANSFER);
  s = wait_present(h, 2u, 0, DATA_US);
  if (s != SDRO_OK) return fail(h, s);
  for (i = 0; i < 512; ++i) data[i] = staged[i];
  h->blocks_read++;
  h->last_status = SDRO_OK;
  h->transfer_active = 0;
  return SDRO_OK;
}

enum sdro_status sdro_unisoc_divider(sd_u32 base, sd_u32 target, sd_u32 *encoded, sd_u32 *actual) {
  sd_u64 div;
  if (!base || !target || target > 25000000u || !encoded || !actual) return SDRO_ARGUMENT;
  if ((sd_u64)base <= (sd_u64)target * 2) {
    div = 0;
    *actual = base / 2;
  } else {
    div = ((sd_u64)base + (sd_u64)target * 4 - 1) / ((sd_u64)target * 4);
    if (div > 0x3ffu) return SDRO_RANGE;
    *actual = base / (4 * (sd_u32)div);
  }
  *encoded = (((sd_u32)div & 0x300u) >> 2) | (((sd_u32)div & 0xffu) << 8);
  return SDRO_OK;
}
