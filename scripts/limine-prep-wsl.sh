#!/usr/bin/env bash
# scripts/limine-prep-wsl.sh - 在 WSL 中准备 Limine 源码构建
#
# 背景：Windows 宿主机走本地代理(默认 127.0.0.1:10808)，WSL NAT 模式下
#       无法直接访问 localhost，需通过宿主机网关 IP 复用该代理。
#
# 本脚本执行：克隆 limine v8.x 到 /root/limine-src（WSL 原生文件系统，
#   避免 /mnt/d drvfs 慢速 IO），运行 bootstrap 拉取第三方依赖。
#
# 用法：wsl -u root -e bash scripts/limine-prep-wsl.sh
set -euo pipefail

LIMINE_TAG="${LIMINE_TAG:-v8.7.0}"
PROXY_PORT="${WSL_PROXY_PORT:-10808}"
WORK="${WORK:-/root/limine-src}"

# 可选：复用 Windows 宿主机代理（默认关闭；代理仅监听 127.0.0.1 时
# WSL NAT 无法到达，WSL 直连 GitHub 通常即可）
if [ "${USE_WSL_PROXY:-0}" = "1" ]; then
    GW="$(ip route show default | awk '{print $3}')"
    export http_proxy="http://${GW}:${PROXY_PORT}"
    export https_proxy="http://${GW}:${PROXY_PORT}"
    echo "[Nova] using proxy: ${http_proxy}"
fi

rm -rf "${WORK}"
echo "[Nova] cloning limine ${LIMINE_TAG} ..."
git clone --depth 1 --branch "${LIMINE_TAG}" \
    https://github.com/limine-bootloader/limine.git "${WORK}"

cd "${WORK}"
echo "[Nova] running bootstrap ..."
./bootstrap
echo "[OK] limine source prepared at ${WORK}"
