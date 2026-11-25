#!/bin/bash
# trip1.sh STEP...: trip 1 driver (model: grav/out/e1/e1.sh), build D at the 192-game sat2 shape, knob auto, through SSM (grav/ssm.sh).
# Output appended to campaign/trip1/out/steps.log.
# Steps: check | smoke | base1 | perfcy | perfclk | bpf1 | base2 | irq1 | irq0 | proc:LABEL | push | sync
#   a run step: srv.sh start (fresh server and DB), cpu.sh sample, irqsnap start, [profiler start]; load.sh run 192 0 1 100000 25000
#   (with the load-box sampler); [profiler stop]; irqsnap end; srv.sh stop; cpu.sh report (E1 window and play window) and report2.
G=/Users/alex/Dev/foolish-bench-results/grav; source $G/state.env; S=$G/ssm.sh
T=/Users/alex/Dev/foolish-bench-results/campaign/trip1; L=$T/out/steps.log
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
IP=$(aws ec2 describe-instances --instance-ids $SRV --query 'Reservations[0].Instances[0].PrivateIpAddress' --output text)
log() { echo "[$(date -u +%T)] $*" | tee -a $L; }
ssm_out() { local o; o=$(bash $S "$1" "$2" ${3:-300}); echo "$o" | tee -a $L; }
run1() {   # run1 LABEL PROF(none|cy|clk|bpf) [IRQCPU]
  local lbl=$1 prof=$2 irq=${3:-} o t0 t1 mv win pre post pm p0 p1
  log "run $lbl prof=$prof irq=${irq:-default} knob=auto IP=$IP games=192"
  pre=""; [ -n "$irq" ] && pre="bash /opt/mt/prof.sh irq $irq $lbl; "
  pre+="bash /opt/mt/srv.sh start $lbl D 8080 27 auto; bash /opt/mt/cpu.sh sample $lbl; sleep 1; grep -h pinned /var/tmp/mt/server_$lbl.log; grep Cpus_allowed_list /proc/\$(cat /var/tmp/mt/pid_$lbl)/status; bash /opt/mt/cpu.sh irqsnap $lbl start"
  [ $prof != none ] && pre+="; bash /opt/mt/prof.sh start $prof $lbl"
  ssm_out $SRV "$pre" 240
  o=$(ssm_out $LOAD "bash /opt/mt/load.sh run $lbl $IP 192 0 1 100000 25000; bash /opt/mt/cpu.sh loadcheck $lbl" 420)
  mv=$(echo "$o" | grep -o 'moves=[0-9]*' | head -1 | cut -d= -f2)   # the summ line (first; the SSM status may prefix it)
  read -r t0 t1 < <(echo "$o" | grep -E '^[0-9]+\.[0-9]+$' | head -2 | tr '\n' ' ')
  win=$(echo "$o" | grep -o 'window_s=[0-9.]*' | head -1 | cut -d= -f2); t1=$(awk -v a=$t0 -v w=$win 'BEGIN{printf "%.6f", a+w}')
  pm=$(echo "$o" | grep 'play moves=' | grep -o 'moves=[0-9]*' | cut -d= -f2)
  p0=$(echo "$o" | grep -o 'epoch0=[0-9.]*' | cut -d= -f2); p1=$(echo "$o" | grep -o 'epoch1=[0-9.]*' | cut -d= -f2)
  log "run $lbl moves=$mv t_go=$t0 cpu_window_end=$t1 (window_s=$win) play_moves=$pm play=[$p0,$p1]"
  post=""; [ $prof != none ] && post="bash /opt/mt/prof.sh stop $prof $lbl; "
  post+="bash /opt/mt/cpu.sh irqsnap $lbl end; bash /opt/mt/srv.sh stop $lbl; echo '== E1 window'; bash /opt/mt/cpu.sh report $lbl $t0 $t1 $mv; echo '== play window'; bash /opt/mt/cpu.sh report $lbl $p0 $p1 $pm; bash /opt/mt/cpu.sh report2 $lbl $p0 $p1 $pm"
  post+="; grep -E 'VmHWM|VmRSS' /var/tmp/mt/status_${lbl}_end.txt; sort -g -k2 /var/tmp/mt/rss_$lbl.log | tail -1; grep -E 'inline dispatch|journal:|valve|ERROR' /var/tmp/mt/server_$lbl.log | tail -6; grep -c WARN /var/tmp/mt/server_$lbl.log; free -m | head -2"
  ssm_out $SRV "$post" 300
}
for st in "$@"; do
  case $st in
  check) log check
    ssm_out $SRV "uname -r; nproc; cat /sys/kernel/mm/transparent_hugepage/enabled; ls -l /opt/mt; sha256sum /opt/mt/cbase_D | cut -c1-16; test -f /opt/mt/READY && echo READY; test -f /opt/mt/UD_DONE && echo UD_DONE; tail -2 /var/tmp/dnf_srv.txt; bash /opt/mt/prof.sh info; bash /opt/mt/prof.sh symsetup"
    ssm_out $LOAD "ls -l /opt/mt | wc -l; test -f /opt/mt/UD_DONE && echo UD_DONE; grep -c failed /var/tmp/build_out.txt; tail -1 /var/tmp/build_out.txt; nproc; perf --version; uname -r" ;;
  smoke) log smoke
    ssm_out $SRV "bash /opt/mt/srv.sh start smoke D 8080 '' auto; grep -h pinned /var/tmp/mt/server_smoke.log"
    bash $S $LOAD "bash /opt/mt/load.sh run smoke $IP 16 0 1 100000 15000" 200 > $T/out/smoke_load.txt 2>&1 &
    lp=$!; sleep 12
    ssm_out $SRV "bash /opt/mt/prof.sh smoke 6" 200
    wait $lp; cat $T/out/smoke_load.txt | tee -a $L | head -3
    ssm_out $SRV "bash /opt/mt/srv.sh stop smoke" ;;
  base1) run1 base1 none ;; perfcy) run1 perfcy cy ;; perfclk) run1 perfclk clk ;; bpf1) run1 bpf1 bpf ;; base2) run1 base2 none ;;
  irq1) run1 irq1 none 1 ;; irq0) run1 irq0 none 0 ;;
  proc:*) l=${st#proc:}; log "proc $l"; ssm_out $LOAD "bash /opt/mt/prof.sh process $l" 900 ;;
  push) log push; ssm_out $SRV "bash /opt/mt/srv.sh push" 300; ssm_out $LOAD "bash /opt/mt/load.sh push" 900 ;;
  sync) log sync; mkdir -p $T/out/raw; aws s3 sync --only-show-errors s3://$B/res/ $T/out/raw/; du -sh $T/out/raw | tee -a $L; ls $T/out/raw/* | wc -l | tee -a $L ;;
  esac
done
