#!/usr/bin/env bash
cd "$(dirname "$0")/.."
objdump -d build/kernel.elf | sed -n '/<pmm_init>:/,/ret/p' | grep -n -B3 -A3 'b721d0\|6b31d0\|0x1b7\|0x36b' | head -20
echo "=== search both constants in whole pmm_init ==="
objdump -d build/kernel.elf | sed -n '/<pmm_init>:/,/^$/p' | grep -E 'movabs.*(b721|6b31|00000001b|000000036)' | head -6
