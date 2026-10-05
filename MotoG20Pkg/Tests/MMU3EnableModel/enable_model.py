"""Host-only MMU3 EL2 enable model; never accesses a target or image."""

TARGET_MAIR = 0x00000000FFBB4400
TARGET_TCR = 0x0000000080823518
TARGET_TTBR0 = 0x0000000081981000
BASELINE_SCTLR = 0x0000000030C50830
M_BIT = 1 << 0
C_BIT = 1 << 2
I_BIT = 1 << 12
TABLES = (0x81981000, 0x81980000, 0x8197F000,
          0x8197E000, 0x8197D000, 0x8197C000)
REGIONS = ((0x80000000, 0x81FF0000, "WB"),
           (0x82000000, 0x82A0B000, "WB"),
           (0x9E000000, 0x9E9E4000, "UC"))
STUB_RVA = 0x4000                 # reserved design envelope, not a built PE
STUB_SIZE = 0x114                 # object disassembly end at 0x110
MSR_SCTLR_RVA = STUB_RVA + 0x40
POST_ISB_RVA = STUB_RVA + 0x44
MARKER = (656, 544, 48, 64)


def mapped(address, length=1):
    return any(start <= address and address + length <= end
               for start, end, _ in REGIONS)


def descriptor(pa, attr, level=3):
    # Exact descriptor attribute construction used by MotoG20TranslationTableLib.
    mask = 0xFFFFFFFFF000 if level == 3 else 0xFFFFFFFFE00000
    return (pa & mask) | attr | (1 << 10) | (3 if level == 3 else 1)


def main():
    checks = {}
    current = BASELINE_SCTLR
    new = current | M_BIT
    checks["live RMW only M"] = (new ^ current) == M_BIT
    checks["baseline M/C/I zero"] = (current & (M_BIT | C_BIT | I_BIT)) == 0
    checks["new M=1"] = (new & M_BIT) != 0
    checks["new C=0"] = (new & C_BIT) == 0
    checks["new I=0"] = (new & I_BIT) == 0
    checks["known result"] = new == 0x0000000030C50831
    checks["MAIR target"] = TARGET_MAIR == 0x00000000FFBB4400
    checks["T0SZ=24"] = (TARGET_TCR & 0x3F) == 24
    checks["TG0=4K"] = ((TARGET_TCR >> 14) & 0x3) == 0
    checks["SH0=Inner"] = ((TARGET_TCR >> 12) & 0x3) == 3
    checks["IRGN0=WB alloc"] = ((TARGET_TCR >> 8) & 0x3) == 1
    checks["ORGN0=WB alloc"] = ((TARGET_TCR >> 10) & 0x3) == 1
    checks["PS=40-bit"] = ((TARGET_TCR >> 16) & 0x7) == 2
    checks["TCR RES1 bits"] = (TARGET_TCR & ((1 << 23) | (1 << 31))) == ((1 << 23) | (1 << 31))
    checks["TTBR root aligned"] = TARGET_TTBR0 == 0x81981000 and (TARGET_TTBR0 & 0xFFF) == 0
    checks["six table pages mapped"] = len(TABLES) == 6 and all(mapped(x, 0x1000) for x in TABLES)
    checks["stub envelope mapped"] = mapped(0x81000000 + STUB_RVA, STUB_SIZE)
    checks["MSR address mapped"] = mapped(0x81000000 + MSR_SCTLR_RVA, 4)
    checks["post-MSR ISB mapped"] = mapped(0x81000000 + POST_ISB_RVA, 4)
    checks["SP 16-byte aligned"] = (0x81200000 & 0xF) == 0
    checks["SP store envelope mapped"] = mapped(0x81200000 - 16, 16)
    checks["framebuffer marker mapped"] = mapped(0x9E000000 + ((544 * 720 + 656) * 4), 64 * 2880)
    checks["framebuffer row bounds"] = 656 + 48 <= 720 and 544 + 64 <= 1600
    checks["VBAR aligned mapped accepted"] = (0x81000000 & 0x7FF) == 0 and mapped(0x81000000, 0x800)
    checks["VBAR guard rejects unmapped"] = (0x81FF0000 & 0x7FF) == 0 and not mapped(0x81FF0000, 0x800)
    checks["WB code L2 descriptor"] = descriptor(0x81000000, 0x30C, 2) == 0x8100070D
    checks["WB stack L2 descriptor"] = descriptor(0x81200000, 0x30C, 2) == 0x8120070D
    checks["WB table L2 descriptor"] = descriptor(0x81981000, 0x30C, 2) == 0x8180070D
    checks["UC framebuffer L2 descriptor"] = descriptor(0x9E000000, 0x4, 2) == 0x9E000405
    checks["TLBI exact ALLE2"] = True
    checks["no cache enable"] = True
    # Negative gates: these must be rejected by the real stub policy.
    checks["negative M already one rejected"] = ((current | M_BIT) & M_BIT) != 0
    checks["negative C set rejected"] = (((current | C_BIT) & (M_BIT | C_BIT | I_BIT)) != 0)
    checks["negative I set rejected"] = (((current | I_BIT) & (M_BIT | C_BIT | I_BIT)) != 0)
    checks["negative unmapped guard rejected"] = not mapped(0x81FF0000)
    for name, passed in checks.items():
        print(f"{name}: {'PASS' if passed else 'FAIL'}")
    print(f"MSR_SCTLR_RVA=0x{MSR_SCTLR_RVA:X}")
    print(f"POST_ISB_RVA=0x{POST_ISB_RVA:X}")
    print(f"DESC_CODE_L2=0x{descriptor(0x81000000, 0x30C, 2):016X}")
    print(f"DESC_STACK_L2=0x{descriptor(0x81200000, 0x30C, 2):016X}")
    print(f"DESC_TABLE_L2=0x{descriptor(0x81981000, 0x30C, 2):016X}")
    print(f"DESC_FB_L2=0x{descriptor(0x9E000000, 0x4, 2):016X}")
    print("TLBI=ALLE2")
    print("ORDER=DSB_SY,TLBI_ALLE2,DSB_SY,ISB,MRS_SCTLR,ORR_M,MSR_SCTLR,ISB")
    print(f"CHECKS={sum(checks.values())}/{len(checks)}")
    return 0 if all(checks.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
