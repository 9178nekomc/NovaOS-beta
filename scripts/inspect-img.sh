#!/usr/bin/env bash
cd "$(dirname "$0")/.."
echo "=== sector 1 (stage2 A) ==="
dd if=build/install.img bs=512 skip=1 count=1 2>/dev/null | xxd | head -3
echo "=== sector 21 (stage2 B) ==="
dd if=build/install.img bs=512 skip=21 count=1 2>/dev/null | xxd | head -3
echo "=== sector 2048 (partition start) ==="
dd if=build/install.img bs=512 skip=2048 count=1 2>/dev/null | xxd | head -3
echo "=== MBR stage1 first bytes ==="
dd if=build/install.img bs=512 count=1 2>/dev/null | xxd | head -2
echo "=== limine-bios.sys stage2 source ==="
dd if=limine/limine-bios.sys bs=512 skip=1 count=1 2>/dev/null | xxd | head -2
echo "=== MBR 0x1A4 fields ==="
dd if=build/install.img bs=512 count=1 2>/dev/null | xxd -s 0x1A4 -l 20
