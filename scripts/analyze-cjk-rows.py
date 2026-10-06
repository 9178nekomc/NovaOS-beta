#!/usr/bin/env python3
# scripts/analyze-cjk-rows.py - 逐行分析截图 CJK 字形分布
import sys

data = open(sys.argv[1], "rb").read()
parts = data.split(b"\n", 3)
w, h = int(parts[1].split()[0]), int(parts[1].split()[1])
off = len(parts[0]) + 1 + len(parts[1]) + 1 + len(parts[2]) + 1

def px(x, y):
    o = off + (y * w + x) * 3
    return data[o], data[o + 1], data[o + 2]

def col_density(y0, col):
    n = 0
    x0 = col * 8
    for gy in range(0, 16, 2):
        for gx in range(0, 8, 2):
            r, g, b = px(x0 + gx, y0 + gy)
            if r > 40 or g > 40 or b > 40:
                n += 1
    return n

ncols = w // 8
for y0 in range(0, h - 16, 16):
    dens = [col_density(y0, c) for c in range(ncols)]
    cjk = 0
    starts = []
    c = 0
    while c < ncols - 1:
        if dens[c] >= 8 and dens[c + 1] >= 8:
            cjk += 1
            starts.append(c)
            c += 2
        else:
            c += 1
    if cjk > 0:
        print(f"row{y0//16:2d}: cjk={cjk} cols={starts[:24]}")
