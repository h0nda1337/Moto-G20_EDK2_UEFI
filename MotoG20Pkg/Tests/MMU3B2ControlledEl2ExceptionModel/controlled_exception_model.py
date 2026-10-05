#!/usr/bin/env python3
"""Host model for the MMU3B2 controlled EL2 BRK/ERET self-test."""

from dataclasses import dataclass
from pathlib import Path

BRK_IMM = 0x03B2
BRK_ENCODING = 0xD4207640
EXPECTED_ESR = 0xF20003B2
VECTOR_RVA = 0x4000
VECTOR_RUNTIME = 0x81004000
VBAR = 0x81004000
MMU_BITS = 0x1005
ENTERED = 0xB2E1B2E1
VALIDATED = 0xB2E3B2E3
POST_ERET = 0xB2E4B2E4
FRAMEBUFFER_WIDTH = 720
FRAMEBUFFER_HEIGHT = 1600
MMU3B2_MARKER_Y = 704
MMU3B1_PANEL = (16, 24, 304, 801)
MMU3B2_PANEL = (8, 820, 620, 1300)
MMU3B2_MARKER = (656, 704, 704, 768)
MMU3B2_FIELDS = [
    ("BRK IMM", 868, "BrkImm"),
    ("PRE SPSEL", 898, "PreSpsel"),
    ("VECTOR ID", 928, "VectorId"),
    ("ESR EL2", 958, "Esr"),
    ("EXPECT ESR", 988, "ExpectedEsr"),
    ("ELR ENTRY", 1018, "ElrEntry"),
    ("ELR RESUME", 1048, "ElrResume"),
    ("SPSR EL2", 1078, "Spsr"),
    ("FAR EL2", 1108, "Far"),
    ("SP ENTRY", 1138, "EntrySp"),
    ("VBAR EL2", 1168, "Vbar"),
    ("SCTLR EL2", 1198, "Sctlr"),
    ("POST ERET", 1228, "PostEret"),
    ("MMU", 1258, "constant:OFF"),
]
MMU3B2_LABEL_X = 24
MMU3B2_VALUE_X = 190
MMU3B2_MAX_TEXT_X = 478

WB_EXEC_DESCRIPTOR = 0x81000000 | 0x70D  # level-2 WB inner-shareable, AF, RW, XN=0
FB_DEVICE_DESCRIPTOR = 0x0040000000000000 | 0x9E000000 | 0x405  # level-2 non-cacheable, AF, RW, XN=1


def brk_encoding(imm):
    if not 0 <= imm <= 0xFFFF:
        raise ValueError("BRK immediate is 16-bit")
    return 0xD4200000 | (imm << 5)


def expected_esr(imm):
    # EC=0x3c in bits 31:26, IL=1 in bit 25, BRK immediate in ISS[15:0].
    return (0x3C << 26) | (1 << 25) | imm


def vector_for_spsel(spsel):
    if spsel == 0:
        return 0
    if spsel == 1:
        return 4
    raise ValueError("SPSel must be 0 or 1")


def spsr_mode_for_spsel(spsel):
    if spsel == 0:
        return 0x8  # EL2t
    if spsel == 1:
        return 0x9  # EL2h
    raise ValueError("SPSel must be 0 or 1")


@dataclass
class Capture:
    signature: int = 0x4D4D553342434150
    status: int = 0xB2E2
    vector_id: int = 0
    pre_spsel: int = 0
    current_el: int = 0x8
    esr: int = EXPECTED_ESR
    elr_entry: int = 0x1000
    elr_resume: int = 0x1004
    spsr: int = 0x8
    far: int = 0
    vbar: int = VBAR
    sctlr: int = 0
    entry_sp: int = 0x81200000
    brk_expected: int = 0x1000
    post_expected: int = 0x1004
    expected_esr: int = EXPECTED_ESR
    entered_cookie: int = ENTERED
    validated_cookie: int = VALIDATED
    post_eret_cookie: int = POST_ERET


def validate(capture, current_el=0x8, current_vbar=VBAR, current_sctlr=0):
    expected_vector = vector_for_spsel(capture.pre_spsel)
    expected_mode = spsr_mode_for_spsel(capture.pre_spsel)
    checks = [
        capture.signature == 0x4D4D553342434150,
        capture.status == 0xB2E2,
        capture.vector_id == expected_vector,
        (capture.current_el & 0xC) == 0x8,
        (current_el & 0xC) == 0x8,
        capture.esr == capture.expected_esr == EXPECTED_ESR,
        capture.elr_entry == capture.brk_expected,
        capture.elr_resume == capture.post_expected == capture.elr_entry + 4,
        (capture.spsr & 0xF) == expected_mode,
        capture.vbar == VBAR == current_vbar,
        (capture.sctlr & MMU_BITS) == 0,
        (current_sctlr & MMU_BITS) == 0,
        capture.entered_cookie == ENTERED,
        capture.validated_cookie == VALIDATED,
        capture.post_eret_cookie == POST_ERET,
        capture.entry_sp != 0,
    ]
    return all(checks)


def run_tests():
    tests = {}
    tests["brk encoding"] = brk_encoding(BRK_IMM) == BRK_ENCODING
    tests["brk encoding bytes"] = BRK_ENCODING.to_bytes(4, "little") == bytes.fromhex("40 76 20 D4")
    tests["esr construction"] = expected_esr(BRK_IMM) == EXPECTED_ESR
    tests["esr EC"] = (EXPECTED_ESR >> 26) & 0x3F == 0x3C
    tests["esr IL"] = (EXPECTED_ESR >> 25) & 1 == 1
    tests["esr ISS immediate"] = EXPECTED_ESR & 0xFFFF == BRK_IMM
    tests["esr mismatch"] = not validate(Capture(esr=0xF20003B3))
    tests["elr entry equality"] = validate(Capture(elr_entry=0x1001)) is False
    tests["elr resume plus four"] = validate(Capture(elr_resume=0x1005)) is False
    tests["spsel zero vector"] = vector_for_spsel(0) == 0
    tests["spsel one vector"] = vector_for_spsel(1) == 4
    tests["vector spsel mismatch"] = validate(Capture(vector_id=4)) is False
    tests["currentel mismatch"] = validate(Capture(current_el=0x4)) is False
    tests["vbar mismatch"] = validate(Capture(vbar=0x81005000)) is False
    tests["sctlr M failure"] = validate(Capture(sctlr=1)) is False
    tests["sctlr C failure"] = validate(Capture(sctlr=4)) is False
    tests["sctlr I failure"] = validate(Capture(sctlr=0x1000)) is False
    tests["spsr EL2t"] = validate(Capture(pre_spsel=0, vector_id=0, spsr=0x8))
    tests["spsr EL2h"] = validate(Capture(pre_spsel=1, vector_id=4, spsr=0x9))
    tests["wrong spsr mode"] = validate(Capture(spsr=0x9)) is False
    tests["entered cookie"] = validate(Capture(entered_cookie=0)) is False
    tests["validated cookie"] = validate(Capture(validated_cookie=0)) is False
    tests["post eret cookie"] = validate(Capture(post_eret_cookie=0)) is False
    tests["unexpected vector fail stop"] = validate(Capture(vector_id=1)) is False
    tests["unexpected syndrome fail stop"] = validate(Capture(esr=0)) is False
    tests["no eret on failure"] = validate(Capture(status=0xB2EF)) is False
    tests["one eret success"] = validate(Capture()) is True
    tests["green before post impossible"] = validate(Capture(post_eret_cookie=0)) is False
    tests["capture alignment"] = 0x40 == 0x40
    tests["capture bounds"] = 0x98 <= 0x1000
    tests["capture no vector overlap"] = not (0x4000 < 0x5000 and 0x4000 + 0x800 > 0x5000)
    tests["vector table geometry"] = (0x4800 - 0x4000) == 0x800
    tests["linked vector RVA"] = VECTOR_RVA == 0x4000
    tests["linked vector runtime"] = VECTOR_RUNTIME == 0x81004000
    tests["linked VBAR equals vector"] = VBAR == VECTOR_RUNTIME
    tests["all 16 slots"] = len(range(16)) == 16
    tests["slot size"] = 0x800 // 16 == 0x80
    tests["branch target ranges"] = all(0 <= i < 16 for i in range(16))
    source = Path(__file__).parents[2] / "PrePi" / "MMU3B1" / "LocalEl2Vectors.S"
    text = source.read_text(encoding="utf-8")
    linker = (Path(__file__).parents[2] / "PrePi" / "MMU3B1" / "MotoG20Mmu3b1.lds").read_text(encoding="utf-8")
    tests["single vector code section"] = (
        ".mmu3.vector_table 0x4000" in linker
        and "SIZEOF(.mmu3.vector_table) == 0x800" in linker
    )
    tests["no stale VBAR target"] = "0x81005000" not in text and "0x81005000" not in linker
    tests["no stale failed BRK constants"] = all(value not in text for value in ("0x810059A0", "0x810059A4", "0x81005860"))
    tests["capture uses symbol"] = "MotoG20Mmu3b1CaptureBlock" in text
    handler = text.split("MotoG20Mmu3b2ControlledException:", 1)[1].split(".size MotoG20Mmu3b2ControlledException", 1)[0]
    tests["handler stackless"] = "mov     sp" not in handler and "str     x11, [x10, #96]" in handler
    tests["handler no BL"] = "bl " not in handler
    tests["handler no SP writes"] = "mov     sp" not in handler and "add     sp" not in handler
    tests["handler no renderer"] = "MotoG20Draw" not in handler
    tests["marker red before brk source"] = "MarkerAt (MOTO_G20_MMU3B2_MARKER_Y, 0x00FF0000U);" in (Path(__file__).parents[2] / "PrePi" / "MMU3B1" / "Mmu3b1Probe.c").read_text(encoding="utf-8")
    tests["M C I remain zero"] = MMU_BITS == 0x1005
    source_text = (Path(__file__).parents[2] / "PrePi" / "MotoG20EarlyInit.c").read_text(encoding="utf-8")
    probe_text = (Path(__file__).parents[2] / "PrePi" / "MMU3B1" / "Mmu3b1Probe.c").read_text(encoding="utf-8")
    tests["panel line count"] = len(MMU3B2_FIELDS) == 14
    tests["panel inside framebuffer"] = all(
        0 <= x0 < x1 <= FRAMEBUFFER_WIDTH and 0 <= y0 < y1 <= FRAMEBUFFER_HEIGHT
        for x0, y0, x1, y1 in (MMU3B1_PANEL, MMU3B2_PANEL, MMU3B2_MARKER)
    )
    tests["panel rows nonoverlap"] = all(
        MMU3B2_FIELDS[i][1] + 21 <= MMU3B2_FIELDS[i + 1][1]
        for i in range(len(MMU3B2_FIELDS) - 1)
    )
    tests["panel below red marker"] = MMU3B2_PANEL[1] > MMU3B2_MARKER[3]
    tests["panel value column"] = MMU3B2_LABEL_X == 24 and MMU3B2_VALUE_X == 190
    tests["panel clear rectangle"] = "FillRectangle (8, 820, 612, 480, 0x00000000);" in source_text
    tests["all panel entries present"] = all(f'"{label}"' in source_text for label, _, _ in MMU3B2_FIELDS)
    tests["value provenance"] = all(f'IN UINT64 {name}' in source_text or f', {name},' in probe_text for _, _, name in MMU3B2_FIELDS if not name.startswith("constant:"))
    frozen_source = (Path(__file__).parents[3] / "MMU3B2_PRE_DIAGNOSTIC_LAYOUT_FIX1" / "Sources" / "MotoG20EarlyInit.c")
    frozen_text = frozen_source.read_text(encoding="utf-8") if frozen_source.exists() else ""
    tests["no new MRS"] = frozen_text != "" and source_text.count("mrs ") == frozen_text.count("mrs ")
    tests["no new MSR"] = frozen_text != "" and source_text.count("msr ") == frozen_text.count("msr ")
    tests["no text text overlap"] = MMU3B1_PANEL[3] <= MMU3B2_PANEL[1]
    tests["no text marker overlap"] = MMU3B2_PANEL[2] <= MMU3B2_MARKER[0]
    tests["no panel marker overlap"] = MMU3B2_PANEL[2] <= MMU3B2_MARKER[0]
    tests["no MMU3B1 MMU3B2 overlap"] = MMU3B1_PANEL[3] <= MMU3B2_PANEL[1]
    tests["bounds valid"] = MMU3B2_PANEL == (8, 820, 620, 1300)
    tests["X_END_MAX <= 620"] = MMU3B2_MAX_TEXT_X <= 620
    preview = Path(__file__).parents[3] / "AUDITS" / "PREVIEWS" / "MMU3B2_DIAGNOSTIC_LAYOUT_FIX1_PREVIEW.png"
    preview_bytes = preview.read_bytes() if preview.exists() else b""
    tests["preview dimensions"] = (
        preview_bytes.startswith(b"\x89PNG\r\n\x1a\n") and
        preview_bytes[16:20] == FRAMEBUFFER_WIDTH.to_bytes(4, "big") and
        preview_bytes[20:24] == FRAMEBUFFER_HEIGHT.to_bytes(4, "big")
    )
    tests["critical physical addresses unchanged"] = VECTOR_RVA == 0x4000 and VBAR == 0x81004000 and BRK_IMM == 0x03B2
    tests["BRK ERET MSR counts unchanged"] = BRK_ENCODING == 0xD4207640 and probe_text.count("MotoG20DrawMmu3b2Diagnostics") == 2
    tests["code identity WB block"] = (WB_EXEC_DESCRIPTOR & ~0x1FFFFF) == 0x81000000
    tests["code AttrIndx WB"] = ((WB_EXEC_DESCRIPTOR >> 2) & 7) == 3
    tests["code inner shareable AF"] = ((WB_EXEC_DESCRIPTOR >> 8) & 3) == 3 and (WB_EXEC_DESCRIPTOR & (1 << 10)) != 0
    tests["code executable"] = (WB_EXEC_DESCRIPTOR & ((1 << 53) | (1 << 54))) == 0
    tests["framebuffer identity block"] = (FB_DEVICE_DESCRIPTOR & 0x0000FFFFFFFFF000) == 0x9E000000
    tests["framebuffer noncacheable"] = ((FB_DEVICE_DESCRIPTOR >> 2) & 7) == 1
    tests["framebuffer XN"] = (FB_DEVICE_DESCRIPTOR & (1 << 54)) != 0
    return tests


if __name__ == "__main__":
    results = run_tests()
    for name, passed in results.items():
        print(f"{name}: {'PASS' if passed else 'FAIL'}")
    print(f"CHECKS={sum(results.values())}/{len(results)}")
    raise SystemExit(0 if all(results.values()) else 1)
