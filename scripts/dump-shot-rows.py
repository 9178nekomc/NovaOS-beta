#!/usr/bin/env python3
# scripts/dump-shot-rows.py <shot.ppm> - OCR 每行文本（诊断用）
import sys

src = open("src/graphics/font/psf2font.h").read()

def glyph(ch):
    pos = src.index("/* glyph 0x%02x */" % ord(ch))
    brace = src.rfind("{", 0, pos)
    end = src.rfind("}", brace, pos)
    body = src[brace + 1:end]
    return [int(v.strip(), 16) for v in body.split(",") if v.strip()]

data = open(sys.argv[1], "rb").read()
parts = data.split(b"\n", 3)
w, h = int(parts[1].split()[0]), int(parts[1].split()[1])
off = len(parts[0]) + 1 + len(parts[1]) + 1 + len(parts[2]) + 1

def cell(y0, x0):
    out = []
    for row in range(16):
        b = 0
        for col in range(8):
            o = off + ((y0 + row) * w + (x0 + col)) * 3
            r, g, b2 = data[o], data[o + 1], data[o + 2]
            if (r, g, b2) == (170, 170, 170):
                b |= 0x80 >> col
        out.append(b)
    return out

bits = {ch: glyph(ch) for ch in
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
        "0123456789 +-=()/.[]!$:_,;'\""}

for band in range(0, h // 22):
    row = []
    for cx in range(0, 80 * 8, 8):
        c = cell(band * 22, cx)
        if all(v == 0 for v in c):
            row.append(" ")
            continue
        best = None
        for ch, g in bits.items():
            m = sum(1 for a, b2 in zip(c, g) if a == b2)
            if best is None or m > best[0]:
                best = (m, ch)
        row.append(best[1] if best[0] >= 13 else "?")
    line = "".join(row).rstrip()
    if line:
        print("%02d|%s" % (band, line))
