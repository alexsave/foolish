#!/bin/bash
# the bow model knobs: the friction curve model (x_curve) closes the 5-10 kHz gap by 10 dB, so scan the thermal model's own constants (bristle stiffness, transition steepness, heat conduction)
cd /Users/alex/Dev/foolish-wt-altcello/foolish/tools/soundtrack/tools/altcello/
PY=/Users/alex/Dev/soundtrack-tools/venv/bin/python
run() { nice $PY -I charts/rich_run.py "$1" "$2" > out/rich/log_$1.txt 2>&1; }
run x_ths3 "loss=0,th_s0=3e6"
run x_ths10 "loss=0,th_s0=1e7"
run x_thxi3 "loss=0,th_xi=3"
run x_thb05 "loss=0,th_b=0.05"
echo done > out/rich/scan2.done
