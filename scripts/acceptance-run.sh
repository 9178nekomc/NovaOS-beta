#!/usr/bin/env bash
cd "$(dirname "$0")/.."
bash scripts/test-install.sh 2>&1 | grep -aE 'install: done|PASS|FAIL|kernel.elf|limine-bios|dict.dat|MBR' | head -16
