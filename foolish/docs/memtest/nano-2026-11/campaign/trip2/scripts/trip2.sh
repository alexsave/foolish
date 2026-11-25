#!/bin/bash
# trip2.sh STEP...: trip 2 driver (model: campaign/trip1/scripts/trip1.sh), the A/B of the PR builds at the 192-game shape,
# knob auto, through SSM (grav/ssm.sh). Output appended to campaign/trip2/out/steps.log.
# Steps:
#   check            server and load box facts (chronyd running, binaries and their sha256, irqbalance, ENA IRQs)
#   warm             one throwaway run of arm A (cbase_D, split), label wA
#   rounds           three rounds of A B C E F G N (labels r1A ... r3N), with the time-budget drops below
#   human            if box time is under 34 min: hum_split and hum_cpu1 (cbase_D, 192 pairs, think 2-4 s, 45 s)
#   push | sync
# Every run: irq.sh MODE (stops irqbalance, pins the ENA IRQs, verifies), srv.sh start (fresh server and DB), cpu.sh sample,
# cpu.sh irqsnap start; load.sh run; then cpu.sh irqsnap end, irq.sh show, srv.sh stop, cpu.sh report/report2 for the play
# window, irq.sh pct, memory and the server's shutdown lines. The post step of one run and the pre step of the next go in
# one SSM call to save time.
# Time budget (from out/launch_epoch, the server's run-instances time): no run starts if it would end after minute 38
# (estimated from the mean run time so far); arm N runs are dropped first, then round 3 of F and G; A B C E never dropped.
G=/Users/alex/Dev/foolish-bench-results/grav; source $G/state.env; S=$G/ssm.sh
T=/Users/alex/Dev/foolish-bench-results/campaign/trip2; L=$T/out/steps.log
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
IP=$(aws ec2 describe-instances --instance-ids $SRV --query 'Reservations[0].Instances[0].PrivateIpAddress' --output text)
LE=$(cat $T/out/launch_epoch)
STOP_S=$((38*60))
log() { echo "[$(date -u +%T) +$(elapsed)s] $*" | tee -a $L; }
elapsed() { echo $(( $(date +%s) - LE )); }
ssm_out() { local o; o=$(bash $S "$1" "$2" ${3:-300}); echo "$o" | tee -a $L; }
build_of() { case $1 in A|F) echo D;; B) echo P1;; C) echo P3;; E|G) echo ALL;; N) echo N1;; hum_split|hum_cpu1) echo D;; esac; }
mode_of() { case $1 in F|G|hum_cpu1) echo cpu1;; *) echo split;; esac; }
pre_cmd() {   # pre_cmd LABEL BUILD MODE
  local l=$1 b=$2 m=$3
  echo "echo '== pre $l build=$b irq=$m'; bash /opt/mt/irq.sh $m $l || { sleep 1; bash /opt/mt/irq.sh $m $l; }; bash /opt/mt/srv.sh start $l $b 8080 27 auto; bash /opt/mt/cpu.sh sample $l; sleep 1; grep -h pinned /var/tmp/mt/server_$l.log; grep Cpus_allowed_list /proc/\$(cat /var/tmp/mt/pid_$l)/status; bash /opt/mt/irq.sh show; bash /opt/mt/cpu.sh irqsnap $l start; sha256sum /opt/mt/cbase_$b | cut -c1-16"
}
post_cmd() {  # post_cmd LABEL P0 P1 PLAYMOVES
  local l=$1 p0=$2 p1=$3 pm=$4
  echo "echo '== post $l'; bash /opt/mt/cpu.sh irqsnap $l end; bash /opt/mt/irq.sh show; bash /opt/mt/irq.sh pct $l; bash /opt/mt/srv.sh stop $l; echo '== play window'; bash /opt/mt/cpu.sh report $l $p0 $p1 $pm; bash /opt/mt/cpu.sh report2 $l $p0 $p1 $pm; grep -E 'VmHWM|VmRSS' /var/tmp/mt/status_${l}_end.txt; echo \"rssmax \$(sort -g -k2 /var/tmp/mt/rss_$l.log | tail -1)\"; grep -E 'inline dispatch|journal:|valve|ERROR' /var/tmp/mt/server_$l.log | tail -6; echo \"warn_lines \$(grep -c WARN /var/tmp/mt/server_$l.log)\"; free -m | head -2; echo '== end post $l'"
}
PENDING=""     # the post command of the last run, sent with the next pre (or alone by flush_post)
NRUN=0; SUMRUN=0
flush_post() { [ -n "$PENDING" ] && ssm_out $SRV "$PENDING" 300; PENDING=""; }
run1() {   # run1 LABEL ARM TMIN TMAX DUR_MS [PW0 PW1]
  local l=$1 arm=$2 tmin=$3 tmax=$4 dur=$5 pw0=${6:-2} pw1=${7:-23} b m o mv pm p0 p1 t0 pre
  b=$(build_of $arm); m=$(mode_of $arm); t0=$(date +%s)
  log "run $l arm=$arm build=$b irq=$m knob=auto IP=$IP pairs=192 think=$tmin-$tmax dur_ms=$dur window=[t_go+$pw0,t_go+$pw1]"
  pre=$(pre_cmd $l $b $m); [ -n "$PENDING" ] && pre="$PENDING; $pre"; PENDING=""
  o=$(ssm_out $SRV "$pre" 300)
  if ! echo "$o" | grep -q "== pre $l" || ! echo "$o" | grep -q "start $l build $b pid [0-9]* 1 listening"; then log "PRE FAILED $l"; ssm_out $SRV "bash /opt/mt/srv.sh stop $l" 120; return 1; fi
  if echo "$o" | sed -n "/== pre $l/,\$p" | grep -q IRQ_FAIL; then log "IRQ FAILED $l"; ssm_out $SRV "bash /opt/mt/srv.sh stop $l" 120; return 1; fi
  o=$(ssm_out $LOAD "PW0=$pw0 PW1=$pw1 bash /opt/mt/load.sh run $l $IP 192 $tmin $tmax 100000 $dur; bash /opt/mt/cpu.sh loadcheck $l" 420)
  mv=$(echo "$o" | grep -o 'moves=[0-9]*' | head -1 | cut -d= -f2)
  pm=$(echo "$o" | grep 'play moves=' | grep -o 'moves=[0-9]*' | cut -d= -f2)
  p0=$(echo "$o" | grep -o 'epoch0=[0-9.]*' | cut -d= -f2); p1=$(echo "$o" | grep -o 'epoch1=[0-9.]*' | cut -d= -f2)
  log "run $l moves=$mv play_moves=$pm play=[$p0,$p1]"
  PENDING=$(post_cmd $l ${p0:-0} ${p1:-0} ${pm:-1})
  if [ -z "$pm" ] || [ -z "$p0" ]; then log "LOAD FAILED $l"; flush_post; return 1; fi
  NRUN=$((NRUN+1)); SUMRUN=$((SUMRUN + $(date +%s) - t0))
}
avg_run() { if [ $NRUN -gt 0 ]; then echo $(( SUMRUN / NRUN + 12 )); else echo 90; fi; }   # +12 s: the post step of the last run
cls() { case $1 in A|B|C|E) echo 0;; F|G) [ $2 -le 2 ] && echo 0 || echo 1;; N) echo 2;; esac; }
rounds() {
  local plan=() i j r a k ahead e av
  for r in 1 2 3; do for a in A B C E F G N; do plan+=("$r$a"); done; done
  for ((i=0; i<${#plan[@]}; i++)); do
    r=${plan[$i]:0:1}; a=${plan[$i]:1}; k=$(cls $a $r); e=$(elapsed); av=$(avg_run)
    ahead=0; for ((j=i+1; j<${#plan[@]}; j++)); do [ $(cls ${plan[$j]:1} ${plan[$j]:0:1}) -lt $k ] && ahead=$((ahead+1)); done
    if [ $((e + av)) -gt $STOP_S ]; then log "STOP: elapsed ${e}s + run ${av}s passes minute 38; not starting r$r$a or later"; return 0; fi
    if [ $k -gt 0 ] && [ $((e + av + ahead*av)) -gt $STOP_S ]; then log "DROP r$r$a (class $k): ${e}s + ${av}s + $ahead higher-priority runs x ${av}s passes minute 38"; continue; fi
    run1 r$r$a $a 0 1 25000 || { log "run r$r$a failed, retrying once"; run1 r$r$a $a 0 1 25000 || { log "r$r$a failed twice: stopping the rounds"; return 1; }; }
  done
}
for st in "$@"; do
  case $st in
  check) log check
    ssm_out $SRV "uname -r; nproc; free -m | head -2; echo chronyd \$(systemctl is-active chronyd); chronyc tracking 2>&1 | grep -E 'Leap|System time'; ls -l /opt/mt; sha256sum /opt/mt/cbase_* | cut -c1-80; test -f /opt/mt/READY && echo READY; test -f /opt/mt/UD_DONE && echo UD_DONE; echo irqbalance \$(systemctl is-active irqbalance); grep -E 'ena|ens5' /proc/interrupts; rpm -q perf bpftrace 2>&1 | tr '\n' ' '; echo; dmesg | grep -i -E 'oom|killed process' | tail -3"
    ssm_out $LOAD "ls -l /opt/mt | wc -l; test -f /opt/mt/UD_DONE && echo UD_DONE; grep -c failed /var/tmp/build_out.txt; tail -1 /var/tmp/build_out.txt; nproc; uname -r; echo chronyd \$(systemctl is-active chronyd)" ;;
  warm) run1 wA A 0 1 25000; flush_post ;;
  rounds) rounds; flush_post ;;
  human) e=$(elapsed)
    if [ $e -lt $((34*60)) ]; then run1 hum_split hum_split 2000 4000 45000 5 43 && run1 hum_cpu1 hum_cpu1 2000 4000 45000 5 43; flush_post
    else log "human runs skipped: box time ${e}s is not under 34 min"; fi ;;
  push) log push; ssm_out $SRV "bash /opt/mt/srv.sh push" 300; ssm_out $LOAD "bash /opt/mt/load.sh push" 900 ;;
  sync) log sync; mkdir -p $T/out/raw; aws s3 sync --only-show-errors s3://$B/res/ $T/out/raw/; du -sh $T/out/raw | tee -a $L; find $T/out/raw -type f | wc -l | tee -a $L ;;
  esac
done
