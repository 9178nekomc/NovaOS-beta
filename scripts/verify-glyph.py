#!/usr/bin/env python3
# scripts/verify-glyph.py - 校验截图中指定位置的字形与字体数据一致
# 用法：python3 scripts/verify-glyph.py <shot.ppm> <ch> <x_px> <y_px> [fg_r] [fg_g] [fg_b] [inv]
#   inv: 附加接受反色渲染（光标块可能正停在该格）
import re
import sys

src = open("src/graphics/font/psf2font.h").read()

def glyph(ch):
    pat = r"glyph 0x%02x \*/" % ord(ch)
    m = re.search(pat, src)
    line = src[:m.start()].rfind("{")
    body = src[line + 1:m.start()]
    body = body.split("/*")[0].replace("{", "").replace("}", "").strip()
    return [int(v, 16) for v in body.split(",") if v.strip()]

data = open(sys.argv[1], "rb").read()
parts = data.split(b"\n", 3)
w, h = int(parts[1].split()[0]), int(parts[1].split()[1])
off = len(parts[0]) + 1 + len(parts[1]) + 1 + len(parts[2]) + 1

ch = sys.argv[2]
gx, gy = int(sys.argv[3]), int(sys.argv[4])
if len(sys.argv) > 5 and sys.argv[5] != "inv":
    fg = (int(sys.argv[5]), int(sys.argv[6]), int(sys.argv[7]))
else:
    fg = (170, 170, 170)  # TERM_COLOR_LIGHT_GRAY
accept_inv = "inv" in sys.argv[5:]

bits = glyph(ch)
assert len(bits) == 16, f"glyph rows: {len(bits)}"

def check(invert):
    match = total = 0
    for row in range(16):
        for col in range(8):
            o = off + ((gy + row) * w + (gx + col)) * 3
            r, g, b = data[o], data[o + 1], data[o + 2]
            bit = bits[row] & (0x80 >> col)
            if invert:
                expect = (0, 0, 0) if bit else fg
            else:
                expect = fg if bit else (0, 0, 0)
            total += 1
            if (r, g, b) == expect:
                match += 1
    return match

m_normal = check(False)
m_inv = check(True) if accept_inv else 0
best = max(m_normal, m_inv)
mode = "inverse" if accept_inv and m_inv > m_normal else "normal"
print(f"glyph '{ch}' at ({gx},{gy}): {best}/{128} pixels match ({mode})")
sys.exit(0 if best > 128 * 0.8 else 1)
