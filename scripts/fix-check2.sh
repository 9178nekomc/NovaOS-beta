#!/usr/bin/env bash
cd "$(dirname "$0")/.."
grep -a 'install' build/inst1.log | tail -5
echo "=== fixed map count ==="
grep -ac 'fixed map' build/inst1.log
echo "=== raw tail ==="
tail -c 300 build/inst1.log | tr -d '\r' | tail -3
