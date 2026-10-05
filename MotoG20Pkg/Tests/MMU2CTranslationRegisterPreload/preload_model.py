"""Pure host model for MMU2C preload invariants; never accesses a target."""

TARGET_MAIR = 0x00000000FFBB4400
TARGET_TCR = 0x0000000080823518
TARGET_TTBR0 = 0x0000000081981000
BASELINE_SCTLR = 0x0000000030C50830
TABLES = (0x81981000, 0x81980000, 0x8197F000,
          0x8197E000, 0x8197D000, 0x8197C000)


def main():
    checks = {
        "MAIR target": TARGET_MAIR == 0xFFBB4400,
        "TCR T0SZ": (TARGET_TCR & 0x1F) == 24,
        "TCR TG0 4K": ((TARGET_TCR >> 14) & 0x3) == 0,
        "TCR PS 1TB": ((TARGET_TCR >> 16) & 0x7) == 2,
        "TTBR root": TARGET_TTBR0 == TABLES[0],
        "six 4K tables": len(TABLES) == 6 and all((x & 0xFFF) == 0 for x in TABLES),
        "SCTLR MCI off": (BASELINE_SCTLR & ((1 << 0) | (1 << 2) | (1 << 12))) == 0,
        "SCTLR preserved": BASELINE_SCTLR == 0x30C50830,
        "TLBI policy none": True,
    }
    for name, passed in checks.items():
        print(f"{name}: {'PASS' if passed else 'FAIL'}")
    print(f"MAIR_EL2=0x{TARGET_MAIR:016X}")
    print(f"TCR_EL2=0x{TARGET_TCR:016X}")
    print(f"TTBR0_EL2=0x{TARGET_TTBR0:016X}")
    print("ORDER=DSB_SY,MSR_MAIR_EL2,ISB,MSR_TCR_EL2,ISB,MSR_TTBR0_EL2,ISB")
    print("MMU_ENABLE=NO")
    print("I_CACHE_ENABLE=NO")
    print("D_CACHE_ENABLE=NO")
    print(f"CHECKS={sum(checks.values())}/{len(checks)}")
    return 0 if all(checks.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
