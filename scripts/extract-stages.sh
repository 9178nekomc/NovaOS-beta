#!/usr/bin/env bash
# 提取官方 bios-install 写入的真实 stage1/stage2 数据（hdd 版）
cd "$(dirname "$0")/.."
mkdir -p build/official-stages
truncate -s 64M /tmp/official.img
python3 - <<'EOF'
import struct
img = bytearray(64 * 1024 * 1024)
part = struct.pack('<B3sB3sII', 0x80, b'\0\0\0', 0x83, b'\0\0\0', 2048, 129024)
img[446:462] = part
img[510:512] = b'\x55\xAA'
open('/tmp/official.img', 'wb').write(bytes(img))
EOF
./limine/limine bios-install /tmp/official.img >/dev/null 2>&1

dd if=/tmp/official.img of=build/official-stages/mbr.bin bs=512 count=1 status=none
dd if=/tmp/official.img of=build/official-stages/stage2a.bin bs=512 skip=1 count=20 status=none
dd if=/tmp/official.img of=build/official-stages/stage2b.bin bs=512 skip=21 count=20 status=none
dd if=/tmp/official.img of=build/official-stages/part0.bin bs=512 skip=41 count=8 status=none

echo "=== sizes ==="
ls -la build/official-stages/
echo "=== mbr head ==="
xxd build/official-stages/mbr.bin | head -2
echo "=== stage2a head ==="
xxd build/official-stages/stage2a.bin | head -2
echo "=== stage2a in limine-bios.sys? ==="
python3 - <<'EOF'
data = open('limine/limine-bios.sys','rb').read()
s2a = open('build/official-stages/stage2a.bin','rb').read()
pos = data.find(s2a[:64])
print('stage2a[0:64] found at:', hex(pos) if pos >= 0 else 'NOT FOUND')
EOF
echo "=== mbr.bin == limine-bios.sys[0:512]? ==="
head -c 512 limine/limine-bios.sys > /tmp/sys512.bin
cmp build/official-stages/mbr.bin /tmp/sys512.bin && echo IDENTICAL || echo DIFFERENT
cmp -l build/official-stages/mbr.bin /tmp/sys512.bin | head -5
