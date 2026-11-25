#!/bin/bash
# e1.sh STEP...: experiment E1 on build D at the 192-pair (sat2) shape, driven through SSM (ssm.sh). Output appended to out/e1/steps.log.
# Steps: check, auto1 off1 auto2 off2 auto3 off3 (each: srv.sh start with KNOB, cpu sampler, load.sh run 192 pairs, srv.sh stop + cpu report), push.
D=/Users/alex/Dev/foolish-bench-results/grav; source $D/state.env; S=$D/ssm.sh; L=$D/out/e1/steps.log
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
IP=$(aws ec2 describe-instances --instance-ids $SRV --query 'Reservations[0].Instances[0].PrivateIpAddress' --output text)
log() { echo "[$(date -u +%T)] $*" | tee -a $L; }
# ssm_out ID CMD TIMEOUT: prints the SSM output to the log and to stdout
ssm_out() { local o; o=$(bash $S "$1" "$2" ${3:-300}); echo "$o" | tee -a $L; }
run1() {   # run1 LABEL KNOB
  local lbl=$1 knob=$2 o t0 t1 mv
  log "run $lbl knob=$knob IP=$IP pairs=192"
  ssm_out $SRV "bash /opt/mt/srv.sh start $lbl D 8080 27 $knob; bash /opt/mt/cpu.sh sample $lbl; sleep 1; grep -h pinned /var/tmp/mt/server_$lbl.log; cat /var/tmp/mt/knob_$lbl; grep Cpus_allowed_list /proc/\$(cat /var/tmp/mt/pid_$lbl)/status" 120
  o=$(ssm_out $LOAD "bash /opt/mt/load.sh run $lbl $IP 192 0 1 100000 25000; bash /opt/mt/cpu.sh loadcheck $lbl" 420)
  mv=$(echo "$o" | grep -o 'moves=[0-9]*' | head -1 | cut -d= -f2)
  read -r t0 t1 < <(echo "$o" | grep -E '^[0-9]+\.[0-9]+$' | head -2 | tr '\n' ' ')
  # the CPU window is the summary's moves window (t_go + window_s), not t_go..t_done: the clients stop at 25 s and the quit wait follows
  win=$(echo "$o" | grep -o 'window_s=[0-9.]*' | head -1 | cut -d= -f2); t1=$(awk -v a=$t0 -v w=$win 'BEGIN{printf "%.6f", a+w}')
  log "run $lbl moves=$mv t_go=$t0 cpu_window_end=$t1 (window_s=$win)"
  ssm_out $SRV "bash /opt/mt/srv.sh stop $lbl; bash /opt/mt/cpu.sh report $lbl $t0 $t1 $mv; grep -E 'VmHWM|VmRSS' /var/tmp/mt/status_${lbl}_end.txt; sort -g -k2 /var/tmp/mt/rss_$lbl.log | tail -1" 120
}
for st in "$@"; do
  case $st in
  check) log check; ssm_out $SRV "uname -r; nproc; cat /sys/kernel/mm/transparent_hugepage/enabled; getconf CLK_TCK; ls -l /opt/mt; sha256sum /opt/mt/cbase_D | cut -c1-16; test -f /opt/mt/READY && echo READY; test -f /opt/mt/UD_DONE && echo UD_DONE"
         ssm_out $LOAD "ls -l /opt/mt | wc -l; test -f /opt/mt/UD_DONE && echo UD_DONE; grep -c failed /var/tmp/build_out.txt; nproc" ;;
  auto1) run1 auto1 auto ;; off1) run1 off1 off ;;
  auto2) run1 auto2 auto ;; off2) run1 off2 off ;;
  auto3) run1 auto3 auto ;; off3) run1 off3 off ;;
  push) log push; ssm_out $SRV "bash /opt/mt/srv.sh push" 300; ssm_out $LOAD "bash /opt/mt/load.sh push" 600 ;;
  esac
done
