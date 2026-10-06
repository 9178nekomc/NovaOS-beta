#!/usr/bin/env python3
# scripts/verify-glyph-at.py - 在截图指定位置对比参考字形
# 用法: python3 scripts/verify-glyph-at.py <shot.ppm> <row> <col8> <ref.ppm> <label>
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

def crop(data, off, w, x0, y0, gw, gh):
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

def match(a, b):
    hit = tot = 0
    for ya, yb in zip(a, b):
        for xa, xb in zip(ya, yb):
            tot += 1
            if xa == xb:
                hit += 1
    return hit / tot if tot else 0

sw, sh, soff, sdata = load_ppm(sys.argv[1])
row = int(sys.argv[2])
col8 = int(sys.argv[3])
rw, rh, roff, rdata = load_ppm(sys.argv[4])
label = sys.argv[5]

# 截图：取 col8*8 起 16px 宽（CJK 覆盖 2 列），裁剪非空边界
sc = crop(sdata, soff, sw, col8 * 8, row * 16, 16, 16)
rc = crop(rdata, roff, rw, 0, 0, rw, rh)
if sc is None or rc is None:
    print(f"{label}: empty region")
    sys.exit(1)

# 以参考大小为窗口，在截图裁剪块内滑动 ±2px 对齐
sbx, sby, sbw, sbh = sc
rbx, rby, rbw, rbh = rc
# 从截图块提取参考尺寸的窗口（尝试偏移）
best = 0
for dy in range(-2, 3):
    for dx in range(-2, 3):
        x0 = col8 * 8 + sbx + dx
        y0 = row * 16 + sby + dy
        if x0 < 0 or y0 < 0 or x0 + rbw > sw or y0 + rbh > sh:
            continue
        # 提取截图窗口并裁剪后对比参考裁剪
        win = []
        for y in range(rbh):
            rowp = []
            for x in range(rbw):
                r, g, b = px(sdata, soff, sw, x0 + x, y0 + y)
                rowp.append(1 if (r > 60 or g > 60 or b > 60) else 0)
            win.append(rowp)
        # 参考裁剪
        ref = []
        for y in range(rbh):
            rowp = []
            for x in range(rbw):
                r, g, b = px(rdata, roff, rw, rbx + x, rby + y)
                rowp.append(1 if (r > 60 or g > 60 or b > 60) else 0)
            ref.append(rowp)
        m = match(win, ref)
        if m > best:
            best = m

print(f"{label}: match={best*100:.1f}%")
if best >= 0.78:
    print("GLYPH AT PASS")
    sys.exit(0)
print("GLYPH AT FAIL")
sys.exit(1)
