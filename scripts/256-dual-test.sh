#!/usr/bin/env bash
# 复现用户场景：双分区安装到 256M 盘（dev2）
cd "$(dirname "$0")/.."
make -j8 iso 2>&1 | grep -E 'error|OK\]' | tail -1
BUILD="build"
ISO="$BUILD/nova.iso"
OVMF_CODE="${OVMF_CODE:-/usr/share/ovmf/OVMF.fd}"
rm -f /tmp/hd256r.img
truncate -s 256M /tmp/hd256r.img
CMDS="sleep 40
echo 'sendkey i'; sleep 2
echo 'sendkey n'; sleep 2
echo 'sendkey s'; sleep 2
echo 'sendkey t'; sleep 2
echo 'sendkey a'; sleep 2
echo 'sendkey l'; sleep 2
echo 'sendkey l'; sleep 2
echo 'sendkey spc'; sleep 2
echo 'sendkey 1'; sleep 2
echo 'sendkey spc'; sleep 2
echo 'sendkey y'; sleep 2
echo 'sendkey e'; sleep 2
echo 'sendkey s'; sleep 2
echo 'sendkey ret'
for i in 1 2 3 4 5 6 7 8 9 10; do
    echo 'screendump build/inst-256r.ppm'; sleep 6
done
echo 'quit'"
timeout -k 5 700 bash -c "( $CMDS ) | qemu-system-x86_64 -M q35 -m 512M -smp 1 -no-reboot \
    -accel tcg,thread=multi \
    -cdrom \"$ISO\" -bios \"$OVMF_CODE\" \
    -device piix4-ide,id=ide0 \
    -drive \"file=$BUILD/hd512.img,format=raw,if=none,id=disk0,cache=unsafe\" \
    -device ide-hd,drive=disk0,bus=ide0.0 \
    -drive \"file=/tmp/hd256r.img,format=raw,if=none,id=disk1,cache=unsafe\" \
    -device ide-hd,drive=disk1,bus=ide0.1 \
    -vnc 127.0.0.1:0 -serial file:$BUILD/inst-256r.log -monitor stdio >/dev/null 2>&1" || true
echo "=== result ==="
grep -a 'install: done' "$BUILD/inst-256r.log" && echo "256MB DUAL INSTALL OK" || {
    echo "FAILED:"
    grep -a 'PANIC\|install' "$BUILD/inst-256r.log" | tail -6
}
