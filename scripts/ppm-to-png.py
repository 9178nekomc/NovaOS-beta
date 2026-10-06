#!/usr/bin/env python3
# scripts/ppm-to-png.py - 将 QEMU screendump 的 PPM 裁剪转 PNG（开发辅助）
# 用法：python3 scripts/ppm-to-png.py <in.ppm> <out.png> [x] [y] [w] [h]
import sys
from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
x, y = int(sys.argv[3]) if len(sys.argv) > 3 else 0, int(sys.argv[4]) if len(sys.argv) > 4 else 0
w = int(sys.argv[5]) if len(sys.argv) > 5 else 480
h = int(sys.argv[6]) if len(sys.argv) > 6 else 160

im = Image.open(src).convert("RGB")
im = im.crop((x, y, x + w, y + h)).resize((w * 3, h * 3))
im.save(dst)
print(f"saved {dst} ({im.size[0]}x{im.size[1]})")
