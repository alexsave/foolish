#!/bin/bash
cd "$(dirname "$0")/.."
PY="/Users/alex/Dev/soundtrack-tools/venv/bin/python -I"
r() { nice $PY charts/rough_run.py "$@"; }
r u_det "th_n=4,detune.G=60,detune.D=60,detune.A=60" "G2c,D2" ""
r u_damp "th_n=4,sig0.G=15,sig0.D=15,sig0.A=15" "G2c,D2" ""
