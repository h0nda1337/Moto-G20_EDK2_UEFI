/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MOTO_SD_DMA_BUFFER_H
#define MOTO_SD_DMA_BUFFER_H

#include "sdhci_ro.h"

#define SD_DMA_PAGE_SIZE   4096U
#define SD_DMA_DATA_OFFSET 512U
#define SD_DMA_DATA_SIZE   512U

typedef void (*sd_dma_cache_fn)(void *, void *, sd_u32);
typedef void (*sd_dma_barrier_fn)(void *);

/* Owns no memory. The caller owns a dedicated, page-aligned, DMA-accessible
 * page and must keep it mapped and allocated while the controller can use it.
 * Cache callbacks operate on the entire page, including both guard regions.
 */
struct sd_dma_buffer {
  sd_u8 *page;
  sd_u64 physical_base;
  void *cache_context;
  sd_dma_cache_fn clean_invalidate;
  sd_dma_cache_fn invalidate;
  sd_dma_barrier_fn barrier;
  sd_u32 attempts;
  sd_u32 completions;
  int initialized;
  int prepared;
  int faulted;
  int guards_ok;
};

/* Only initialize a fresh controller session. Reinitialization is not a
 * recovery mechanism for an active or faulted DMA transfer.
 */
int sd_dma_buffer_init(struct sd_dma_buffer *, sd_u8 *page,
                       sd_u64 physical_base, void *cache_context,
                       sd_dma_cache_fn clean_invalidate,
                       sd_dma_cache_fn invalidate,
                       sd_dma_barrier_fn barrier);

/* DMA address is physical_base + SD_DMA_DATA_OFFSET. These signatures can
 * be installed directly as core callbacks, with the helper as their context.
 * Every invalid call or guard violation latches a fault for this session.
 */
int sd_dma_buffer_prepare(void *);
int sd_dma_buffer_complete(void *, sd_u8 *out512);

/* Error-path cache cleanup for a prepared transfer. Never copies payload.
 * Returns guard integrity, while always faulting this session to forbid reuse.
 * This does not stop DMA; controller reset/quiescence belongs to the caller.
 */
int sd_dma_buffer_abort(void *);

/* A successful complete proves guard integrity, not delivery of fresh card
 * data. The caller must first validate controller completion/error status.
 * Physical validation additionally compares all 512 bytes with independent
 * sector references. Poison is diagnostic and is never accepted as proof.
 */

#endif
