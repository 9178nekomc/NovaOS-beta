#!/usr/bin/env python3
# scripts/psf2info.py - 打印 PSF2 字体文件头信息（开发辅助）
# 用法：python3 scripts/psf2info.py <file.psf>
import struct
import sys

data = open(sys.argv[1], "rb").read()
magic, ver, hsize, flags, ng, bpg, h, w = struct.unpack_from("<IIIIIIII", data, 0)
print(f"file: {sys.argv[1]} ({len(data)} bytes)")
print(f"magic: 0x{magic:08x} ({'OK' if magic == 0x864ab572 else 'NOT PSF2'})")
print(f"version: {ver}  headersize: {hsize}  flags: 0x{flags:x}")
print(f"numglyph: {ng}  bytesperglyph: {bpg}  size: {w}x{h}")
assert magic == 0x864ab572, "not a PSF2 font"
assert bpg == h * ((w + 7) // 8), "bytesperglyph mismatch"
