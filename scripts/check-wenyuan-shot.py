#!/usr/bin/env python3
# scripts/check-wenyuan-shot.py - 验证 wenyuan 模式截图中的 CJK 渲染
#
# 原理：CJK 字形 16px 宽 = 连续 2 个 8px 列。按 8px 列统计每列 16 行
# 内的非黑像素数，若存在连续两列（16px）均有 >=8/256 像素，则计为
# 一个 CJK 字形（灰度抗锯齿字形笔画较稀疏，阈值放低）。
# 至少 1 行含 >=2 个 CJK 且总数 >=4 即判定中文渲染成功。
import sys

data = open(sys.argv[1], "rb").read()
parts = data.split(b"\n", 3)
w, h = int(parts[1].split()[0]), int(parts[1].split()[1])
off = len(parts[0]) + 1 + len(parts[1]) + 1 + len(parts[2]) + 1

def px(x, y):
    o = off + (y * w + x) * 3
    return data[o], data[o + 1], data[o + 2]

def col_density(y0, col):
    """8px 列内非黑像素数（0..128，每 2px 采样）"""
    n = 0
    x0 = col * 8
    for gy in range(0, 16, 2):
        for gx in range(0, 8, 2):
            r, g, b = px(x0 + gx, y0 + gy)
            if r > 40 or g > 40 or b > 40:
                n += 1
    return n

rows_with_cjk = 0
total_cjk = 0
ncols = w // 8
for y0 in range(0, h - 16, 16):
    dens = [col_density(y0, c) for c in range(ncols)]
    # 找连续 2 列都 >=8 像素的 CJK 字形
    cjk_in_row = 0
    c = 0
    while c < ncols - 1:
        if dens[c] >= 8 and dens[c + 1] >= 8:
            cjk_in_row += 1
            c += 2
        else:
            c += 1
    if cjk_in_row >= 2:
        rows_with_cjk += 1
    total_cjk += cjk_in_row

print(f"rows_with_cjk={rows_with_cjk} total_cjk_glyphs={total_cjk}")
if rows_with_cjk >= 1 and total_cjk >= 4:
    print("WENYUAN_SHOT PASS (CJK 16px glyphs rendered)")
    sys.exit(0)
print("WENYUAN_SHOT FAIL")
sys.exit(1)
