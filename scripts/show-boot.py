#!/usr/bin/env python3
data = open('build/boothd.ppm','rb').read()
parts = data.split(b'\n',3)
w,h = int(parts[1].split()[0]), int(parts[1].split()[1])
off = len(parts[0])+1+len(parts[1])+1+len(parts[2])+1
print(f'{w}x{h}')
for y in range(0, min(h,160), 2):
    line = []
    for x in range(0, min(w,720), 4):
        o = off + (y*w+x)*3
        r,g,b = data[o],data[o+1],data[o+2]
        line.append('#' if (r>30 or g>30 or b>30) else '.')
    print(f'{y:3d}:', ''.join(line))
