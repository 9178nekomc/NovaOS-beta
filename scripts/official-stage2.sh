#!/usr/bin/env bash
# 单次调用：官方盘生成 + stage2 区域读取 + 与 limine-bios.sys 内容比对
cd "$(dirname "$0")/.."
truncate -s 64M /tmp/official.img
python3 - <<'EOF'
import struct
img = bytearray(64 * 1024 * 1024)
part = struct.pack('<B3sB3sII', 0x80, b'\0\0\0', 0x83, b'\0\0\0', 2048, 129024)
img[446:462] = part
img[510:512] = b'\x55\xAA'
open('/tmp/official.img', 'wb').write(bytes(img))
EOF
./limine/limine bios-install /tmp/official.img 2>&1 | grep -E "Stage 2|installed|error" | head -3
echo "=== official sector 1 (stage2 A) ==="
dd if=/tmp/official.img bs=512 skip=1 count=1 2>/dev/null | xxd | head -2
echo "=== official sector 21 (stage2 B) ==="
dd if=/tmp/official.img bs=512 skip=21 count=1 2>/dev/null | xxd | head -2
echo "=== limine-bios.sys[512:1024] ==="
dd if=limine/limine-bios.sys bs=512 skip=1 count=1 2>/dev/null | xxd | head -2
echo "=== which offsets in limine-bios.sys match official sector1 bytes? ==="
python3 - <<'EOF'
data = open('limine/limine-bios.sys','rb').read()
disk = open('/tmp/official.img','rb').read()
s1 = disk[512:1024]
# try to find first 16 bytes of s1 anywhere in file
head = s1[:16]
pos = data.find(head)
print('sector1 head found at:', hex(pos) if pos >= 0 else 'NOT FOUND')
print('sector1 head:', s1[:16].hex(' '))
# find what IS at file offset 512 (what our installer writes)
print('file[512:528]:', data[512:528].hex(' '))
EOF
