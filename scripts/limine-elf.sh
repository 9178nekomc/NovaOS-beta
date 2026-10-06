#!/usr/bin/env bash
cd /mnt/d/Users/Coffee/Desktop/nova/limine
echo "=== elf loading ==="
grep -rn "p_paddr\|ppaddr\|paddr" common/elf.c common/lib/elf.c 2>/dev/null | head -20
find . -name "elf.c" -not -path "./host/*" | head -3
echo "=== load_kernel ==="
grep -rn "load_kernel\|p_paddr" common/*.c 2>/dev/null | head -10
