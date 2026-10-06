#!/usr/bin/env bash
cd "$(dirname "$0")/.."
grep -a 'probe' build/inst1.log | head -8
echo "=== vmm lines ==="
grep -a 'vmm' build/inst1.log | head -8
