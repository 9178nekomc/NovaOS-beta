#!/usr/bin/env bash
cd "$(dirname "$0")/.."
make -j8 iso 2>&1 | grep -E 'error|OK\]' | tail -2
bash scripts/pt-dump.sh 2>&1 | grep -aE 'PANIC: walk|PANIC: exception|walk: PML4|walk: PDP|walk: PD|walk: PT' | head -14
