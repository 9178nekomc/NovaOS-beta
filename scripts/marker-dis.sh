#!/usr/bin/env bash
cd "$(dirname "$0")/.."
echo "=== marker refs in disasm ==="
objdump -d build/kernel.elf | grep -B3 -A3 'kprobe marker' | head -20
echo "=== string addresses ==="
strings -t x build/kernel.elf | grep 'kprobe marker'
echo "=== kernel.o disasm ==="
objdump -d build/kernel/kernel.o 2>/dev/null | grep -B3 -A3 'kprobe marker' | head -14
