#!/bin/bash
# expert single notes and the scale transitions under one model, serially. Usage: charts/thermal_run2.sh NAME KNOBS   (NAME: curve or a variant name)
cd "$(dirname "$0")/.." || exit 1
PY=/Users/alex/Dev/soundtrack-tools/venv/bin/python
mkdir -p out/thermal out/ring_study
export ALTCELLO_KNOBS="$2"
nice $PY -I charts/thermal_gates.py "$1"
nice $PY -I charts/ring_study.py "th_$1" "$2" scale
echo DONE "$1"
