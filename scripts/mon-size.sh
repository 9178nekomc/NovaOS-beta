#!/usr/bin/env bash
cd "$(dirname "$0")/.."
wc -c build/inst-mon.log
head -c 400 build/inst-mon.log | tr -d '\r'
echo
echo "=== grep EIP ==="
grep -a 'EIP=' build/inst-mon.log | tail -3
