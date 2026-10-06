#!/usr/bin/env bash
# scripts/setup-toolchain.sh - Nova OS 阶段一：Ubuntu/Debian 工具链安装脚本
#
# 安装内容：
#   build-essential / gcc / make / nasm - 内核编译工具链
#   xorriso / mtools                     - ISO 制作（El Torito + EFI）
#   git                                  - 版本控制与 Limine 克隆
#   qemu-system-x86 / ovmf               - 本地运行与 UEFI 固件测试
#   dosfstools                           - 磁盘镜像工具（后续阶段使用）
#   curl                                 - 下载工具（后续阶段使用）
#
# 用法：bash scripts/setup-toolchain.sh
set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo"
fi

echo "[Nova] Updating package lists..."
$SUDO apt-get update

echo "[Nova] Installing toolchain packages..."
$SUDO apt-get install -y \
    build-essential \
    gcc \
    make \
    nasm \
    xorriso \
    mtools \
    dosfstools \
    git \
    curl \
    qemu-system-x86 \
    ovmf

echo "[Nova] Verifying..."
for cmd in gcc make nasm xorriso mformat qemu-system-x86_64 git; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "[Nova] WARNING: $cmd not found"
    else
        echo "[Nova]   OK: $cmd -> $(command -v $cmd)"
    fi
done

echo "[OK] Nova OS toolchain installed."
