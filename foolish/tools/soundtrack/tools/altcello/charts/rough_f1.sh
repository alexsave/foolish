#!/bin/bash
cd "$(dirname "$0")/.."
PY="/Users/alex/Dev/soundtrack-tools/venv/bin/python -I"
for s in 0 0.25 0.5; do
  nice $PY charts/rough_run.py "f1_$s" "bn_scale.C=$s" "D2,G2c" "" 
done
