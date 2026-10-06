#!/usr/bin/env python3
# scripts/gen-font.py - 将 PSF2 字体转换为 C 头文件（Nova 内核用）
#
# 用法：python3 scripts/gen-font.py <in.psf2> <out.h>
#
# 输出：psf2font.h
#   - PSF2FONT_* 常量（宽/高/字形数/每字形字节数）
#   - psf2font_header[32]：原始 PSF2 头（供内核 font_init() 运行时校验）
#   - psf2font_glyphs[256][16]：字形位图（MSB 优先，逐行）
import struct
import sys

src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()

magic, ver, hsize, flags, ng, bpg, h, w = struct.unpack_from("<IIIIIIII", data, 0)
assert magic == 0x864ab572, "not PSF2"
assert hsize == 32
glyphs = data[32:32 + ng * bpg]
assert len(glyphs) == ng * bpg

lines = []
lines.append("/*")
lines.append(" * src/graphics/font/psf2font.h - 生成文件，请勿手改")
lines.append(" * 由 scripts/gen-font.py 从 %s 生成" % src)
lines.append(" * 源字体：Terminus 8x16 (PSF2, %d glyphs)" % ng)
lines.append(" * 格式：每字形 bpg=%d 字节，每行 1 字节（MSB 优先），共 %d 行" % (bpg, h))
lines.append(" */")
lines.append("#ifndef NOVA_PSF2FONT_H")
lines.append("#define NOVA_PSF2FONT_H")
lines.append("")
lines.append("#include <stdint.h>")
lines.append("")
lines.append("#define PSF2FONT_WIDTH          %d" % w)
lines.append("#define PSF2FONT_HEIGHT         %d" % h)
lines.append("#define PSF2FONT_NUMGLYPH       %d" % ng)
lines.append("#define PSF2FONT_BYTESPERGLYPH  %d" % bpg)
lines.append("")
lines.append("/* 原始 PSF2 头（32 字节），内核 font_init() 用于运行时校验 */")
lines.append("static const uint8_t psf2font_header[32] = {")
for i in range(0, 32, 12):
    n = min(12, 32 - i)
    chunk = ", ".join("0x%02x" % b for b in data[i:i + n])
    lines.append("    " + chunk + ("," if i + n < 32 else ""))
lines.append("};")
lines.append("")
lines.append("/* 字形位图：psf2font_glyphs[ch][row] */")
lines.append("static const uint8_t psf2font_glyphs[%d][%d] = {" % (ng, bpg))
for g in range(ng):
    row = ", ".join("0x%02x" % b for b in glyphs[g * bpg:(g + 1) * bpg])
    lines.append("    { %s }, /* glyph 0x%02x */" % (row, g))
lines.append("};")
lines.append("")
lines.append("#endif /* NOVA_PSF2FONT_H */")

with open(dst, "w", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
print("[OK] %s -> %s (%d glyphs, %dx%d)" % (src, dst, ng, w, h))
