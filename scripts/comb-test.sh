#!/usr/bin/env bash
# 组合测试：官方 hdd stages + 我们安装的 ext2 分区/文件
cd "$(dirname "$0")/.."
BUILD="build"
cp "$BUILD/install.img" /tmp/comb.img
./limine/limine bios-install --force-mbr /tmp/comb.img 2>&1 | grep -E "Stage 2|installed|error" | head -3
echo "=== comb MBR head ==="
dd if=/tmp/comb.img bs=512 count=1 2>/dev/null | xxd | head -2
echo "=== boot ==="
( sleep 25; echo "screendump $BUILD/comb.ppm"; sleep 1; echo quit ) | \
    timeout -k 5 60 qemu-system-x86_64 -M q35 -m 512M -smp 1 -no-reboot \
        -accel tcg,thread=multi \
        -boot order=c -net none \
        -device piix4-ide,id=ide0 \
        -drive "file=/tmp/comb.img,format=raw,if=none,id=disk0,cache=unsafe" \
        -device ide-hd,drive=disk0,bus=ide0.0 \
        -vnc 127.0.0.1:1 -serial file:"$BUILD/comb.log" -monitor stdio >/dev/null 2>&1
echo "=== serial ==="
grep -a '\[Nova\]' "$BUILD/comb.log" | head -8
echo "=== screen ==="
python3 scripts/limine-ocr3.py "$BUILD/comb.ppm" build/fonts/Lat15-VGA16.psf.gz 2>/dev/null | head -8
python3 scripts/ppm-analyze.py "$BUILD/comb.ppm" 2>/dev/null | head -8
