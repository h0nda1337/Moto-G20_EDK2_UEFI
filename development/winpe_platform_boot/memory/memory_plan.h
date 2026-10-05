/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MOTO_MEMORY_PLAN_H
#define MOTO_MEMORY_PLAN_H
#ifdef MOTO_EDK2
#include <Base.h>
typedef UINT8 mp_u8;
typedef UINT32 mp_u32;
typedef UINT64 mp_u64;
typedef UINTN mp_size;
#else
#include <stdint.h>
#include <stddef.h>
typedef uint8_t mp_u8;
typedef uint32_t mp_u32;
typedef uint64_t mp_u64;
typedef size_t mp_size;
#endif
struct mp_range { mp_u64 base, size; };
struct mp_plan {
  struct mp_range regions[2];
  mp_u32 count, reserved_count, memory_count;
  mp_u64 total;
};
enum mp_result {
  MP_OK = 0, MP_ARGUMENT, MP_HEADER, MP_STRUCTURE, MP_CELLS,
  MP_BOARD, MP_MISSING, MP_RANGE, MP_OVERLAP, MP_DYNAMIC_RESERVATION
};
/* Pure bounded DTB validator; never accesses memory outside the supplied blob.
 * Candidate ranges are fixed, conservatively rounded, and require positive
 * coverage by a /memory reg plus absence from every reservation in this boot.
 */
enum mp_result mp_make_plan(const void *blob, mp_size available, struct mp_plan *out);
#endif
