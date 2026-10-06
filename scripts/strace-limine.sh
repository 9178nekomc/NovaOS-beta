#!/usr/bin/env bash
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
which strace >/dev/null 2>&1 && {
    strace -f -e trace=openat,read,close ./limine/limine bios-install /tmp/official.img 2>&1 | grep -iE "limine|sys|\.bin|\.img" | head -20
} || {
    echo "no strace; checking binary version and embedded strings"
    ./limine/limine --help 2>&1 | head -3
    strings ./limine/limine 2>/dev/null | grep -iE "limine-bios|\.sys" | head -5
}
