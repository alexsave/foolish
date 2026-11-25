#!/bin/bash
# waitready.sh: polls both boxes through SSM until /opt/mt/READY (server) and UD_DONE (load) exist, at most ~12 min
G=/Users/alex/Dev/foolish-bench-results/grav; source $G/state.env
n=0; while [ $n -lt 40 ]; do
  a=$(bash $G/ssm.sh $SRV "test -f /opt/mt/READY && echo RDY; test -f /opt/mt/UD_DONE && echo UDD" 60 2>&1 | tr '\n\t' '  ')
  b=$(bash $G/ssm.sh $LOAD "test -f /opt/mt/UD_DONE && echo UDD" 60 2>&1 | tr '\n\t' '  ')
  echo "$(date -u +%T) srv[$a] load[$b]"
  case "$a$b" in *RDY*UDD*UDD*) exit 0;; esac
  sleep 15; n=$((n+1))
done; exit 1
