#!/usr/bin/env bash
cd "$(dirname "$0")/.."
make -j8 iso 2>&1 | grep -E 'error|OK\]' | tail -1
bash scripts/test-install.sh 2>&1 | grep -aE 'PASS|FAIL|MBR:|parts|ext2: mount|ext2: super|ext2: no|done' | head -20
