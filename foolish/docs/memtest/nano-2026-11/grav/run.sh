#!/bin/bash
# run.sh STEP...: the sat2 runs on build D, driven from the Mac through SSM (ssm.sh). Every step's output is appended
# to out/steps.log. Only build D is used: bin/cbase_D, built from foolish-bench at 9c5057b64 (log: out/build_D.log).
# Steps: check, sat_s1, sat_s2, sat_s3, sat_s4 (the shipped default: unpinned, no CPUAffinity in cbase.service, so
# srv.sh runs it the same way as the first three), push.
D=/Users/alex/Dev/foolish-bench-results/grav; source $D/state.env; S=$D/ssm.sh; L=$D/out/steps.log
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
IP=$(aws ec2 describe-instances --instance-ids $SRV --query 'Reservations[0].Instances[0].PrivateIpAddress' --output text)
log() { echo "[$(date -u +%T)] $*" | tee -a $L; }
on() { local who=$1; shift; local id; [ $who = srv ] && id=$SRV || id=$LOAD; bash $S $id "$1" ${2:-300} | tee -a $L; }
sat2() {   # sat2 LABEL BUILD
  log "sat2 $1 build $2"
  on srv "bash /opt/mt/srv.sh start $1 $2 8080 27"
  on load "bash /opt/mt/load.sh run $1 $IP 8 0 1 100000 25000" 200
  on srv "bash /opt/mt/srv.sh stop $1; cat /var/tmp/mt/capout_$1_mid.txt"
}
for st in "$@"; do
  case $st in
  check) log check; on srv "uname -r; nproc; cat /sys/kernel/mm/transparent_hugepage/enabled /sys/kernel/mm/transparent_hugepage/defrag; grep -E 'MemTotal|MemAvailable' /proc/meminfo; ls -l /opt/mt; test -f /opt/mt/READY && echo READY"
     on load "ls -l /opt/mt; cat /var/tmp/build_out.txt; nproc" ;;
  sat_s1) sat2 s1_D D ;;
  sat_s2) sat2 s2_D D ;;
  sat_s3) sat2 s3_D D ;;
  sat_s4) sat2 s4_D D ;;
  push) log push; on srv "bash /opt/mt/srv.sh push"; on load "bash /opt/mt/load.sh push" 300 ;;
  esac
done
