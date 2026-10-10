/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MOTO_SDHC_RO_H
#define MOTO_SDHC_RO_H

#ifdef SDRO_EDK2
#include <Base.h>
typedef UINT8 sd_u8;
typedef UINT32 sd_u32;
typedef UINT64 sd_u64;
#else
#include <stdint.h>
typedef uint8_t sd_u8;
typedef uint32_t sd_u32;
typedef uint64_t sd_u64;
#endif

enum sdro_status {
  SDRO_OK, SDRO_ARGUMENT, SDRO_NOT_READY, SDRO_NO_CARD, SDRO_TIMEOUT,
  SDRO_IO_ERROR, SDRO_CARD_ERROR, SDRO_UNSUPPORTED, SDRO_RANGE
};

/* All accesses are aligned 32-bit SD-controller offsets, never addresses.
 * access_ready must certify that this SD instance is mapped, clocked and powered.
 * card_present returns 1 (present), 0 (absent), -1 (unavailable).
 * Callbacks must be bounded; the core cannot bound a stalled MMIO bus itself.
 */
struct sdro_io {
  void *context;
  sd_u32 (*read32)(void *, sd_u32);
  void (*write32)(void *, sd_u32, sd_u32);
  void (*delay_us)(void *, sd_u32);
  int (*access_ready)(void *);
  int (*card_present)(void *);
};

struct sdro_card {
  sd_u64 blocks;
  sd_u32 rca;
  sd_u32 ocr;
  sd_u8 cid[16];
  sd_u8 csd[16];
  int block_addressed;
  int selected;
};

/* Optional native Unisoc SDMA. The owner reserves a non-reused physical buffer
 * for the entire session. prepare poisons/synchronizes that 512-byte buffer;
 * complete synchronizes, checks ownership/guards and copies exactly 512 bytes
 * into the supplied staging array. Both callbacks must return exactly 1.
 * Neither callback may reconfigure or issue commands on this host. */
struct sdro_dma {
  void *context;
  sd_u32 address;
  int (*prepare)(void *);
  int (*complete)(void *, sd_u8 *data);
};

#define SDRO_SNAPSHOT_SETUP 1u
#define SDRO_SNAPSHOT_COMMAND 2u
#define SDRO_SNAPSHOT_TRANSFER 3u
#define SDRO_SNAPSHOT_FAILURE 4u
/* snapshot_valid bits 0..9 correspond to IRQ, IRQ_ENABLE, PRESENT, HC1, HC2,
 * COMMAND, RESPONSE, COUNT, DMA_LOW, DMA_HIGH respectively. */
#define SDRO_SNAPSHOT_ALL 0x3ffu

struct sdro_host {
  struct sdro_io io;
  sd_u32 clock_hz;
  sd_u32 voltage_ocr;
  sd_u32 last_irq;
  sd_u32 last_response;
  sd_u32 commands;
  sd_u32 blocks_read;
  sd_u32 transfer_version, transfer_block, transfer_count, transfer_mode, transfer_count_offset;
  sd_u32 snapshot_valid, snapshot_phase, snapshot_irq, snapshot_irq_enable;
  sd_u32 snapshot_present, snapshot_host_control1, snapshot_host_control2;
  sd_u32 snapshot_command, snapshot_response, snapshot_count;
  sd_u32 snapshot_dma_low, snapshot_dma_high;
  sd_u8 last_command;
  enum sdro_status last_status;
  int faulted;
  int initialized;
  int identifying;
  int dma_enabled;
  int transfer_active;
  struct sdro_dma dma;
  struct sdro_card card;
};

/* Platform must reset/reinitialize controller and supply a fresh session before
 * calling init. Identification remains at <=400kHz; no clock writes occur here.
 * Supports SD v2 SDSC/SDHC/SDXC CSD v1/v2; SD v1 CMD8 timeout is unsupported.
 * PIO is the default; native 32-bit SDMA is explicit opt-in after identify.
 * No data writes, CMD11/voltage switch, ACMD6 or multi-block commands.
 */
enum sdro_status sdro_init(struct sdro_host *, const struct sdro_io *, sd_u32 clock_hz, sd_u32 voltage_ocr);
enum sdro_status sdro_identify(struct sdro_host *);

enum sdro_status sdro_set_dma(struct sdro_host *, const struct sdro_dma *);
enum sdro_status sdro_read_block(struct sdro_host *, sd_u64 lba, sd_u8 *data, sd_u32 length);
enum sdro_status sdro_parse_csd(const sd_u8 csd[16], int block_addressed, sd_u64 *blocks);

/* Unisoc's divider differs from generic SDHCI. Pure calculation; no MMIO.
 * Uses ceil(base/(4*target)); divider 0 is base/2. Reject unachievable targets.
 */
enum sdro_status sdro_unisoc_divider(sd_u32 base_hz, sd_u32 target_hz, sd_u32 *encoded, sd_u32 *actual_hz);

#endif
