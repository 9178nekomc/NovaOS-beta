#!/usr/bin/env bash
cd "$(dirname "$0")/.."
echo "=== ISO contains probe string? ==="
strings build/nova.iso | grep -c 'vmm: probe'
echo "=== timestamps ==="
ls -la build/nova.iso build/kernel.elf build/iso_root/kernel.elf 2>&1
echo "=== inst1.log boot start ==="
head -c 300 build/inst1.log | tr -d '\r' | head -4
