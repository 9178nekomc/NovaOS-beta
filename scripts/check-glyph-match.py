#!/usr/bin/env python3
# scripts/check-glyph-match.py - 从截图找与参考字形最相似的块
#
# 用法: python3 scripts/check-glyph-match.py <shot.ppm> <ref-char.ppm> <char-utf8>
# 参考字形按非空边界自动裁剪（CJK 16x16 / Latin 3-14x16），在截图各行
# 带内滑窗扫描同尺寸块，逐像素二值化匹配，输出最高分。
import sys

def load_ppm(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = int(parts[1].split()[0]), int(parts[1].split()[1])
    off = len(parts[0]) + 1 + len(parts[1]) + 1 + len(parts[2]) + 1
    return w, h, off, data

def px(data, off, w, x, y):
    o = off + (y * w + x) * 3
    return data[o], data[o + 1], data[o + 2]

def glyph_binary(data, off, w, x0, y0, gw, gh):
    out = []
    for y in range(gh):
        row = []
        for x in range(gw):
            r, g, b = px(data, off, w, x0 + x, y0 + y)
            row.append(1 if (r > 60 or g > 60 or b > 60) else 0)
        out.append(row)
    return out

def match(a, b):
    hit = tot = 0
    for ya, yb in zip(a, b):
        for xa, xb in zip(ya, yb):
            tot += 1
            if xa == xb:
                hit += 1
    return hit / tot if tot else 0

def crop(data, off, w, x0, y0, gw, gh):
    """裁剪到非空边界，返回 (bx, by, bw, bh)"""
    minx, miny, maxx, maxy = gw, gh, -1, -1
    for y in range(gh):
        for x in range(gw):
            r, g, b = px(data, off, w, x0 + x, y0 + y)
            if r > 60 or g > 60 or b > 60:
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    if maxx < minx:
        return None
    return (minx, miny, maxx - minx + 1, maxy - miny + 1)

sw, sh, soff, sdata = load_ppm(sys.argv[1])
rw, rh, roff, rdata = load_ppm(sys.argv[2])

# 参考字形裁剪
rc = crop(rdata, roff, rw, 0, 0, rw, rh)
if rc is None:
    print(f"char {sys.argv[3]}: reference empty")
    sys.exit(1)
bx, by, bw, bh = rc
ref = glyph_binary(rdata, roff, rw, bx, by, bw, bh)

# 滑窗扫描截图（水平 2px 步长 + 垂直 ±4px 容忍 hinting 位移差异）
best = 0.0
best_pos = None
for y0 in range(0, sh - bh, 4):
    for x0 in range(0, sw - bw, 2):
        cand = glyph_binary(sdata, soff, sw, x0, y0, bw, bh)
        m = match(ref, cand)
        if m > best:
            best = m
            best_pos = (x0, y0)

print(f"char {sys.argv[3]}: best_match={best*100:.1f}% at px{best_pos} (ref {bw}x{bh})")
if best >= 0.78:
    print("GLYPH MATCH PASS")
    sys.exit(0)
print("GLYPH MATCH FAIL")
sys.exit(1)
