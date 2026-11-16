#!/bin/bash
# one-factor scans of the richness work: each configuration against the candidate 4 defaults (loss=0 is the sigma1 string loss of candidate 4)
cd /Users/alex/Dev/foolish-wt-altcello/foolish/tools/soundtrack/tools/altcello/
PY=/Users/alex/Dev/soundtrack-tools/venv/bin/python
run() { nice $PY -I charts/rich_run.py "$1" "$2" > out/rich/log_$1.txt 2>&1; }
run c4 "loss=0"
run f3_statf1 "loss=0,stat_f1=16000"
run f3_etaexp "loss=0,eta_exp=0.15"
run f3_massexp "loss=0,mass_exp=0.3"
run f4_br07 "loss=0,bridge_r=0.7"
run f4_br15 "loss=0,bridge_r=1.5"
run f4_lead2 "loss=0,lead_mass=2"
run f4_lead05 "loss=0,lead_mass=0.5"
run f4_stat2 "loss=0,stat_mass=2"
run f4_stat05 "loss=0,stat_mass=0.5"
run x_bw5 "loss=0,bw=0.005"
run x_curve "loss=0,bow=0"
echo done > out/rich/scan.done
