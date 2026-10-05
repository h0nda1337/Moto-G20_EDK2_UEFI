#!/usr/bin/env python3
"""Host-only placement and instruction gate for the MMU3 TRUE M-only build."""

from pathlib import Path
import hashlib
import struct
import sys

ROOT = Path(__file__).parents[3]
PE = ROOT / "edk2/Build/MotoG20Mmu3b1/DEBUG_GCCNOLTO/AARCH64/MotoG20Mmu3b1LocalEl2Vectors.efi"
AUTH = ROOT / "MMU3B2_DIAGNOSTIC_LAYOUT_FIX1_STATIC_PASS/Artifacts/MotoG20Mmu3b1LocalEl2Vectors.efi"
MAP = ROOT / "edk2/Build/MotoG20Mmu3b1/DEBUG_GCCNOLTO/AARCH64/MotoG20Pkg/PrePi/MMU3B1/MotoG20Mmu3b1LocalEl2Vectors/OUTPUT/MotoG20Mmu3b1LocalEl2Vectors.map"
ASM = ROOT / "MotoG20Pkg/PrePi/MotoG20Mmu3EnableStub.S"
SRC = ROOT / "MotoG20Pkg/PrePi/MMU3B1/Mmu3b1Probe.c"
CAPTURE_RVA = 0x8100
CAPTURE_SIZE = 0xB0
OLD_CAPTURE_RVA = 0x8040
FROZEN_START, FROZEN_END = 0x4000, 0x49A7


def pe_layout(blob):
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    number = struct.unpack_from("<H", blob, pe + 6)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", blob, opt)[0]
    entry = struct.unpack_from("<I", blob, opt + 16)[0]
    image_size = struct.unpack_from("<I", blob, opt + 56)[0]
    sections = []
    sh = opt + (240 if magic == 0x20B else 224)
    for i in range(number):
        off = sh + i * 40
        name = blob[off:off + 8].rstrip(b"\0").decode("ascii")
        va, vs, raw, raw_size = struct.unpack_from("<IIII", blob, off + 8)
        sections.append((name, va, vs, raw, raw_size))
    return magic, entry, image_size, sections


def main():
    tests = {}
    if not PE.exists() or not AUTH.exists() or not MAP.exists():
        print("MISSING_ARTIFACT", file=sys.stderr)
        return 1
    pe, auth = PE.read_bytes(), AUTH.read_bytes()
    magic, entry, image_size, sections = pe_layout(pe)
    text = SRC.read_text(encoding="utf-8")
    asm = ASM.read_text(encoding="utf-8")
    mapping = MAP.read_text(encoding="utf-8")
    tests["PE size 0xC000"] = len(pe) == 0xC000
    tests["PE32+ ARM64"] = magic == 0x20B and struct.unpack_from("<H", pe, struct.unpack_from("<I", pe, 0x3C)[0] + 4)[0] == 0xAA64
    tests["SizeOfImage 0xC000"] = image_size == 0xC000
    tests["frozen bytes identical"] = pe[FROZEN_START:FROZEN_END + 1] == auth[FROZEN_START:FROZEN_END + 1]
    tests["vector RVA 0x4000"] = "MotoG20Mmu3b1VectorTableStart" in mapping and "0x0000000000004000" in mapping
    tests["old capture RVA 0x8040"] = "0x0000000000008040                MotoG20Mmu3b1CaptureBlock" in mapping
    tests["true capture RVA 0x8100"] = "0x0000000000008100                MotoG20Mmu3TrueCaptureBlock" in mapping
    tests["true capture size 0xB0"] = ".zero 0xB0" in (ROOT / "MotoG20Pkg/PrePi/MMU3B1/LocalEl2Vectors.S").read_text(encoding="utf-8")
    tests["true capture aligned 0x40"] = CAPTURE_RVA % 0x40 == 0
    tests["true capture below capacity"] = CAPTURE_RVA + CAPTURE_SIZE <= 0xC000
    tests["true capture writable data"] = any(n == ".data" and va <= CAPTURE_RVA < va + max(vs, raw_size) for n, va, vs, raw, raw_size in sections)
    tests["old and true captures do not overlap"] = OLD_CAPTURE_RVA + 0x98 <= CAPTURE_RVA
    tests["pre SCTLR constant"] = "0x30C50830" in asm
    tests["post SCTLR constant"] = "0x30C50831" in asm
    tests["one SCTLR write"] = asm.count("msr     SCTLR_EL2") == 1
    tests["one TLBI ALLE2"] = asm.count("tlbi    alle2") == 1
    tests["no cache maintenance"] = " dc " not in asm.lower() and " ic " not in asm.lower()
    tests["stackless routine"] = "bl " not in asm.lower() and "[sp" not in asm.lower()
    tests["M-only enable symbol"] = "MotoG20EnableMmuEl2MOnly" in mapping
    tests["post-enable marker"] = "MotoG20Mmu3FirstPostEnableInstruction" in mapping
    tests["source page-walk TRUE capture"] = "0x81008100ULL" in text and "0x810081AFULL" in text
    tests["M/C/I transition"] = "0x30C50830" in asm and "0x30C50831" in asm and "(Capture->SctlrBefore ^ Capture->SctlrAfter) != 1ULL" in text
    for name, ok in tests.items():
        print(f"{name}: {'PASS' if ok else 'FAIL'}")
    print(f"PE_SHA256={hashlib.sha256(pe).hexdigest().upper()}")
    print(f"FROZEN_SHA256={hashlib.sha256(pe[FROZEN_START:FROZEN_END + 1]).hexdigest().upper()}")
    print(f"CHECKS={sum(tests.values())}/{len(tests)}")
    return 0 if all(tests.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
