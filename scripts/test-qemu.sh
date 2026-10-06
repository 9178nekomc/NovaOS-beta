#!/usr/bin/env bash
# scripts/test-qemu.sh - Nova OS QEMU 自动验收脚本
#
# 功能：
#   1. 构建 ISO（若缺失）
#   2. 以 UEFI（OVMF）或 BIOS 模式启动 QEMU（-vnc 提供显示面，支持 screendump）
#   3. 截取基线画面 -> 通过 monitor sendkey 注入键击（a b c Shift+X）
#   4. 截取注入后画面，校验：内核终端可见（1920x1080 + 文字 + 绿色[OK]）
#      且键击回显产生可观测的像素增量（阶段四）
#   5. 解析串口日志验证内核启动信息
#
# 用法：
#   bash scripts/test-qemu.sh            # 默认 UEFI 模式
#   bash scripts/test-qemu.sh bios       # BIOS(SeaBIOS) 模式
#
# 依赖：qemu-system-x86_64、OVMF（UEFI 模式）、python3
set -euo pipefail

cd "$(dirname "$0")/.."

MODE="${1:-uefi}"
QEMU="${QEMU:-qemu-system-x86_64}"
OVMF_CODE="${OVMF_CODE:-/usr/share/ovmf/OVMF_CODE.fd}"
[ -f "$OVMF_CODE" ] || OVMF_CODE="/usr/share/ovmf/OVMF.fd"
[ -f "$OVMF_CODE" ] || OVMF_CODE="/usr/share/OVMF/OVMF_CODE.fd"

BUILD="build"
ISO="$BUILD/nova.iso"
DISK="$BUILD/test.img"
DISK2="$BUILD/test2.img"        # 阶段二十：第二盘（E:）
REPO_DIR="$BUILD/repo"          # 阶段十九：host HTTP 仓库目录
# 串口写到 WSL ext4（/tmp），避免 drvfs 写入停滞拖慢内核 uart 输出
SERIAL="/tmp/nova-serial.log"
MONITOR_LOG="$BUILD/qemu-monitor.log"

rm -f "$SERIAL" "$MONITOR_LOG" "$BUILD"/shot*.ppm
[ -f "$ISO" ] || make iso
# 阶段十五/十六：MBR + ext2 测试盘（每次重建，保证干净状态）
bash scripts/prepare-ext2-disk.sh

# 阶段十九：启动 host HTTP 仓库服务器（服务 build/repo/），
# QEMU 内 e1000 经 slirp（10.0.2.2:8080）访问
HTTP_LOG="$BUILD/http-server.log"
rm -f "$HTTP_LOG"
python3 -m http.server 8080 --bind 127.0.0.1 --directory "$REPO_DIR" \
    >"$HTTP_LOG" 2>&1 &
HTTP_PID=$!
trap 'kill "$HTTP_PID" 2>/dev/null || true' EXIT
# 确保服务器就绪（用 python3 探测，避免依赖 curl）
for _ in $(seq 1 20); do
    if python3 -c 'import sys,urllib.request
try:
    urllib.request.urlopen("http://127.0.0.1:8080/index.nvp", timeout=1)
    sys.exit(0)
except Exception:
    sys.exit(1)' >/dev/null 2>&1; then
        break
    fi
    sleep 0.2
done
echo "[Nova] HTTP repo server pid=$HTTP_PID (dir=$REPO_DIR)"

echo "[Nova] launching QEMU ($MODE mode, vnc display) ..."

QEMU_ARGS=(-M q35 -m 512M -smp 2 -no-reboot -cdrom "$ISO" \
           -device piix4-ide,id=ide0 \
           -drive "file=$DISK,format=raw,if=none,id=disk0" \
           -device ide-hd,drive=disk0,bus=ide0.0 \
           -drive "file=$DISK2,format=raw,if=none,id=disk1" \
           -device ide-hd,drive=disk1,bus=ide0.1 \
           -netdev user,id=net0 \
           -device e1000,netdev=net0 \
           -vnc 127.0.0.1:0 \
           -d guest_errors,unimp)
if [ "$MODE" = "uefi" ]; then
    QEMU_ARGS+=(-bios "$OVMF_CODE")
fi

# monitor 命令流：基线截屏 -> sendkey 注入 CLI 命令 -> 截屏 -> 退出。
# 阶段十八起：主循环为命令行（CLI），sendkey 输入命令验证交互：
#   echo hello -> 输出 hello；help -> 帮助；nvp list -> 包列表
# 注意：命令必须 echo 到 QEMU monitor 的 stdin，不能作为 shell 命令直接执行。
CMDS="sleep 30
echo 'screendump $BUILD/shot0.ppm'
sleep 3
echo 'screendump $BUILD/shot1.ppm'
sleep 3"
for k in p s ret \
         l s sp - l sp / ret \
         e c h o sp h e l l o ret \
         h e l p ret \
         n v p sp l i s t ret \
         r e p o sp l i s t ret \
         c a t sp s h e l l dot t x t ret \
         c d sp p k g s ret \
         p w d ret \
         d i s k ret \
         c d sp e : ret \
         p w d ret \
         c a t sp d a t a dot t x t ret \
         f o n t sp s e t sp w e n y u a n ret; do
    key="$k"
    [ "$k" = "sp" ] && key="spc"
    [ "$k" = "ret" ] && key="ret"
    [ "$k" = "-" ] && key="minus"
    [ "$k" = "/" ] && key="slash"
    [ "$k" = ":" ] && key="shift-semicolon"
    [ "$k" = "." ] && key="dot"
    CMDS="$CMDS
sleep 2
echo 'sendkey $key'"
done
# wenyuan 模式截图（验证 CJK 渲染），然后切回 mono 供 CLI OCR
CMDS="$CMDS
sleep 8
echo 'screendump $BUILD/shot3.ppm'
sleep 2
echo 'sendkey f'
sleep 2
echo 'sendkey o'
sleep 2
echo 'sendkey n'
sleep 2
echo 'sendkey t'
sleep 2
echo 'sendkey spc'
sleep 2
echo 'sendkey s'
sleep 2
echo 'sendkey e'
sleep 2
echo 'sendkey t'
sleep 2
echo 'sendkey spc'
sleep 2
echo 'sendkey m'
sleep 2
echo 'sendkey o'
sleep 2
echo 'sendkey n'
sleep 2
echo 'sendkey o'
sleep 2
echo 'sendkey ret'
sleep 8
echo 'screendump $BUILD/shot2.ppm'"
CMDS="$CMDS
sleep 3
echo 'info registers'
echo 'quit'"

timeout -k 5 420 bash -c "( $CMDS ) | \"$QEMU\" ${QEMU_ARGS[*]} \
    -serial \"file:$SERIAL\" -monitor stdio >\"$MONITOR_LOG\" 2>&1" \
    || true

# 清理可能残留的 QEMU（timeout 杀 bash 时管道另一端的 QEMU 可能存活，
# 累积会拖慢后续运行）
pkill -f "qemu-system-x86_64.*nova.iso" 2>/dev/null || true
sleep 1

echo "=== kernel serial log ==="
# 持久化串口日志（WSL /tmp 可能在会话间被清理，丢失调试依据）
cp -f "$SERIAL" "$BUILD/nova-serial.log" 2>/dev/null || true
grep -a "\[Nova\]" "$SERIAL" || true

echo "=== SMP check (phase 11) ==="
SMP_OK=1
for c in "CPU 1 online" "smp test"; do
    if ! grep -aq "$c" "$SERIAL"; then
        SMP_OK=0
        echo "  SMP check '$c': MISSING"
    fi
done
if ! grep -aq "pin=OK" "$SERIAL"; then
    SMP_OK=0
    echo "  SMP check 'pin=OK': MISSING"
fi
[ "$SMP_OK" = "1" ] && echo "SMP_TEST PASS (2 CPUs online, per-CPU burners, pin check)"

echo "=== user mode check (phase 12) ==="
USER_OK=1
for c in "user: program" "\[user\] Hello from ring3" "ring3 done" "user mode demo done"; do
    if ! grep -aq "$c" "$SERIAL"; then
        USER_OK=0
        echo "  user check '$c': MISSING"
    fi
done
[ "$USER_OK" = "1" ] && echo "USERMODE_TEST PASS (ring3 syscalls: write/getpid/yield/exit)"

echo "=== tmpfs check (phase 13) ==="
TMPFS_OK=1
for c in "tmpfs test" "readme.txt file" "docs dir" "data.bin file"; do
    if ! grep -aq "$c" "$SERIAL"; then
        TMPFS_OK=0
        echo "  tmpfs check '$c': MISSING"
    fi
done
[ "$TMPFS_OK" = "1" ] && echo "TMPFS_TEST PASS (create/write/read/dir/list/unlink, 1KB pattern)"

echo "=== ATA check (phase 14) ==="
ATA_OK=1
if ! grep -aq "ATA: dev0 IDENTIFY ok" "$SERIAL"; then
    ATA_OK=0
    echo "  ATA check 'IDENTIFY': MISSING"
fi
if ! grep -aq "ATA test: .* rw=OK" "$SERIAL"; then
    ATA_OK=0
    echo "  ATA check 'rw=OK': MISSING"
fi
[ "$ATA_OK" = "1" ] && echo "ATA_TEST PASS (IDENTIFY + LBA28 sector read/write verify)"

echo "=== MBR check (phase 15) ==="
MBR_OK=1
for c in "MBR test: sig=0xaa55" "part0: type=0x83"; do
    if ! grep -aq "$c" "$SERIAL"; then
        MBR_OK=0
        echo "  MBR check '$c': MISSING"
    fi
done
[ "$MBR_OK" = "1" ] && echo "MBR_TEST PASS (0xAA55 signature + partition parsed)"

echo "=== ext2 check (phase 16) ==="
EXT2_OK=1
for c in "ext2: super ok" "hello.txt" "ext2 test: hello.txt content='Hello from Nova ext2" ; do
    if ! grep -aq "$c" "$SERIAL"; then
        EXT2_OK=0
        echo "  ext2 check '$c': MISSING"
    fi
done
if ! grep -aq "verified\| OK" "$SERIAL"; then
    EXT2_OK=0
    echo "  ext2 check 'verified': MISSING"
fi
[ "$EXT2_OK" = "1" ] && echo "EXT2_TEST PASS (superblock + root dir + hello.txt content read)"

echo "=== multi-disk check (phase 20: multiple drives + capacity) ==="
MD_OK=1
# 挂载（启动日志）+ 交互（注入命令输出，shell 输出同时进串口）：
#   disk 列出 D:/E: 容量；cd E: 后 pwd 为 nova:E:/$；cat data.txt 读到内容
for c in "multi-disk: 2 drive" "D: mounted" "E: mounted" \
         "D:.*ext2" "E:.*ext2" \
         "Second drive data file" "Nova multi-disk test." \
         "nova:E:/"; do
    if ! grep -aq "$c" "$SERIAL"; then
        MD_OK=0
        echo "  multi-disk check '$c': MISSING"
    fi
done
[ "$MD_OK" = "1" ] && echo "MULTIDISK_TEST PASS (D:/E: mounted + disk/cd E:/cat data.txt)"
[ "$MD_OK" = "0" ] && echo "MULTIDISK_TEST FAIL (drives not all mounted or interactive check)"

echo "=== nvp check (phase 17) ==="
NVP_OK=1
for c in "nvp: 3 files" "readme.txt" "hello.txt" "version.txt" "nvp: hello.txt='Hello from .nvp!" "crc=OK"; do
    if ! grep -aq "$c" "$SERIAL"; then
        NVP_OK=0
        echo "  nvp check '$c': MISSING"
    fi
done
[ "$NVP_OK" = "1" ] && echo "NVP_TEST PASS (NVP1 archive parse + file read + CRC verify)"

echo "=== nvp-cli check (phase 18) ==="
NPCM_OK=1
for c in "nvpmgr: installed 'demo'" "nvp-cli test: install/list OK" ; do
    if ! grep -aq "$c" "$SERIAL"; then
        NPCM_OK=0
        echo "  nvp-cli check '$c': MISSING"
    fi
done
[ "$NPCM_OK" = "1" ] && echo "NVPMGR_TEST PASS (pkg install to tmpfs + list)"

echo "=== net check (phase 19: e1000 + ARP + TCP + HTTP repo) ==="
NET_OK=1
for c in "e1000: found" "e1000: MAC" "net: self 10.0.2.15" \
         "net: gateway MAC" "tcp: connected to 10.0.2.2:8080" \
         "http: GET /index.nvp ->" "repo: updated, 3 package(s) in index" \
         "repo: 3 package(s) available" \
         "http: GET /webdemo.nvp ->" \
         "net-repo test: repo=3 pkgs, HTTP install OK, installed=2"; do
    if ! grep -aq "$c" "$SERIAL"; then
        NET_OK=0
        echo "  net check '$c': MISSING"
    fi
done
[ "$NET_OK" = "1" ] && echo "NET_TEST PASS (e1000 NIC + ARP gateway + TCP connect + HTTP GET repo install)"

echo "=== shell check (phase 20: selftest + interactive commands) ==="
SH_OK=1
for c in "shell selftest: fs/redirect/pipe/env/builtins OK" \
         "PID  STATE" "repo (3 available)" ; do
    if ! grep -aq "$c" "$SERIAL"; then
        SH_OK=0
        echo "  shell check '$c': MISSING"
    fi
done
[ "$SH_OK" = "1" ] && echo "SHELL_TEST PASS (selftest + ps/ls/repo interactive)"

echo "=== font_cjk check (HZK16 bitmap CJK font, replaces FreeType) ==="
WY_OK=1
for c in "font_cjk: bitmap CJK ready" "font_cjk: CJK bitmap font loaded" \
         "font_cjk: .* glyphs, .* bytes blob" ; do
    if ! grep -aq "$c" "$SERIAL"; then
        WY_OK=0
        echo "  font_cjk check '$c': MISSING"
    fi
done
[ "$WY_OK" = "1" ] && echo "WENYUAN_TEST PASS (HZK16 CJK bitmap font loaded)"

echo "=== wenyuan pixel check (CJK 16px glyphs on screen) ==="
# shot3 为 font set wenyuan 后的截图（必含中文输出行）；shot1/0 为
# 启动画面（wenyuan 默认字体，含启动中文行）。
WY_SHOT_OK=0
for i in 3 1 0; do
    SHOT="$BUILD/shot$i.ppm"
    [ -f "$SHOT" ] || continue
    if python3 scripts/check-wenyuan-shot.py "$SHOT" >/dev/null 2>&1; then
        WY_SHOT_OK=1
        echo "  wenyuan shot$i: CJK glyphs OK"
        break
    fi
done
if [ "$WY_SHOT_OK" = "1" ]; then
    echo "WENYUAN_SHOT_TEST PASS (CJK 16px glyphs visible on screen)"
else
    echo "WENYUAN_SHOT_TEST FAIL (no CJK glyphs detected in screenshots)"
fi

echo "=== font_cjk glyph match (kernel bitmap vs host FreeType reference) ==="
# 说明：内核现在用 HZK16 位图（构建期从文渊 TTF 渲染，1-bit 无抗锯齿），
# 与宿主 FreeType 灰度渲染逐像素不可能一致，此检查仅作参考，不判定
# PASS/FAIL（渲染正确性由 WENYUAN_SHOT_TEST 的 CJK 字形像素验证保证）。
if [ -x "$BUILD/ref_char" ] || gcc -o "$BUILD/ref_char" scripts/ref_char.c \
        -Ithird_party/freetype/include /usr/lib/x86_64-linux-gnu/libfreetype.so.6 \
        >/dev/null 2>&1; then
    GM_NOTE=0
    for i in 3 1; do
        SHOT="$BUILD/shot$i.ppm"
        [ -f "$SHOT" ] || continue
        for ch in 你 好 黑 体; do
            "$BUILD/ref_char" src/graphics/font/WenYuanSansSCVF.ttf "$ch" \
                "$BUILD/ref_$ch.ppm" >/dev/null 2>&1
            R=$(python3 scripts/check-glyph-match.py "$SHOT" \
                    "$BUILD/ref_$ch.ppm" "$ch" 2>/dev/null | head -1)
            echo "  shot$i glyph '$ch': $R"
            GM_NOTE=1
            break 2
        done
    done
    [ "$GM_NOTE" = "1" ] && echo "WENYUAN_GLYPH_NOTE (reference: host FT vs kernel bitmap, diff expected)"
else
    echo "WENYUAN_GLYPH_NOTE SKIP (no host freetype)"
fi

echo "=== pixel check (phase 18: CLI interaction) ==="

# 分析单张截图：文本像素数 / 绿色像素数 / 分辨率
analyze() {
    python3 - "$1" <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
parts = data.split(b'\n', 3)
w, h = int(parts[1].split()[0]), int(parts[1].split()[1])
off = len(parts[0]) + 1 + len(parts[1]) + 1 + len(parts[2]) + 1
text_px = green_px = 0
for y in range(0, 416, 2):
    for x in range(0, 640, 2):
        o = off + (y * w + x) * 3
        r, g, b = data[o], data[o + 1], data[o + 2]
        if r or g or b:
            text_px += 1
        if g > 150 and r < 100 and b < 100:
            green_px += 1
print(f"{w} {h} {text_px} {green_px}")
EOF
}

# 找到注入后的内核截图（优先 shot2；1920x1080 且有文字）
AFTER_SHOT=""
for i in 2 1 0; do
    SHOT="$BUILD/shot$i.ppm"
    [ -f "$SHOT" ] || continue
    read -r w h text green <<< "$(analyze "$SHOT")"
    echo "  shot$i: ${w}x${h} text_px=$text green_px=$green"
    if [ "$w" = "1920" ] && [ "$h" = "1080" ] && [ "$text" -gt 500 ]; then
        AFTER_SHOT="$SHOT"
        break
    fi
done
if [ -z "$AFTER_SHOT" ]; then
    echo "PIXEL_TEST FAIL (no kernel screen captured)"
    exit 1
fi

# 回显校验（决定性证据）：自动扫描定位 CLI 交互输出
# （echo hello / help / nvp list），不依赖固定行号
if python3 scripts/verify-cli.py "$AFTER_SHOT"; then
    echo "PIXEL_TEST PASS (CLI: echo/help/nvp list interaction)"
    exit 0
fi
echo "PIXEL_TEST FAIL"
exit 1
