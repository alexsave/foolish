#!/bin/bash
# F1: bow noise corner and level scan (one configuration per run of rich_run.py)
cd /Users/alex/Dev/foolish-wt-altcello/foolish/tools/soundtrack/tools/altcello/
PY=/Users/alex/Dev/soundtrack-tools/venv/bin/python
for fc in 6000 10000 16000; do
  for lv in 0.005 0.01 0.02; do
    rate=$((fc*4))
    nice $PY -I charts/rich_run.py f1_${fc}_${lv} "bn_fc=$fc,bn_rate=$rate,bn_level=$lv" > out/rich/log_f1_${fc}_${lv}.txt 2>&1
  done
done
echo done > out/rich/f1.done
