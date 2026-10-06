#!/usr/bin/env python3
"""Raw-byte analysis of a QEMU screendump PPM."""
import sys

path = sys.argv[1]
data = open(path, 'rb').read()
# find PPM header end (after maxval line)
i = 0
lines = 0
while lines < 3:
    if data[i] == 0x0A:
        lines += 1
    i += 1
body = data[i:]
print(path, 'body', len(body), 'bytes')
w, h = 720, 400
if len(body) >= w * h * 3:
    nz = sum(1 for j in range(0, len(body), 3)
             if body[j] or body[j + 1] or body[j + 2])
    print('nonzero px:', nz)
    for y in range(h):
        row = body[y * w * 3:(y + 1) * w * 3]
        c = sum(1 for j in range(0, len(row), 3)
                if row[j] or row[j + 1] or row[j + 2])
        if c:
            print('row', y, ':', c, 'px')
else:
    print('unexpected size; dumping first 64 bytes of body:',
          body[:64].hex())
