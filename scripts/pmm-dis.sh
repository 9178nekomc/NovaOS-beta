#!/usr/bin/env bash
cd "$(dirname "$0")/.."
objdump -d build/kernel.elf | grep -A80 '<pmm_init>:' | grep -E 'movabs|sub|cmp|lea' | head -20
echo "=== core.elf end symbol ==="
nm -n build/kernel-core.elf | tail -3
echo "=== core.elf _kernel_end ==="
nm -n build/kernel-core.elf | grep -E '_kernel_end'
