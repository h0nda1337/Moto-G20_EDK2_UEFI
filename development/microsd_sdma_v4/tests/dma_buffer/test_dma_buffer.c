/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sd_dma_buffer.h"

struct fixture {
  struct sd_dma_buffer buffer;
  sd_u8 *allocation;
  sd_u8 *page;
  char operations[32];
  unsigned int operation_count;
};

static unsigned int cases;

static void operation(struct fixture *f, char code)
{
  assert(f->operation_count + 1U < sizeof(f->operations));
  f->operations[f->operation_count++] = code;
  f->operations[f->operation_count] = 0;
}

static void clean_invalidate(void *context, void *address, sd_u32 length)
{
  struct fixture *f = context;
  assert(address == f->page);
  assert(((uintptr_t)address & 4095U) == 0);
  assert(length == 4096U);
  operation(f, 'C');
}

static void invalidate(void *context, void *address, sd_u32 length)
{
  struct fixture *f = context;
  assert(address == f->page);
  assert(((uintptr_t)address & 4095U) == 0);
  assert(length == 4096U);
  operation(f, 'I');
}

static void barrier(void *context)
{
  operation(context, 'B');
}

static void reset_log(struct fixture *f)
{
  f->operation_count = 0;
  f->operations[0] = 0;
}

static void initialize(struct fixture *f)
{
  reset_log(f);
  assert(sd_dma_buffer_init(&f->buffer, f->page, 0x87000000ULL, f,
                           clean_invalidate, invalidate, barrier));
  assert(f->buffer.initialized);
  assert(!f->buffer.prepared && !f->buffer.faulted);
  assert(f->buffer.attempts == 0 && f->buffer.completions == 0);
}

static void fixture_start(struct fixture *f)
{
  uintptr_t address;
  memset(f, 0, sizeof(*f));
  f->allocation = malloc(8191U);
  assert(f->allocation != NULL);
  address = ((uintptr_t)f->allocation + 4095U) & ~(uintptr_t)4095U;
  f->page = (sd_u8 *)address;
  initialize(f);
}

static void fixture_end(struct fixture *f)
{
  free(f->allocation);
  ++cases;
}

static void fill_expected(sd_u8 expected[512], sd_u32 seed)
{
  sd_u32 i;
  for (i = 0; i < 512U; ++i) {
    expected[i] = (sd_u8)((i * 73U) ^ (i >> 2) ^ seed);
  }
}

static void assert_latched(struct fixture *f, const sd_u8 expected[512])
{
  sd_u8 out[512];
  unsigned int calls = f->operation_count;
  memcpy(out, expected, sizeof(out));
  assert(f->buffer.faulted && !f->buffer.prepared);
  assert(!sd_dma_buffer_prepare(&f->buffer));
  assert(!sd_dma_buffer_complete(&f->buffer, out));
  assert(memcmp(out, expected, sizeof(out)) == 0);
  assert(f->operation_count == calls);
}

static void test_transfers(void)
{
  struct fixture f;
  sd_u8 out[512], expected[512], first_poison[512];
  unsigned int transfer;
  fixture_start(&f);
  for (transfer = 0; transfer < 2; ++transfer) {
    reset_log(&f);
    memset(out, 0x66, sizeof(out));
    assert(sd_dma_buffer_prepare(&f.buffer));
    assert(strcmp(f.operations, "CB") == 0);
    if (transfer == 0) {
      memcpy(first_poison, f.page + 512, sizeof(first_poison));
    } else {
      assert(memcmp(first_poison, f.page + 512, sizeof(first_poison)) != 0);
    }
    fill_expected(expected, transfer + 27U);
    memcpy(f.page + 512, expected, sizeof(expected)); /* Device writes. */
    assert(sd_dma_buffer_complete(&f.buffer, out));
    assert(strcmp(f.operations, "CBBIB") == 0);
    assert(memcmp(out, expected, sizeof(out)) == 0);
    assert(f.buffer.attempts == transfer + 1U);
    assert(f.buffer.completions == transfer + 1U);
    assert(f.buffer.guards_ok && !f.buffer.prepared && !f.buffer.faulted);
  }
  fixture_end(&f);
}

static void test_generation_boundary(void)
{
  struct fixture f;
  sd_u8 previous[512], out[512];
  unsigned int generation;
  fixture_start(&f);
  f.buffer.attempts = 254;
  for (generation = 255; generation < 258; ++generation) {
    reset_log(&f);
    assert(sd_dma_buffer_prepare(&f.buffer));
    if (generation != 255) {
      assert(memcmp(previous, f.page + 512, sizeof(previous)) != 0);
    }
    memcpy(previous, f.page + 512, sizeof(previous));
    assert(sd_dma_buffer_complete(&f.buffer, out));
    assert(f.buffer.attempts == generation);
  }
  fixture_end(&f);
}

static void test_every_guard_byte(void)
{
  sd_u32 offset;
  for (offset = 0; offset < 4096U; ++offset) {
    struct fixture f;
    sd_u8 out[512], original[512];
    if (offset >= 512U && offset < 1024U) {
      continue;
    }
    fixture_start(&f);
    fill_expected(original, 41U);
    memcpy(out, original, sizeof(out));
    assert(sd_dma_buffer_prepare(&f.buffer));
    f.page[offset] ^= 1U;
    assert(!sd_dma_buffer_complete(&f.buffer, out));
    assert(strcmp(f.operations, "CBBIB") == 0);
    assert(memcmp(out, original, sizeof(out)) == 0);
    assert(f.buffer.completions == 0 && !f.buffer.guards_ok);
    assert_latched(&f, original);
    fixture_end(&f);
  }
}

static void test_physical_bounds(void)
{
  static const sd_u64 invalid[] = {
    0, 1, 4095, 4097, 0xffffefffULL, 0xfffff001ULL,
    0xffffffffULL, 0x100000000ULL, ~(sd_u64)0
  };
  static const sd_u64 valid[] = { 0x1000ULL, 0xfffff000ULL };
  unsigned int i;
  for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    struct fixture f;
    sd_u8 original[512];
    fixture_start(&f);
    memset(original, 0xcc, sizeof(original));
    assert(!sd_dma_buffer_init(&f.buffer, f.page, invalid[i], &f,
                              clean_invalidate, invalidate, barrier));
    assert(!f.buffer.initialized && f.buffer.faulted);
    assert_latched(&f, original);
    fixture_end(&f);
  }
  for (i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
    struct fixture f;
    sd_u8 out[512];
    fixture_start(&f);
    assert(sd_dma_buffer_init(&f.buffer, f.page, valid[i], &f,
                             clean_invalidate, invalidate, barrier));
    assert(sd_dma_buffer_prepare(&f.buffer));
    assert(sd_dma_buffer_complete(&f.buffer, out));
    fixture_end(&f);
  }
}

static void test_bad_configuration(void)
{
  unsigned int variant;
  for (variant = 0; variant < 5; ++variant) {
    struct fixture f;
    sd_u8 original[512];
    fixture_start(&f);
    memset(original, 0xcc, sizeof(original));
    assert(!sd_dma_buffer_init(&f.buffer,
                              variant == 0 ? NULL :
                              variant == 1 ? f.page + 1 : f.page,
                              0x87000000ULL, &f,
                              variant == 2 ? NULL : clean_invalidate,
                              variant == 3 ? NULL : invalidate,
                              variant == 4 ? NULL : barrier));
    assert_latched(&f, original);
    fixture_end(&f);
  }
}

static void test_lost_callback(void)
{
  unsigned int variant;
  for (variant = 0; variant < 3; ++variant) {
    struct fixture f;
    sd_u8 out[512], original[512];
    fixture_start(&f);
    memset(original, 0xcc, sizeof(original));
    memcpy(out, original, sizeof(out));
    assert(sd_dma_buffer_prepare(&f.buffer));
    if (variant == 0) f.buffer.clean_invalidate = NULL;
    if (variant == 1) f.buffer.invalidate = NULL;
    if (variant == 2) f.buffer.barrier = NULL;
    assert(!sd_dma_buffer_complete(&f.buffer, out));
    assert(memcmp(out, original, sizeof(out)) == 0);
    assert(strcmp(f.operations, "CB") == 0);
    assert_latched(&f, original);
    fixture_end(&f);
  }
}

static void test_bad_sequence(void)
{
  unsigned int variant;
  for (variant = 0; variant < 4; ++variant) {
    struct fixture f;
    sd_u8 out[512], original[512];
    fixture_start(&f);
    memset(original, 0xcc, sizeof(original));
    memcpy(out, original, sizeof(out));
    if (variant == 0) {
      assert(!sd_dma_buffer_complete(&f.buffer, out));
    } else if (variant == 1) {
      assert(sd_dma_buffer_prepare(&f.buffer));
      assert(!sd_dma_buffer_prepare(&f.buffer));
    } else if (variant == 2) {
      assert(sd_dma_buffer_prepare(&f.buffer));
      assert(sd_dma_buffer_complete(&f.buffer, out));
      memcpy(out, original, sizeof(out));
      assert(!sd_dma_buffer_complete(&f.buffer, out));
    } else {
      f.buffer.attempts = 0xffffffffU;
      assert(!sd_dma_buffer_prepare(&f.buffer));
    }
    assert(memcmp(out, original, sizeof(out)) == 0);
    assert_latched(&f, original);
    fixture_end(&f);
  }
}

static void test_bad_destination(void)
{
  unsigned int variant;
  for (variant = 0; variant < 5; ++variant) {
    struct fixture f;
    sd_u8 page_copy[4096], original[512];
    sd_u8 *destination;
    fixture_start(&f);
    memset(original, 0xcc, sizeof(original));
    assert(sd_dma_buffer_prepare(&f.buffer));
    memcpy(page_copy, f.page, sizeof(page_copy));
    destination = variant == 0 ? NULL :
                  variant == 1 ? f.page :
                  variant == 2 ? f.page + 512 :
                  variant == 3 ? f.page + 4095 :
                  (sd_u8 *)(uintptr_t)(UINTPTR_MAX - 255U);
    assert(!sd_dma_buffer_complete(&f.buffer, destination));
    assert(memcmp(page_copy, f.page, sizeof(page_copy)) == 0);
    assert_latched(&f, original);
    fixture_end(&f);
  }
}

/* No-DMA and partial-DMA completions intentionally cannot be distinguished
 * from legitimate sectors here. This test preserves that explicit boundary:
 * caller controller checks and independent reference comparison are required.
 */
static void test_no_data_is_not_proven_by_guards(void)
{
  unsigned int partial;
  for (partial = 0; partial < 2; ++partial) {
    struct fixture f;
    sd_u8 out[512], expected[512];
    fixture_start(&f);
    fill_expected(expected, 97U);
    assert(sd_dma_buffer_prepare(&f.buffer));
    if (partial) memcpy(f.page + 512, expected, 128U);
    assert(sd_dma_buffer_complete(&f.buffer, out));
    assert(memcmp(out, expected, sizeof(out)) != 0);
    fixture_end(&f);
  }
}

static void test_abort(void)
{
  unsigned int variant;
  for (variant = 0; variant < 4; ++variant) {
    struct fixture f;
    sd_u8 original[512], page_copy[4096];
    fixture_start(&f);
    fill_expected(original, 141U);
    if (variant != 3) {
      assert(sd_dma_buffer_prepare(&f.buffer));
      memcpy(f.page + 512, original, sizeof(original));
      if (variant == 1) f.page[0] ^= 1U;
      if (variant == 2) f.page[4095] ^= 1U;
      memcpy(page_copy, f.page, sizeof(page_copy));
    }
    assert(sd_dma_buffer_abort(&f.buffer) == (variant == 0));
    if (variant != 3) {
      assert(strcmp(f.operations, "CBBIB") == 0);
      assert(memcmp(page_copy, f.page, sizeof(page_copy)) == 0);
      assert(f.buffer.guards_ok == (variant == 0));
    } else {
      assert(f.operation_count == 0);
    }
    assert(f.buffer.completions == 0);
    assert_latched(&f, original);
    assert(!sd_dma_buffer_abort(&f.buffer));
    assert(f.operation_count == (variant == 3 ? 0U : 5U));
    fixture_end(&f);
  }
}

int main(void)
{
  assert(!sd_dma_buffer_init(NULL, NULL, 0, NULL, NULL, NULL, NULL));
  assert(!sd_dma_buffer_prepare(NULL));
  assert(!sd_dma_buffer_complete(NULL, NULL));
  assert(!sd_dma_buffer_abort(NULL));
  ++cases;
  test_transfers();
  test_generation_boundary();
  test_every_guard_byte();
  test_physical_bounds();
  test_bad_configuration();
  test_lost_callback();
  test_bad_sequence();
  test_bad_destination();
  test_no_data_is_not_proven_by_guards();
  test_abort();
  printf("DMA buffer: %u cases PASS (including 3584 individual guard-byte corruptions)\n", cases);
  return 0;
}
