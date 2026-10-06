#!/usr/bin/env bash
cd "$(dirname "$0")/.."
make -j8 iso 2>&1 | grep -E 'error|OK\]' | tail -1
bash scripts/pt-dump.sh 2>&1 | grep -aE 'kprobe|vmm: probe|PANIC: exception' | head -12
