#!/usr/bin/env bash
cd "$(dirname "$0")/.."
echo "=== kfree 入口 0xffffffff8000a670 ==="
objdump -d build/kernel.elf --start-address=0xffffffff8000a660 --stop-address=0xffffffff8000a6b0 | head -12
echo "=== rsi 指向的 .rodata 0xffffffff80077f20 ==="
objdump -s build/kernel.elf --start-address=0xffffffff80077f00 --stop-address=0xffffffff80077f60 2>/dev/null | head -6
echo "=== kmalloc_locked 0xa5a0-0xa5c0（上次 rip）==="
objdump -d build/kernel.elf --start-address=0xffffffff8000a5a0 --stop-address=0xffffffff8000a5c0 | head -8
echo "=== 0x77f20 属于哪个节 ==="
readelf -SW build/kernel.elf | grep -E 'rodata|\.data'
