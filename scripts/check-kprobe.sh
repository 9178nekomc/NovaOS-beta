#!/usr/bin/env bash
cd "$(dirname "$0")/.."
echo "=== kprobe in kernel.elf ==="
strings build/kernel.elf | grep -c 'kprobe'
echo "=== kprobe in ISO ==="
strings build/nova.iso | grep -c 'kprobe'
echo "=== kernel.c compiled? ==="
ls -la build/kernel/kernel.o src/core/kernel.c
echo "=== last boot lines before install ==="
grep -a 'kprobe\|vmm test\|kmalloc init' build/inst1.log | head -4
