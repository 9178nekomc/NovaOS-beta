#!/usr/bin/env python3
# scripts/verify-echo.py - 自动定位并校验键盘回显行（不依赖固定行号）
#
# 用法：python3 scripts/verify-echo.py <shot.ppm>
# 扫描每个 16px 行带，寻找连续两行：
#   [A][1][+][2]  （caps+a、小键盘 1+2）
#   [a][Y][b][c]  （编辑：插入 X -> 退格删 X -> 插入 Y -> "aYbc"）
# 并校验第二行 col4 之后无字符（右移未越界）。
# 返回 0 通过；1 失败。
import re
import sys

src = open("src/graphics/font/psf2font.h").read()

def glyph(ch):
    pos = src.index("/* glyph 0x%02x */" % ord(ch))
    brace = src.rfind("{", 0, pos)
    end = src.rfind("}", brace, pos)
    body = src[brace + 1:end]
    return [int(v.strip(), 16) for v in body.split(",") if v.strip()]

def cell(data, w, off, y0, x0):
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

def match(data, w, off, y0, x0, ch):
    bits = glyph(ch)
    c = cell(data, w, off, y0, x0)
    return sum(1 for a, b2 in zip(c, bits) if a == b2)

data = open(sys.argv[1], "rb").read()
parts = data.split(b"\n", 3)
w, h = int(parts[1].split()[0]), int(parts[1].split()[1])
off = len(parts[0]) + 1 + len(parts[1]) + 1 + len(parts[2]) + 1

found = False
for band in range(20, h // 16):
    y0 = band * 16
    if (match(data, w, off, y0, 0, "A") >= 13 and
        match(data, w, off, y0, 8, "1") >= 13 and
        match(data, w, off, y0, 16, "+") >= 13 and
        match(data, w, off, y0, 24, "2") >= 13):
        y1 = y0 + 16
        if (match(data, w, off, y1, 0, "a") >= 13 and
            match(data, w, off, y1, 8, "Y") >= 13 and
            match(data, w, off, y1, 16, "b") >= 13 and
            match(data, w, off, y1, 24, "c") >= 13):
            # col4 (x=32) 必须无字符：允许 0 个亮像素（无光标）或
            # 128 个亮像素（反色光标块正停在此格）；混合 = 有字符 = 越界
            fgpx = 0
            for row in range(16):
                for col in range(8):
                    o = off + ((y1 + row) * w + (32 + col)) * 3
                    r, g, b2 = data[o], data[o + 1], data[o + 2]
                    if (r, g, b2) == (170, 170, 170):
                        fgpx += 1
            ok4 = (fgpx == 0 or fgpx == 128)
            print(f"echo found at y={y0}/{y1}, col4 fgpx={fgpx}")
            found = ok4
            break
if not found:
    print("FAIL: echo lines 'A1+2'/'aYbc' not found or right-move overran")
    sys.exit(1)
sys.exit(0)
