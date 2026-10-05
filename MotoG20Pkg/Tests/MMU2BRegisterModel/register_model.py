"""Pure host-side arithmetic model for the MMU2B register derivation.

This file performs no target access and emits no target instructions.
"""

MAIR = sum(value << (index * 8) for index, value in enumerate((0x00, 0x44, 0xBB, 0xFF, 0, 0, 0, 0)))

T0SZ = 64 - 40
TCR = (
    T0SZ
    | (0x1 << 8)       # inner WB allocation for table walks
    | (0x1 << 10)      # outer WB allocation for table walks
    | (0x3 << 12)      # inner shareable
    | (0x2 << 16)      # 1 TB physical address size
    | (1 << 23)        # classic EL2 RES1
    | (1 << 31)        # classic EL2 RES1
)

TTBR0 = 0x81981000


def attr_byte(index):
    return (MAIR >> (index * 8)) & 0xFF


def main():
    checks = {
        "MAIR value": MAIR == 0x00000000FFBB4400,
        "TCR value": TCR == 0x0000000080823518,
        "TTBR0 4 KiB aligned": (TTBR0 & 0xFFF) == 0,
        "device descriptor": attr_byte(0) == 0x00,
        "uncached descriptor": attr_byte(1) == 0x44,
        "write-through descriptor": attr_byte(2) == 0xBB,
        "write-back descriptor": attr_byte(3) == 0xFF,
    }
    for name, passed in checks.items():
        print(f"{name}: {'PASS' if passed else 'FAIL'}")
    print(f"MAIR_EL2=0x{MAIR:016X}")
    print(f"TCR_EL2=0x{TCR:016X}")
    print(f"TTBR0_EL2=0x{TTBR0:016X}")
    print(f"CHECKS={sum(checks.values())}/{len(checks)}")
    return 0 if all(checks.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
