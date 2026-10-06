#!/usr/bin/env bash
# scripts/clone-limine.sh - Nova OS 阶段一：克隆 Limine v8.x 引导器
#
# 用法：bash scripts/clone-limine.sh
# 环境变量 LIMINE_TAG 可覆盖版本（默认 v8.7.0）
set -euo pipefail

cd "$(dirname "$0")/.."

LIMINE_TAG="${LIMINE_TAG:-v8.7.0}"

if [ -d "limine/.git" ]; then
    echo "[Nova] limine/ already exists, skipping clone"
    exit 0
fi

echo "[Nova] Cloning Limine $LIMINE_TAG ..."
git clone --depth 1 --branch "$LIMINE_TAG" \
    https://github.com/limine-bootloader/limine.git limine

echo "[Nova] Building Limine host tools and bootloader binaries ..."
make -C limine

echo "[OK] Limine $LIMINE_TAG cloned and built."
