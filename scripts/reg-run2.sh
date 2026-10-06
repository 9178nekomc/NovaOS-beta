#!/usr/bin/env bash
cd "$(dirname "$0")/.."
bash scripts/regression-run.sh 2>&1 | tail -20
