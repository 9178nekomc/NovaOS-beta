#!/usr/bin/env python3
# 检查中文字体与图像库
import glob
import os

print("== CJK fonts ==")
patterns = [
    "/usr/share/fonts/**/*wqy*",
    "/usr/share/fonts/**/*microhei*",
    "/usr/share/fonts/**/*NotoSansCJK*",
    "/usr/share/fonts/**/*DroidSansFallback*",
    "/usr/share/fonts/**/*SourceHanSans*",
]
found = []
for p in patterns:
    found += glob.glob(p, recursive=True)
for f in found[:10]:
    print(" ", f, os.path.getsize(f))
if not found:
    print("  NONE")

print("== fonttools/PIL ==")
try:
    import fontTools
    print("  fontTools", fontTools.version)
except Exception as e:
    print("  fontTools missing:", e)
try:
    from PIL import Image, ImageFont
    print("  PIL ok")
except Exception as e:
    print("  PIL missing:", e)
