#!/usr/bin/env python3
"""Disposable host validation for the production MMU2B hex formatter/raster."""
from pathlib import Path

FONT = [
    [0x0E,0x11,0x13,0x15,0x19,0x11,0x0E], [0x04,0x0C,0x04,0x04,0x04,0x04,0x0E],
    [0x0E,0x11,0x01,0x02,0x04,0x08,0x1F], [0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E],
    [0x02,0x06,0x0A,0x12,0x1F,0x02,0x02], [0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E],
    [0x06,0x08,0x10,0x1E,0x11,0x11,0x0E], [0x1F,0x01,0x02,0x04,0x08,0x08,0x08],
    [0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E], [0x0E,0x11,0x11,0x0F,0x01,0x02,0x1C],
    [0x0E,0x11,0x11,0x1F,0x11,0x11,0x11], [0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E],
    [0x0F,0x10,0x10,0x10,0x10,0x10,0x0F], [0x1E,0x11,0x11,0x11,0x11,0x11,0x1E],
    [0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F], [0x1F,0x10,0x10,0x1E,0x10,0x10,0x10],
]
HEX = "0123456789ABCDEF"
SCALE, ADVANCE = 3, 18

def format64(value):
    return ''.join(HEX[(value >> ((15-i)*4)) & 0xF] for i in range(16))

def raster(text):
    pixels = set()
    for ci, char in enumerate(text):
        glyph = FONT[HEX.index(char)]
        for row, bits in enumerate(glyph):
            for col in range(5):
                if bits & (1 << (4-col)):
                    for dy in range(SCALE):
                        for dx in range(SCALE):
                            pixels.add((ci*ADVANCE+col*SCALE+dx, row*SCALE+dy))
    return pixels

def write_ppm(path):
    text = "0123456789ABCDEF\nFEDCBA9876543210"
    width, height = 16*ADVANCE, 2*7*SCALE+SCALE
    pix = set()
    for line, value in enumerate(text.splitlines()):
        for x, y in raster(value): pix.add((x, line*(7*SCALE+SCALE)+y))
    with path.open('w', encoding='ascii') as f:
        f.write(f"P3\n{width} {height}\n255\n")
        for y in range(height):
            f.write(' '.join('255 255 255' if (x,y) in pix else '0 0 0' for x in range(width))+'\n')

def main():
    values = [0, 0xFFFFFFFFFFFFFFFF, 0xFFBB4400, 0x80823518, 0x81981000]
    expected = ["0000000000000000", "FFFFFFFFFFFFFFFF", "00000000FFBB4400", "0000000080823518", "0000000081981000"]
    assert [format64(v) for v in values] == expected
    assert format64(0x0123456789ABCDEF) == "0123456789ABCDEF"
    assert format64(0xFEDCBA9876543210) == "FEDCBA9876543210"
    assert all(raster(c) for c in HEX)
    assert len({tuple(sorted(raster(c))) for c in HEX}) == 16
    out = Path(__file__).with_name('MMU2BHexRendererTest.ppm')
    write_ppm(out)
    print('VALUE_TESTS=7 PASS')
    print('GLYPH_TESTS=16_UNIQUE_NONEMPTY PASS')
    print('PATTERNS=0123456789ABCDEF,FEDCBA9876543210,00000000FFBB4400,0000000080823518,0000000081981000,FFFFFFFFFFFFFFFF,0000000000000000')
    print(f'PPM={out}')

if __name__ == '__main__':
    main()
