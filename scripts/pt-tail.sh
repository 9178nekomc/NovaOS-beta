#!/usr/bin/env bash
cd "$(dirname "$0")/.."
tail -60 build/pt-mon.log | head -60
