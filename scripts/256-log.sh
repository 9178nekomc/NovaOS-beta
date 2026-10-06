#!/usr/bin/env bash
cd "$(dirname "$0")/.."
grep -a 'install\|PANIC\|BAD free' build/inst-256r.log | tail -10
echo "=== raw tail ==="
tail -c 300 build/inst-256r.log | tr -d '\r' | tail -3
