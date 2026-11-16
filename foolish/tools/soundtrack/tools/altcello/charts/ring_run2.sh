#!/bin/bash
# the pure-fifths tuning factor (lead's request after charts/ring_run.sh), serial
cd "$(dirname "$0")/.."
PY=/Users/alex/Dev/soundtrack-tools/venv/bin/python
nice $PY -I charts/ring_study.py fifths "fifths=1"
echo DONE
