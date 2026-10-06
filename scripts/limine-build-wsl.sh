#!/usr/bin/env bash
# scripts/limine-build-wsl.sh - 在 WSL 中 configure + make 构建 Limine
#
# 前置：先运行 scripts/limine-prep-wsl.sh（克隆 + bootstrap）
# 产物：/root/limine-src/ 下的 limine 主机工具与引导二进制
#
# 用法：wsl -u root -e bash scripts/limine-build-wsl.sh
set -euo pipefail

WORK="${WORK:-/root/limine-src}"

cd "${WORK}"

echo "[Nova] configuring limine (bios + bios-cd + uefi-x86-64 + uefi-ia32 + uefi-cd) ..."
./configure \
    --enable-bios \
    --enable-bios-cd \
    --enable-uefi-x86-64 \
    --enable-uefi-ia32 \
    --enable-uefi-cd

echo "[Nova] building limine ..."
make -j"$(nproc)"

echo "[Nova] build outputs:"
ls -la limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin \
      BOOTX64.EFI BOOTIA32.EFI limine 2>&1
echo "[OK] limine built at ${WORK}"
