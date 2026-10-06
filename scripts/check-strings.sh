#!/usr/bin/env bash
cd "$(dirname "$0")/.."
echo "=== probe string in kernel.elf? ==="
strings build/kernel.elf | grep -c 'vmm: probe'
echo "=== vmm.o timestamp ==="
ls -la build/src/core/mm/vmm.o src/core/mm/vmm.c
echo "=== idt walk string ==="
strings build/kernel.elf | grep -c 'walk: PML4'
echo "=== panic walk in idt.o ==="
ls -la build/src/core/idt/idt.o src/core/idt/idt.c
