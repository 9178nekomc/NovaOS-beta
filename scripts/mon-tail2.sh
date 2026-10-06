#!/usr/bin/env bash
cd "$(dirname "$0")/.."
grep -ac 'EAX=' build/inst-mon.log
tail -c 1500 build/inst-mon.log | tr -d '\r' | tail -20
