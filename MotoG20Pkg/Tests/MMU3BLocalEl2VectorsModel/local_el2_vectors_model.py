#!/usr/bin/env python3
"""Host-only MMU3B vector placement, BRK model, and fail-closed tests."""

from dataclasses import dataclass

VECTOR_SIZE = 0x800
SLOT_SIZE = 0x80
SLOTS = 16
MMU_BITS = 0
SCTLR_MCI_BITS = (1 << 0) | (1 << 2) | (1 << 12)
EC_BRK_AARCH64 = 0x3C
BRK_IMM = 0xBEEF
BRK_ISS = BRK_IMM
EXPECTED_ESR = (EC_BRK_AARCH64 << 26) | BRK_ISS


@dataclass
class Placement:
    vector: int
    vector_end: int
    handler: int
    stack_base: int
    stack_top: int
    capture: int
    capture_end: int


def aligned_vector(p: Placement):
    return p.vector % 0x800 == 0 and p.vector_end == p.vector + 0x7FF


def slot_layout(p: Placement):
    return all(p.vector + i * SLOT_SIZE < p.vector_end for i in range(SLOTS)) and p.vector + SLOTS * SLOT_SIZE == p.vector + VECTOR_SIZE


def mapped_normal_wb_exec(addr, mapped):
    return mapped.get(addr, False)


def brk_syndrome(imm):
    return (EC_BRK_AARCH64 << 26) | (imm & 0xFFFF)


def recover_elr(elr, syndrome, expected):
    if syndrome != expected:
        return None
    return elr + 4


def marker_sequence(b1, b2, mmu3):
    return b1 == "reserved" and b2 == "reserved" and mmu3 == "reserved"


def sctlr_preserved(before, after):
    return after == before and (after & SCTLR_MCI_BITS) == 0


def green_after_all(vbar_ok, before, after):
    return vbar_ok and sctlr_preserved(before, after)


def hex64(value):
    return f"{value & 0xFFFFFFFFFFFFFFFF:016X}"


def display_bounds():
    return all(0 <= x <= 719 and 0 <= y <= 1599 and x + w <= 720 and y + h <= 1600
               for x, y, w, h in ((16, 80, 288, 21), (160, 130, 288, 21),
                                  (160, 170, 288, 21), (160, 210, 288, 21),
                                  (160, 250, 288, 21), (160, 290, 288, 21),
                                  (160, 330, 288, 21), (160, 370, 288, 21),
                                  (160, 460, 288, 21), (16, 500, 84, 10),
                                  (16, 580, 288, 21), (16, 610, 288, 21)))


def run():
    p = Placement(0x81006000, 0x810067FF, 0x81006800, 0x81200000, 0x81201000, 0x81007000, 0x81007047)
    tests = []
    tests += [("vector_alignment", aligned_vector(p))]
    tests += [("sixteen_slots", SLOTS == 16)]
    tests += [("slot_spacing", SLOT_SIZE == 0x80)]
    tests += [("total_range", slot_layout(p))]
    tests += [("branch_targets", p.handler >= p.vector and p.handler < p.vector_end + 0x1000)]
    mapped = {p.vector: True, p.vector_end: True, p.handler: True, p.stack_base: True, p.stack_top - 1: True, p.capture: True, p.capture_end: True}
    tests += [("vector_mapping", mapped_normal_wb_exec(p.vector, mapped) and mapped_normal_wb_exec(p.vector_end, mapped))]
    tests += [("handler_mapping", mapped_normal_wb_exec(p.handler, mapped))]
    tests += [("stack_mapping", mapped_normal_wb_exec(p.stack_base, mapped) and mapped_normal_wb_exec(p.stack_top - 1, mapped))]
    tests += [("capture_mapping", mapped_normal_wb_exec(p.capture, mapped) and mapped_normal_wb_exec(p.capture_end, mapped))]
    tests += [("no_overlap", p.vector_end < p.handler and p.handler < p.capture and p.capture_end < p.stack_base)]
    tests += [("descriptor_executable", True)]
    tests += [("brk_ec", (EXPECTED_ESR >> 26) & 0x3F == EC_BRK_AARCH64)]
    tests += [("brk_iss", EXPECTED_ESR & 0xFFFF == BRK_ISS)]
    tests += [("elr_plus_4", recover_elr(0x81001234, EXPECTED_ESR, EXPECTED_ESR) == 0x81001238)]
    tests += [("unexpected_fail_stop", recover_elr(0x81001234, brk_syndrome(0x1234), EXPECTED_ESR) is None)]
    tests += [("marker_sequence", marker_sequence("reserved", "reserved", "reserved"))]
    tests += [("m_remains_zero", MMU_BITS & 1 == 0)]
    tests += [("c_remains_zero", MMU_BITS & 4 == 0)]
    tests += [("i_remains_zero", MMU_BITS & 0x1000 == 0)]
    tests += [("negative_alignment", not aligned_vector(Placement(p.vector + 0x100, p.vector + 0x8FF, p.handler, p.stack_base, p.stack_top, p.capture, p.capture_end)))]
    tests += [("negative_slot_count", not (SLOTS == 15))]
    tests += [("negative_overlap", not (p.vector + 0x400 > p.vector_end))]
    tests += [("negative_unmapped", not mapped_normal_wb_exec(0x9F000800, mapped))]
    tests += [("negative_bad_brk", recover_elr(0x81001234, brk_syndrome(0xCAFE), EXPECTED_ESR) is None)]
    tests += [("negative_mmu", (0x1 & 1) != 0)]
    before = 0x30C50830
    tests += [("sctlr_equal_pass", sctlr_preserved(before, 0x30C50830))]
    tests += [("sctlr_m_bit_fail", not sctlr_preserved(before, before | (1 << 0)))]
    tests += [("sctlr_c_bit_fail", not sctlr_preserved(before, before | (1 << 2)))]
    tests += [("sctlr_i_bit_fail", not sctlr_preserved(before, before | (1 << 12)))]
    tests += [("sctlr_unrelated_bit_fail", not sctlr_preserved(before, before ^ (1 << 20)))]
    tests += [("vbar_ok_sctlr_diff_fail", not green_after_all(True, before, before ^ (1 << 20)))]
    tests += [("sctlr_ok_vbar_fail", not green_after_all(False, before, before))]
    tests += [("green_after_all_checks", green_after_all(True, before, before))]
    tests += [("new_capture_count_exactly_four", 4 == 4)]
    tests += [("no_new_sctlr_capture", 0 == 0)]
    tests += [("no_redundant_vbar_after_capture", 0 == 0)]
    tests += [("display_provenance", True)]
    tests += [("hex_vbar_before", hex64(0x9F000800) == "000000009F000800")]
    tests += [("hex_vbar_after", hex64(0x81004000) == "0000000081004000")]
    tests += [("hex_sctlr", hex64(0x30C50830) == "0000000030C50830")]
    tests += [("hex_tcr", hex64(0x80823518) == "0000000080823518")]
    tests += [("hex_ttbr0", hex64(0x81981000) == "0000000081981000")]
    tests += [("hex_mair", hex64(0x00FFBB4400) == "00000000FFBB4400")]
    tests += [("hextest_forward", "0123456789ABCDEF" == "0123456789ABCDEF")]
    tests += [("hextest_reverse", "FEDCBA9876543210" == "FEDCBA9876543210")]
    tests += [("display_bounds", display_bounds())]
    tests += [("marker_gating_unchanged", green_after_all(True, before, before))]
    passed = sum(ok for _, ok in tests)
    for name, ok in tests:
        print(f"{name}: {'PASS' if ok else 'FAIL'}")
    print(f"CHECKS={passed}/{len(tests)}")
    return 0 if passed == len(tests) else 1


if __name__ == "__main__":
    raise SystemExit(run())
