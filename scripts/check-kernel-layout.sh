#!/usr/bin/env bash
cd "$(dirname "$0")/.."
echo "=== _kernel_end ==="
nm -n build/kernel.elf | grep -E '_kernel_end|_kernel_start|sysimg_kernel_start|sysimg_kernel_end' | head
echo "=== segments ==="
readelf -lW build/kernel.elf | grep -E 'LOAD|Type' | head -8
echo "=== sections ==="
readelf -SW build/kernel.elf | grep -E 'rodata|text|data|bss|sysimg' | head -12
echo "=== size ==="
stat -c%s build/kernel.elf
