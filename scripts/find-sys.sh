#!/usr/bin/env bash
cd "$(dirname "$0")/.."
echo "=== limine-bios.sys copies in tree ==="
find . -name "limine-bios.sys" -exec ls -la {} \; 2>/dev/null
echo "=== root copies ==="
ls -la limine-bios.sys 2>/dev/null
echo "=== md5 all copies ==="
find . -name "limine-bios.sys" -exec md5sum {} \; 2>/dev/null
echo "=== host/limine.c: how it locates limine-bios.sys ==="
grep -n "limine-bios.sys\|bootloader_file\|fopen" host/limine.c | head -10
