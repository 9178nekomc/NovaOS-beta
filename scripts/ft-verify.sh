#!/usr/bin/env bash
cd "$(dirname "$0")/.."
make -j8 iso 2>&1 | grep -E 'error|OK\]' | tail -1
echo "===== 256M 安装测试 ====="
bash scripts/256-dual-test.sh 2>&1 | tail -4
echo "===== 全量回归 ====="
bash scripts/regression-run.sh 2>&1 | grep -aE 'PASS|FAIL' | tail -18
