#!/usr/bin/env bash
cd "$(dirname "$0")/.."
grep -a 'PANIC' build/inst1.log | head -8
