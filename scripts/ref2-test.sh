#!/usr/bin/env bash
cd "$(dirname "$0")/.."
truncate -s 64M /tmp/ref2.img
python3 - <<'EOF'
import struct
img = bytearray(open('/tmp/ref2.img','rb').read())
mbr = bytearray(512)
part = struct.pack('<B3sB3sII', 0, b'\0\0\0', 0x83, b'\0\0\0', 2048, 129024)
mbr[446:462] = part
mbr[510:512] = b'\x55\xAA'
img[0:512] = mbr
open('/tmp/ref2.img','wb').write(bytes(img))
EOF
limine/limine bios-install /tmp/ref2.img 2>&1 | tail -1
( sleep 25; echo 'screendump /tmp/ref2.ppm'; sleep 1; echo quit ) | \
    timeout -k 5 60 qemu-system-x86_64 -M q35 -m 512M \
        -device piix4-ide,id=ide0 \
        -drive file=/tmp/ref2.img,format=raw,if=none,id=d0 \
        -device ide-hd,drive=d0,bus=ide0.0 \
        -vnc 127.0.0.1:0 -monitor stdio 2>&1 | tail -2
python3 - <<'EOF'
data = open('/tmp/ref2.ppm','rb').read()
parts = data.split(b'\n',3)
w,h = int(parts[1].split()[0]), int(parts[1].split()[1])
off = len(parts[0])+1+len(parts[1])+1+len(parts[2])+1
n = 0
for i in range(off, len(data), 3):
    if data[i] > 20 or data[i+1] > 20 or data[i+2] > 20:
        n += 1
print('ref2 screen nonblack pixels:', n)
EOF
