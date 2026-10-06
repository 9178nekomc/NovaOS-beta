#!/usr/bin/env python3
# scripts/psf1-to-psf2.py - PSF1 -> PSF2 转换（无 unicode 表时逐字符合成）
# 用法：python3 scripts/psf1-to-psf2.py <in.psf1> <out.psf2>
import struct
import sys

src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
assert data[0] == 0x36 and data[1] == 0x04, "not PSF1"
mode = data[2]
charsize = data[3]
numglyph = 512 if mode & 0x01 else 256
hastab = bool(mode & 0x02)
glyphbytes = numglyph * charsize
glyphs = data[4:4 + glyphbytes]
tab = data[4 + glyphbytes:] if hastab else b""

# PSF2 头
hdr = struct.pack("<IIIIIIII", 0x864ab572, 0, 32, 0x01, numglyph, charsize, charsize, 8)

# unicode 表：解析 PSF1 序列（终止 0xFF），逐字符合成 PSF2 表
out_tab = bytearray()
if hastab:
    i = 0
    for g in range(numglyph):
        seq = []
        while i < len(tab) and tab[i] != 0xFF:
            v = tab[i]
            if v < 0x80:
                seq.append(v)
            elif v < 0xE0:
                seq.append(((v & 0x1F) << 6) | (tab[i + 1] & 0x3F))
                i += 1
            elif v < 0xF0:
                seq.append(((v & 0x0F) << 12) | ((tab[i + 1] & 0x3F) << 6) | (tab[i + 2] & 0x3F))
                i += 2
            else:
                seq.append(((v & 0x07) << 18) | ((tab[i + 1] & 0x3F) << 12) | ((tab[i + 2] & 0x3F) << 6) | (tab[i + 3] & 0x3F))
                i += 3
            i += 1
        if not seq:
            seq = [0xFFFE]  # glyph 无映射 -> 私用区，避免被当作表终止
        out_tab.append(g)
        for u in seq:
            out_tab += struct.pack("<H", u)
        out_tab += b"\xff\xff"
else:
    for g in range(numglyph):
        out_tab.append(g)
        out_tab += struct.pack("<H", g)
        out_tab += b"\xff\xff"

open(dst, "wb").write(hdr + glyphs + bytes(out_tab))
print(f"[OK] {src} -> {dst} PSF2 {numglyph} glyphs {charsize}x{charsize}")
