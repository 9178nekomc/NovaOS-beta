#!/usr/bin/env bash
cd "$(dirname "$0")/.."
make -j8 iso 2>&1 | grep -E 'error|OK\]' | tail -2
bash scripts/test-install.sh 2>&1 | tail -26
