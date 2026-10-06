#!/usr/bin/env bash
cd "$(dirname "$0")/.."
grep -a 'install' build/inst1.log | tail -4
echo "=== panic? ==="
grep -a 'PANIC\|fixed map' build/inst1.log | tail -4
echo "=== raw tail 300 ==="
tail -c 300 build/inst1.log | tr -d '\r' | tail -3
