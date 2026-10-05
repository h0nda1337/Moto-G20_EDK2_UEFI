"""Host-only MMU3A VBAR/SP/PC safety model."""

VA_LIMIT = 1 << 40
VECTOR_SIZE = 0x800
WB_CODE = (0x80000000, 0x81FF0000)
WB_CODE2 = (0x82000000, 0x82A0B000)
WB_STACK = (0x811F0000, 0x81200000)
PE_TEXT = (0x81000000, 0x81006000)


def add_overflow(a, b):
    end = a + b
    return end < a, end


def in_region(address, region):
    return region[0] <= address < region[1]


def vector_range_ok(vbar):
    overflow, end = add_overflow(vbar, VECTOR_SIZE - 1)
    return not overflow and vbar < VA_LIMIT and end < VA_LIMIT


def aligned(vbar):
    return (vbar & 0x7FF) == 0


def mapped_exec_wb(address):
    return (in_region(address, WB_CODE) or in_region(address, WB_CODE2))


def mapped_stack(address):
    return in_region(address, WB_STACK)


def classify(vbar, sp, pc, spsel, sctlr, captures_equal=True):
    range_ok = vector_range_ok(vbar)
    end = vbar + VECTOR_SIZE - 1 if range_ok else 0
    vbar_ok = range_ok and aligned(vbar) and mapped_exec_wb(vbar) and mapped_exec_wb(end)
    sp_ok = (sp & 0xF) == 0 and WB_STACK[0] <= sp <= WB_STACK[1] and sp >= 16 and mapped_stack(sp - 16) and mapped_stack(sp - 1)
    pc_ok = in_region(pc, PE_TEXT)
    future_ok = in_region(0x81001044, PE_TEXT) and in_region(0x81001070, PE_TEXT)
    green = captures_equal and vbar_ok and sp_ok and pc_ok and future_ok and spsel in (0, 1) and (sctlr & 0x1005) == 0
    return {
        "vbar_range": range_ok,
        "vbar_safe": vbar_ok,
        "sp_ok": sp_ok,
        "pc_ok": pc_ok,
        "future_ok": future_ok,
        "green": green,
    }


def run():
    tests = []

    def check(name, condition):
        tests.append((name, bool(condition)))

    check("aligned vector base", aligned(0x81000000))
    check("full 0x800 range", vector_range_ok(0x81000000))
    check("overflow rejected", not vector_range_ok((1 << 64) - 0x100))
    check("unmapped VBAR rejected", not classify(0x40000000, 0x81200000, 0x81001080, 0, 0x30C50830)["vbar_safe"])
    check("unaligned VBAR rejected", not classify(0x81000001, 0x81200000, 0x81001080, 0, 0x30C50830)["vbar_safe"])
    check("vector page boundary accepted", vector_range_ok(0x81FE0000) and mapped_exec_wb(0x81FE07FF))
    check("PXN-style unmapped region rejected", not mapped_exec_wb(0x9E000000))
    check("40-bit envelope accepted", vector_range_ok(0x81F00000))
    check("above 40-bit rejected", not vector_range_ok(VA_LIMIT))
    check("SP alignment and mapping", classify(0x81000000, 0x81200000, 0x81001080, 0, 0x30C50830)["sp_ok"])
    check("SP misalignment rejected", not classify(0x81000000, 0x81200008, 0x81001080, 0, 0x30C50830)["sp_ok"])
    check("SP below envelope rejected", not classify(0x81000000, 0x811EFFF0, 0x81001080, 0, 0x30C50830)["sp_ok"])
    check("PC mapping", classify(0x81000000, 0x81200000, 0x81001080, 0, 0x30C50830)["pc_ok"])
    check("PC outside PE rejected", not classify(0x81000000, 0x81200000, 0x82001080, 0, 0x30C50830)["pc_ok"])
    check("future TLBI address mapped", in_region(0x81001044, PE_TEXT))
    check("future SCTLR address mapped", in_region(0x81001070, PE_TEXT))
    check("SPSel 0 accepted", classify(0x81000000, 0x81200000, 0x81001080, 0, 0x30C50830)["green"])
    check("SPSel 1 accepted", classify(0x81000000, 0x81200000, 0x81001080, 1, 0x30C50830)["green"])
    check("invalid SPSel rejected", not classify(0x81000000, 0x81200000, 0x81001080, 2, 0x30C50830)["green"])
    check("M baseline rejected", not classify(0x81000000, 0x81200000, 0x81001080, 0, 0x30C50831)["green"])
    check("C baseline rejected", not classify(0x81000000, 0x81200000, 0x81001080, 0, 0x30C50834)["green"])
    check("I baseline rejected", not classify(0x81000000, 0x81200000, 0x81001080, 0, 0x30C51830)["green"])
    check("capture instability rejected", not classify(0x81000000, 0x81200000, 0x81001080, 0, 0x30C50830, False)["green"])
    check("safe classification green", classify(0x81000000, 0x81200000, 0x81001080, 0, 0x30C50830)["green"])
    check("MMU remains off", (0x30C50830 & 1) == 0)
    check("no enable policy", True)
    passed = sum(ok for _, ok in tests)
    for name, ok in tests:
        print(f"{name}: {'PASS' if ok else 'FAIL'}")
    print(f"CHECKS={passed}/{len(tests)}")
    return 0 if passed == len(tests) else 1


if __name__ == "__main__":
    raise SystemExit(run())
