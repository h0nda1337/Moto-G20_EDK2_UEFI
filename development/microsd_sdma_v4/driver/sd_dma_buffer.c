/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "sd_dma_buffer.h"

static int
fault(struct sd_dma_buffer *b)
{
  if (b != 0) {
    b->faulted = 1;
    b->prepared = 0;
  }
  return 0;
}

static int
configuration_valid(const struct sd_dma_buffer *b)
{
  return b != 0 && b->page != 0 &&
    (((sd_u64)b->page & (SD_DMA_PAGE_SIZE - 1U)) == 0) &&
    b->physical_base != 0 && b->physical_base <= 0xfffff000ULL &&
    ((b->physical_base & (SD_DMA_PAGE_SIZE - 1U)) == 0) &&
    b->clean_invalidate != 0 && b->invalidate != 0 && b->barrier != 0;
}

/* Mix all four generation bytes across each group of four bytes. Successive
 * transfers have distinct full payloads, including at generation 256.
 */
static sd_u8
poison_byte(sd_u32 offset, sd_u32 generation)
{
  sd_u32 region = (offset >= SD_DMA_DATA_OFFSET &&
                   offset < SD_DMA_DATA_OFFSET + SD_DMA_DATA_SIZE) ?
                   0x5aU : 0xa5U;
  return (sd_u8)(region ^ (offset * 17U + (offset >> 8)) ^
                (generation >> ((offset & 3U) * 8U)));
}

static int
check_guards(struct sd_dma_buffer *b)
{
  sd_u32 i;
  for (i = 0; i < SD_DMA_PAGE_SIZE; ++i) {
    if (i >= SD_DMA_DATA_OFFSET &&
        i < SD_DMA_DATA_OFFSET + SD_DMA_DATA_SIZE) {
      continue;
    }
    if (b->page[i] != poison_byte(i, b->attempts)) {
      b->guards_ok = 0;
      return 0;
    }
  }
  b->guards_ok = 1;
  return 1;
}

int
sd_dma_buffer_init(struct sd_dma_buffer *b, sd_u8 *page,
                   sd_u64 physical_base, void *cache_context,
                   sd_dma_cache_fn clean_invalidate,
                   sd_dma_cache_fn invalidate,
                   sd_dma_barrier_fn barrier)
{
  if (b == 0) {
    return 0;
  }
  b->page = page;
  b->physical_base = physical_base;
  b->cache_context = cache_context;
  b->clean_invalidate = clean_invalidate;
  b->invalidate = invalidate;
  b->barrier = barrier;
  b->attempts = 0;
  b->completions = 0;
  b->initialized = 0;
  b->prepared = 0;
  b->faulted = 0;
  b->guards_ok = 0;
  if (!configuration_valid(b)) {
    return fault(b);
  }
  b->initialized = 1;
  return 1;
}

int
sd_dma_buffer_prepare(void *context)
{
  struct sd_dma_buffer *b = (struct sd_dma_buffer *)context;
  sd_u32 i;

  if (!configuration_valid(b) || !b->initialized || b->faulted ||
      b->prepared || b->attempts == 0xffffffffU) {
    return fault(b);
  }
  ++b->attempts;
  for (i = 0; i < SD_DMA_PAGE_SIZE; ++i) {
    b->page[i] = poison_byte(i, b->attempts);
  }
  b->guards_ok = 1;
  b->clean_invalidate(b->cache_context, b->page, SD_DMA_PAGE_SIZE);
  b->barrier(b->cache_context);
  b->prepared = 1;
  return 1;
}

int
sd_dma_buffer_complete(void *context, sd_u8 *out512)
{
  struct sd_dma_buffer *b = (struct sd_dma_buffer *)context;
  sd_u64 out_address = (sd_u64)out512;
  sd_u64 page_address;
  sd_u32 i;

  if (!configuration_valid(b) || !b->initialized || b->faulted ||
      !b->prepared || out512 == 0 ||
      out_address > (~(sd_u64)0) - (SD_DMA_DATA_SIZE - 1U)) {
    return fault(b);
  }
  page_address = (sd_u64)b->page;
  /* Do not let the copy itself overwrite guards or alias its DMA source. */
  if (out_address <= page_address + SD_DMA_PAGE_SIZE - 1U &&
      out_address + SD_DMA_DATA_SIZE - 1U >= page_address) {
    return fault(b);
  }

  b->barrier(b->cache_context);
  b->invalidate(b->cache_context, b->page, SD_DMA_PAGE_SIZE);
  b->barrier(b->cache_context);
  if (!check_guards(b)) {
    return fault(b);
  }
  for (i = 0; i < SD_DMA_DATA_SIZE; ++i) {
    out512[i] = b->page[SD_DMA_DATA_OFFSET + i];
  }
  b->guards_ok = 1;
  b->prepared = 0;
  ++b->completions;
  return 1;
}

int
sd_dma_buffer_abort(void *context)
{
  struct sd_dma_buffer *b = (struct sd_dma_buffer *)context;
  int guards_ok;

  if (!configuration_valid(b) || !b->initialized || b->faulted ||
      !b->prepared) {
    return fault(b);
  }
  b->barrier(b->cache_context);
  b->invalidate(b->cache_context, b->page, SD_DMA_PAGE_SIZE);
  b->barrier(b->cache_context);
  guards_ok = check_guards(b);
  (void)fault(b);
  return guards_ok;
}
