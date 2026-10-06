#!/usr/bin/env bash
cd "$(dirname "$0")/.."
grep -a 'PMM: kernel image reserved\|Paging enabled\|kernel 0x' build/inst1.log | head -5
