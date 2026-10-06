#!/usr/bin/env bash
cd "$(dirname "$0")/.."
echo "=== rip 0xffffffff80003a70 ==="
objdump -d build/kernel.elf --start-address=0xffffffff80003a30 --stop-address=0xffffffff80003aa0 | head -20
echo "=== symbol near ==="
nm -n build/kernel.elf | grep -E 'ffffffff8000[3-9a-f]' | head -8
echo "=== stack ret 0xffffffff8000fa6b ==="
objdump -d build/kernel.elf --start-address=0xffffffff8000fa30 --stop-address=0xffffffff8000fa90 | head -12
echo "=== symbol near 0xfa6b ==="
nm -n build/kernel.elf | awk '{if ($1 >= "ffffffff8000f800" && $1 <= "ffffffff80010000") print}' | head -8
