#!/usr/bin/env python3
"""scripts/gen-cjk.py - Nova v2 CJK 16x16 bitmap font generator

Renders every needed CJK codepoint from the embedded WenYuan TTF into a
HZK16-format blob (magic "HZKH"), so the kernel renders CJK by pure
bitmap lookup + blit. This removes the runtime FreeType CJK path (the
historic out-of-bounds write root cause, pit 4.1).

Output format (little-endian):
  [0x00-0x03] magic = "HZKH"
  [0x04-0x07] count (uint32)
  [0x08..]    sorted uint32 codepoint array (count entries)
  [...]       bitmap array: each glyph = 32 bytes, row-major, 1 bit/pixel

USAGE: python3 scripts/gen-cjk.py [font.ttf] [out.bin]
Requires Pillow.
"""
import os
import struct
import sys
from PIL import Image, ImageFont

# CJK ranges to include (matches kernel wenyuan_wide_cp / is_wide_cp)
RANGES = [
    (0x2E80, 0x2EFF),   # CJK Radicals Supplement
    (0x3000, 0x303F),   # CJK Symbols and Punctuation
    (0x3400, 0x4DBF),   # CJK Extension A
    (0x4E00, 0x9FFF),   # CJK Unified Ideographs
    (0xF900, 0xFAFF),   # CJK Compatibility Ideographs
    (0xFE30, 0xFE4F),   # CJK Compatibility Forms
    (0xFF00, 0xFFEF),   # Fullwidth Forms
    (0x20000, 0x2FFFF), # CJK Extension B (may be sparse)
]


def render_glyph(font, cp, size=16):
    """Render one codepoint to 32 bytes (HZK16: 16 rows x 2 bytes)."""
    img = Image.new('1', (size, size), 0)
    try:
        mask = font.getmask(chr(cp))
        bb = font.getbbox(chr(cp))
        ox, oy = 0, 0
        if bb:
            ox = (size - (bb[2] - bb[0])) // 2
            oy = (size - (bb[3] - bb[1])) // 2
        for y in range(mask.size[1]):
            for x in range(mask.size[0]):
                if mask.getpixel((x, y)) > 0:
                    px, py = ox + x, oy + y
                    if 0 <= px < size and 0 <= py < size:
                        img.putpixel((px, py), 1)
    except (ValueError, OSError, IndexError):
        pass  # glyph not supported -> blank 32 bytes
    data = bytearray(32)
    for row in range(size):
        b0 = b1 = 0
        for col in range(8):
            if img.getpixel((col, row)):
                b0 |= (0x80 >> col)
            if img.getpixel((8 + col, row)):
                b1 |= (0x80 >> col)
        data[row * 2] = b0
        data[row * 2 + 1] = b1
    return data


def main():
    font_path = sys.argv[1] if len(sys.argv) > 1 else \
        'src/graphics/font/WenYuanSansSCVF.ttf'
    out_path = sys.argv[2] if len(sys.argv) > 2 else 'build/font-cjk.bin'

    if not os.path.exists(font_path):
        # fallback to legacy path (pre-migration)
        legacy = 'kernel/font/WenYuanSansSCVF.ttf'
        if os.path.exists(legacy):
            font_path = legacy
        else:
            print(f"[gen-cjk] ERROR: font not found: {font_path}")
            sys.exit(1)

    print(f"[gen-cjk] loading {font_path}")
    font = ImageFont.truetype(font_path, 16)

    cps = []
    for lo, hi in RANGES:
        step = 4 if (lo >= 0x20000) else 1
        for cp in range(lo, hi + 1, step):
            cps.append(cp)
    # de-dup + sort
    cps = sorted(set(cps))

    # filter to supported glyphs
    supported = []
    for cp in cps:
        try:
            mask = font.getmask(chr(cp))
            if mask.size[0] > 0 and mask.size[1] > 0:
                supported.append(cp)
        except Exception:
            pass
    print(f"[gen-cjk] {len(supported)} supported of {len(cps)} candidates")

    bitmaps = bytearray()
    for i, cp in enumerate(supported):
        if i % 2000 == 0 and i:
            print(f"[gen-cjk]   {i}/{len(supported)}...")
        bitmaps.extend(render_glyph(font, cp))

    os.makedirs(os.path.dirname(out_path) or '.', exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(b'HZKH')
        f.write(struct.pack('<I', len(supported)))
        for cp in supported:
            f.write(struct.pack('<I', cp))
        f.write(bitmaps)
    total = os.path.getsize(out_path)
    print(f"[gen-cjk] output {out_path}: {total:,} bytes, {len(supported)} glyphs")
    print("[gen-cjk] DONE")


if __name__ == '__main__':
    main()
