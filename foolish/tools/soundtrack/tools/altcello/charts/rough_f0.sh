#!/bin/bash
cd "$(dirname "$0")/.."
PY="/Users/alex/Dev/soundtrack-tools/venv/bin/python -I"
nice $PY charts/rough_run.py "solo" "" "D2,G2c" "--solo"
nice $PY charts/rough_run.py "gopen" "" "G2o,G2onv" ""
nice $PY charts/rough_run.py "solo_nv" "" "D2,G2c" "--solo" novib
