#!/bin/bash
# F1 beyond the brief's range: level 0.04 and 0.08 at the 16 kHz corner
cd /Users/alex/Dev/foolish-wt-altcello/foolish/tools/soundtrack/tools/altcello/
PY=/Users/alex/Dev/soundtrack-tools/venv/bin/python
for lv in 0.04 0.08; do
  nice $PY -I charts/rich_run.py f1_16000_${lv} "bn_fc=16000,bn_rate=64000,bn_level=$lv" > out/rich/log_f1_16000_${lv}.txt 2>&1
done
echo done > out/rich/f1b.done
