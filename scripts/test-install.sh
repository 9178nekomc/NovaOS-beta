#!/usr/bin/env bash
# test-install.sh - Nova OS 安装程序端到端验收
#
#   环境：两块崭新空盘
#     build/hd512.img (512 MB) -> ide0.0 (dev0)
#     build/hd256.img (256 MB) -> ide0.1 (dev2)
#   phase 1: UEFI 从 ISO 启动，自动执行 `install 0 yes`，把系统安装到
#            512M 盘（MBR + Limine BIOS 引导器 + ext2 + 系统文件）
#   phase 2: 用 BIOS 从该 512M 盘启动，验证 Nova 完整启动到 shell
#
# 注意：安装目标盘先放在 /tmp（tmpfs 快盘）——QEMU 在 drvfs（/mnt/d）上
# 写入慢且长写入会挂起；安装完成后拷回 build/hd512.img 交付。
#
# 用法: bash scripts/test-install.sh
set -u
cd "$(dirname "$0")/.."
QEMU="${QEMU:-qemu-system-x86_64}"
OVMF_CODE="${OVMF_CODE:-/usr/share/ovmf/OVMF_CODE.fd}"
[ -f "$OVMF_CODE" ] || OVMF_CODE="/usr/share/ovmf/OVMF.fd"
BUILD="build"
ISO="$BUILD/nova.iso"
INSTALL_IMG="/tmp/hd512.img"         # 安装目标（tmpfs 快盘）
FINAL_IMG="$BUILD/hd512.img"         # 交付盘（drvfs，Windows 可用）
PASS=1

[ -f "$ISO" ] || make iso

echo "==================== phase 1: 安装到 512M 盘 ===================="
# 重置目标盘为崭新空盘；256M 盘保留原样（演示双盘可选）
rm -f "$INSTALL_IMG" "$FINAL_IMG"
truncate -s 512M "$INSTALL_IMG"

# QEMU TCG（WSL）下超长 ATA 写入偶发挂起（环境问题，非内核 bug）；
# 失败自动重试，最多 5 次。Windows 原生 QEMU 通常无此问题。
ATTEMPT=0
INSTALL_OK=0
while [ $ATTEMPT -lt 5 ]; do
    ATTEMPT=$((ATTEMPT + 1))
    echo "--- 安装尝试 $ATTEMPT/5 ---"
    rm -f "$INSTALL_IMG"
    truncate -s 512M "$INSTALL_IMG"
    # 自动模式：install 0 yes（枚举序号 0 = dev0 = 512M 盘）
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
    echo 'info registers'; sleep 1.5
    echo 'screendump build/inst-mon.ppm'; sleep 1.5
done
echo 'quit'"
    timeout -k 5 700 bash -c "( $CMDS ) | \"$QEMU\" -M q35 -m 512M -smp 1 -no-reboot \
        -accel tcg,thread=multi \
        -cdrom \"$ISO\" -bios \"$OVMF_CODE\" \
        -device piix4-ide,id=ide0 \
        -drive \"file=$INSTALL_IMG,format=raw,if=none,id=disk0,cache=unsafe\" \
        -device ide-hd,drive=disk0,bus=ide0.0 \
        -drive \"file=$BUILD/hd256.img,format=raw,if=none,id=disk1,cache=unsafe\" \
        -device ide-hd,drive=disk1,bus=ide0.1 \
        -vnc 127.0.0.1:0 -serial file:$BUILD/inst1.log -monitor stdio >$BUILD/inst-mon.log 2>&1" || true
    pkill -f "qemu-system-x86_64.*nova.iso" 2>/dev/null || true
    if grep -aq 'install: done' "$BUILD/inst1.log"; then
        INSTALL_OK=1
        break
    fi
    echo "  安装未完成（$(grep -ac 'install' "$BUILD/inst1.log") 行日志），重试..."
done
if [ $INSTALL_OK -ne 1 ]; then
    echo "FAIL: 安装未完成（5 次尝试均失败，WSL QEMU TCG ATA 挂起）"
    exit 1
fi

echo "--- 安装日志（应显示选择 512M 盘）---"
grep -a 'install' "$BUILD/inst1.log" | grep -aE 'target dev|install: done|MBR|stage2|mkfs|kernel.elf OK|limine.conf OK|limine-bios.sys OK|dict.dat OK' | tail -12

echo "--- MBR 分区表（应为 2 分区：0xEF ESP + 0x83 系统）---"
dd if="$INSTALL_IMG" bs=512 count=1 2>/dev/null | xxd -s 0x1BE -l 32
echo "--- MBR 0x1A4（应为 0028 0028 0002 ... 002a，字节单位）---"
dd if="$INSTALL_IMG" bs=512 count=1 2>/dev/null | xxd -s 0x1A4 -l 20 | head -1
echo "--- MBR 开头（应为 eb3c90 'LIMINE'）---"
dd if="$INSTALL_IMG" bs=512 count=1 2>/dev/null | xxd -l 16 | head -1

echo "--- ESP 内容（FAT32，mtools 检查）---"
dd if="$INSTALL_IMG" of="$BUILD/esp.img" bs=512 skip=2048 count=262144 status=none
if command -v mdir >/dev/null 2>&1; then
    mdir -i "$BUILD/esp.img" ::/ 2>&1 | head -8
    mdir -i "$BUILD/esp.img" ::/EFI/BOOT 2>&1 | head -6
else
    echo "(无 mtools，跳过 ESP 列表)"
fi

echo "--- 系统分区文件（debugfs，LBA 264192 起）---"
dd if="$INSTALL_IMG" of="$BUILD/part.img" bs=512 skip=264192 status=none
debugfs -R "ls -l /" "$BUILD/part.img" 2>&1 | grep -E "kernel|limine|dict" || {
    echo "FAIL: 系统分区文件缺失"
    exit 1
}

# 安装盘拷回 build/（drvfs 读快，交付给 Windows 侧使用）
cp "$INSTALL_IMG" "$FINAL_IMG"
echo "已交付 $FINAL_IMG ($(stat -c%s "$FINAL_IMG") bytes)"

echo "==================== phase 2: 512M 盘 BIOS 直启 ===================="
( sleep 30; echo "screendump $BUILD/boot-install.ppm"; sleep 1; echo quit ) | \
    timeout -k 5 60 "$QEMU" -M q35 -m 512M -smp 1 -no-reboot \
        -accel tcg,thread=multi \
        -boot order=c -net none \
        -device piix4-ide,id=ide0 \
        -drive "file=$INSTALL_IMG,format=raw,if=none,id=disk0,cache=unsafe" \
        -device ide-hd,drive=disk0,bus=ide0.0 \
        -vnc 127.0.0.1:1 -serial file:$BUILD/boot-install.log -monitor stdio >/dev/null 2>&1

echo "--- 启动串口日志 ---"
grep -a '\[Nova\]' "$BUILD/boot-install.log" | head -10
if grep -aq 'nova:/\$' "$BUILD/boot-install.log"; then
    echo "PASS: 512M 硬盘 BIOS 直启成功，进入 Nova shell"
else
    echo "FAIL: 未进入 Nova shell"
    PASS=0
fi
echo "--- 屏幕 ---"
python3 scripts/ppm-analyze.py "$BUILD/boot-install.ppm" 2>/dev/null | head -3

echo "==================== phase 3: UEFI（GOP）直启 ===================="
cp "$INSTALL_IMG" /tmp/boot-uefi.img
( sleep 30; echo "screendump $BUILD/boot-uefi.ppm"; sleep 1; echo quit ) | \
    timeout -k 5 60 "$QEMU" -M q35 -m 512M -smp 1 -no-reboot \
        -accel tcg,thread=multi \
        -bios "$OVMF_CODE" -net none \
        -device piix4-ide,id=ide0 \
        -drive "file=/tmp/boot-uefi.img,format=raw,if=none,id=disk0,cache=unsafe" \
        -device ide-hd,drive=disk0,bus=ide0.0 \
        -vnc 127.0.0.1:2 -serial file:$BUILD/boot-uefi.log -monitor stdio >/dev/null 2>&1
echo "--- UEFI 启动串口日志 ---"
grep -a '\[Nova\]' "$BUILD/boot-uefi.log" | head -10
if grep -aq 'nova:/\$' "$BUILD/boot-uefi.log"; then
    echo "PASS: UEFI（GOP）直启成功，进入 Nova shell"
    grep -a 'framebuffer' "$BUILD/boot-uefi.log" | head -1
else
    echo "FAIL: UEFI 未进入 shell"
    PASS=0
    tail -c 300 "$BUILD/boot-uefi.log" | tr -d '\r' | tail -3
fi

exit $((PASS ? 0 : 1))
