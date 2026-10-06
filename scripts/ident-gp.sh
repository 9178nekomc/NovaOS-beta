#!/usr/bin/env bash
cd "$(dirname "$0")/.."
echo "=== rip 0xffffffff8000a5b1 ==="
objdump -d build/kernel.elf --start-address=0xffffffff8000a580 --stop-address=0xffffffff8000a5e0 | head -14
echo "=== symbol near ==="
nm -n build/kernel.elf | awk '{if ($1 >= "ffffffff8000a500" && $1 <= "ffffffff8000a700") print}' | head -8
