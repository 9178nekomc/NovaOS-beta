#!/usr/bin/env python3
"""Convert a PPM screen dump to PNG (optionally upscaled)."""
import sys
from PIL import Image

src = sys.argv[1]
dst = sys.argv[2]
scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1

im = Image.open(src).convert('RGB')
print(im.size, im.mode)
if scale != 1:
    im = im.resize((im.size[0] * scale, im.size[1] * scale), Image.NEAREST)
im.save(dst)
print('saved', dst)
