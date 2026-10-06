#!/usr/bin/env bash
cd "$(dirname "$0")/.."
bash scripts/test-qemu.sh 2>&1 | grep -aE 'PASS|FAIL|NOTE' | tail -18
