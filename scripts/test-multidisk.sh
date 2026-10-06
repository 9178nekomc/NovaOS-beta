#!/usr/bin/env bash
# scripts/test-multidisk.sh - 快速多盘验收（只跑 ATA 枚举 + D:/E: 挂载）
#
# 与 test-qemu.sh 相同的 QEMU 配置（piix4-ide：disk0 主通道主、
# disk1 主通道从），但不做 sendkey 注入与截图分析，启动后直接
# 看串口日志，适合多盘调试的快速迭代。
set -euo pipefail
cd "$(dirname "$0")/.."

QEMU="${QEMU:-qemu-system-x86_64}"
OVMF_CODE="${OVMF_CODE:-/usr/share/ovmf/OVMF_CODE.fd}"
[ -f "$OVMF_CODE" ] || OVMF_CODE="/usr/share/ovmf/OVMF.fd"
BUILD="build"
ISO="$BUILD/nova.iso"
DISK="$BUILD/test.img"
DISK2="$BUILD/test2.img"
SERIAL="/tmp/nova-serial.log"
MONITOR_LOG="$BUILD/qemu-monitor-multi.log"

rm -f "$SERIAL" "$MONITOR_LOG"
[ -f "$ISO" ] || make iso
bash scripts/prepare-ext2-disk.sh

# 内核网络阶段需要 host HTTP 仓库
HTTP_LOG="$BUILD/http-server.log"
rm -f "$HTTP_LOG"
python3 -m http.server 8080 --bind 127.0.0.1 --directory "$BUILD/repo" \
    >"$HTTP_LOG" 2>&1 &
HTTP_PID=$!
trap 'kill "$HTTP_PID" 2>/dev/null || true' EXIT

QEMU_ARGS=(-M q35 -m 512M -smp 2 -no-reboot -cdrom "$ISO" \
           -device piix4-ide,id=ide0 \
           -drive "file=$DISK,format=raw,if=none,id=disk0" \
           -device ide-hd,drive=disk0,bus=ide0.0 \
           -drive "file=$DISK2,format=raw,if=none,id=disk1" \
           -device ide-hd,drive=disk1,bus=ide0.1 \
           -netdev user,id=net0 \
           -device e1000,netdev=net0 \
           -vnc 127.0.0.1:0 \
           -d guest_errors,unimp \
           -bios "$OVMF_CODE")

# 注入：长输入换行回显验证（100 个 a 跨 80 列 -> unknown 回显应占 2 行）
KEYS="f o n t sp s e t sp w e n y u a n ret \
      a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a ret"
CMDS="sleep 30"
for k in $KEYS; do
    key="$k"
    [ "$k" = "sp" ] && key="spc"
    [ "$k" = "ret" ] && key="ret"
    [ "$k" = ":" ] && key="shift-semicolon"
    [ "$k" = "." ] && key="dot"
    CMDS="$CMDS
sleep 1
echo 'sendkey $key'"
done
CMDS="$CMDS
sleep 5
echo 'screendump $BUILD/shot-desc.ppm'
sleep 1
echo 'screendump $BUILD/shot-multi.ppm'
sleep 1
echo 'quit'"

echo "[multidisk] launching QEMU ..."
timeout -k 5 300 bash -c "( $CMDS ) \
    | \"$QEMU\" ${QEMU_ARGS[*]} -serial \"file:$SERIAL\" -monitor stdio \
        >\"$MONITOR_LOG\" 2>&1" || true
pkill -f "qemu-system-x86_64.*nova.iso" 2>/dev/null || true
sleep 1
cp -f "$SERIAL" "$BUILD/nova-serial.log" 2>/dev/null || true

echo "=== ATA enumeration ==="
grep -a "ATA:" "$SERIAL" || true
echo "=== ext2 / multi-disk ==="
grep -a "ext2:\|multi-disk\|mounted\|物理盘" "$SERIAL" || true
echo "=== interactive (disk/cd/cat/font) ==="
grep -a "dbg\]\|PANIC\|exception\|Second drive\|nova:E\|disk:\|cd:\|cat:\|font:" "$SERIAL" || true
echo "=== tail ==="
tail -8 "$SERIAL" 2>/dev/null || true
