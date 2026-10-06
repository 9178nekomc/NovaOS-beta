#!/usr/bin/env bash
# 安装崩溃后 dump 页表，检查 0xffffffff82e00000 的映射
cd "$(dirname "$0")/.."
BUILD="build"
ISO="$BUILD/nova.iso"
OVMF_CODE="${OVMF_CODE:-/usr/share/ovmf/OVMF_CODE.fd}"
[ -f "$OVMF_CODE" ] || OVMF_CODE="/usr/share/ovmf/OVMF.fd"
rm -f /tmp/hd512.img
truncate -s 512M /tmp/hd512.img
CMDS="sleep 40
echo 'sendkey i'; sleep 2
echo 'sendkey n'; sleep 2
echo 'sendkey s'; sleep 2
echo 'sendkey t'; sleep 2
echo 'sendkey a'; sleep 2
echo 'sendkey l'; sleep 2
echo 'sendkey l'; sleep 2
echo 'sendkey spc'; sleep 2
echo 'sendkey 0'; sleep 2
echo 'sendkey spc'; sleep 2
echo 'sendkey y'; sleep 2
echo 'sendkey e'; sleep 2
echo 'sendkey s'; sleep 2
echo 'sendkey ret'
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do
    echo 'screendump build/inst-mon.ppm'; sleep 6
done
echo 'info registers'
echo 'info mem'
echo 'quit'"
timeout -k 5 500 bash -c "( $CMDS ) | \"$QEMU\" -M q35 -m 512M -smp 1 -no-reboot \
    -accel tcg,thread=multi \
    -cdrom \"$ISO\" -bios \"$OVMF_CODE\" \
    -device piix4-ide,id=ide0 \
    -drive \"file=/tmp/hd512.img,format=raw,if=none,id=disk0,cache=unsafe\" \
    -device ide-hd,drive=disk0,bus=ide0.0 \
    -drive \"file=$BUILD/hd256.img,format=raw,if=none,id=disk1,cache=unsafe\" \
    -device ide-hd,drive=disk1,bus=ide0.1 \
    -vnc 127.0.0.1:0 -serial file:$BUILD/inst1.log -monitor stdio >$BUILD/pt-mon.log 2>&1" || true
echo "=== panic ==="
grep -a 'PANIC' "$BUILD/inst1.log" | head -3
echo "=== CR3 ==="
grep -A2 'CR3=' "$BUILD/pt-mon.log" | head -4
echo "=== info mem around 82e00000 ==="
grep -E '82[0-9a-f]|83[0-9a-f]|81[0-9a-f]' "$BUILD/pt-mon.log" | tail -20
