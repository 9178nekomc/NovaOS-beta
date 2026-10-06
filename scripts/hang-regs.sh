#!/usr/bin/env bash
cd "$(dirname "$0")/.."
echo "=== last registers (hang point) ==="
grep -A4 'EAX=' build/inst-mon.log | tail -14
echo "=== hang point in inst1.log ==="
grep -a 'install' build/inst1.log | tail -2
