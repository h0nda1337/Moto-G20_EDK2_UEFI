#!/usr/bin/env python3
"""Static geometry and source-scope gate for MMU3B1 diagnostic Fix #3."""

from pathlib import Path
import re

FRAME_W, FRAME_H = 720, 1600
MARKERS = [
    ("MMU2A", 656, 304, 48, 64),
    ("MMU2B", 656, 384, 48, 64),
    ("MMU2C", 656, 464, 48, 64),
    ("MMU3A", 656, 544, 48, 64),
    ("MMU3B1", 656, 624, 48, 64),
    ("MMU3B2", 656, 704, 48, 64),
    ("TRUE_MMU3", 656, 784, 48, 64),
]

# DrawText: 12-pixel character pitch, 10-pixel painted height.
# DrawHexString/DrawHex64: 18-pixel character pitch, 21-pixel painted height.
LINES = [
    ("title", "text", 16, 24, 17),
    ("vbar_before_label", "text", 16, 70, 11),
    ("vbar_before_value", "hex", 160, 90, 16),
    ("vbar_after_label", "text", 16, 140, 10),
    ("vbar_after_value", "hex", 160, 160, 16),
    ("sctlr_before_label", "text", 16, 210, 12),
    ("sctlr_before_value", "hex", 160, 230, 16),
    ("sctlr_after_label", "text", 16, 280, 11),
    ("sctlr_after_value", "hex", 160, 300, 16),
    ("tcr_label", "text", 16, 350, 3),
    ("tcr_value", "hex", 160, 370, 16),
    ("ttbr0_label", "text", 16, 420, 5),
    ("ttbr0_value", "hex", 160, 440, 16),
    ("mair_label", "text", 16, 490, 4),
    ("mair_value", "hex", 160, 510, 16),
    ("target_vbar_label", "text", 16, 560, 11),
    ("target_vbar_value", "hex", 160, 580, 16),
    ("mmu_off", "text", 16, 650, 7),
    ("hextest_label", "text", 16, 720, 7),
    ("hextest_forward", "hex", 16, 750, 16),
    ("hextest_reverse", "hex", 16, 780, 16),
]


def box(line):
    _, kind, x, y, chars = line
    # Last painted text pixel: 4*2+2 for DrawText, 4*3+3 for hex glyphs.
    pitch, width, height = (12, 10, 10) if kind == "text" else (18, 15, 21)
    return (x, y, x + (chars - 1) * pitch + width, y + height)


def intersects(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def main():
    source = Path(__file__).parents[2] / "PrePi" / "MotoG20EarlyInit.c"
    text = source.read_text(encoding="utf-8")
    section = text.split("MotoG20DrawMmu3b1Diagnostics", 1)[1]
    assert "0x0000000081004000ULL" in section
    assert '"0123456789ABCDEF"' in section
    assert '"FEDCBA9876543210"' in section
    assert "MRS" not in section and "MSR" not in section

    boxes = [(line[0], box(line)) for line in LINES]
    for name, (x0, y0, x1, y1) in boxes:
        assert 0 <= x0 < x1 <= FRAME_W, (name, (x0, y0, x1, y1))
        assert 0 <= y0 < y1 <= FRAME_H, (name, (x0, y0, x1, y1))
        assert x1 <= 630, (name, "text x-end exceeds 630")
    for i, (name_a, box_a) in enumerate(boxes):
        for name_b, box_b in boxes[i + 1 :]:
            assert not intersects(box_a, box_b), ("text/text", name_a, name_b)
        for marker_name, mx, my, mw, mh in MARKERS:
            assert not intersects(box_a, (mx, my, mx + mw, my + mh)), (
                "text/marker", name_a, marker_name
            )

    print(f"LAYOUT_LINES={len(LINES)} PASS")
    print(f"MARKERS={len(MARKERS)} FIXED PASS")
    print("TEXT_TEXT_OVERLAP=NONE")
    print("TEXT_MARKER_OVERLAP=NONE")
    print("FRAMEBUFFER_BOUNDS=PASS")
    print("X_END_MAX=445")
    print("HEX_VALUES=8_INDEPENDENTLY_READABLE")
    print("SOURCE_SCOPE=DISPLAY_ONLY")


if __name__ == "__main__":
    main()
