#!/bin/bash
# FE body one-factor study: the richness measurement (rich_run.py) for the three bodies, serially. Usage: charts/fe_run.sh [STAT|A|B ...]
cd "$(dirname "$0")/.." || exit 1
PY=/Users/alex/Dev/soundtrack-tools/venv/bin/python
for b in ${@:-STAT A B}; do
  case $b in
    STAT) nice $PY -I charts/rich_run.py fe_stat "" ;;
    A)    nice $PY -I charts/rich_run.py fe_a "fe=1" ;;
    B)    nice $PY -I charts/rich_run.py fe_b "fe=2" ;;
  esac
done
