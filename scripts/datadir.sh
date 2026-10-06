#!/usr/bin/env bash
cd "$(dirname "$0")/.."
echo "=== datadir ==="
./limine/limine --print-datadir 2>&1
DATADIR=$(./limine/limine --print-datadir 2>&1 | tail -1)
echo "=== datadir contents ==="
ls -la "$DATADIR" 2>/dev/null | head -20
echo "=== find limine-bios-hdd ==="
find . -name "*hdd*" -o -name "*bios-hdd*" 2>/dev/null | grep -v build | head -10
echo "=== find in limine tree ==="
find limine -name "*.h" | xargs grep -ln "limine-bios-hdd" 2>/dev/null | head -3
ls -la limine/*.h limine/host/*.h 2>/dev/null | head -20
