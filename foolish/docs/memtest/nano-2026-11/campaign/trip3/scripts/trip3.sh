#!/bin/bash
# trip3.sh STEP...: trip 3 driver, a copy of campaign/trip2/scripts/trip2.sh (same pre/post commands, samplers, play window
# and load shape) for five builds, all with the interrupts in the `split` layout, knob auto (the shipped default), plus the
# profiled runs from trip 1 (prof.sh, trip.bt). Output appended to campaign/trip3/out/steps.log.
# Steps:
#   check            server and load box facts (chronyd, binaries and their sha256, irqbalance, ENA IRQs, perf/bpftrace, OOM in dmesg)
#   install          perf, bpftrace and strace on the server by hand (a 768 MB swapfile first so dnf is not OOM-killed, as in
#                    trip 1's boot), then chronyd and dmesg checked again, then the swapfile is turned off for the counted runs
#   symcheck         prof.sh symsetup M3 (HOME set), a 16-pair load on a cbase_M3 server: perf smoke 6 s (cbase_M3 samples
#                    must resolve through the unstripped twin) and bpftrace attached with trip.bt for 3 s (else strace fallback)
#   warm             one throwaway run of D (label wD)
#   rounds           three rounds of D M1 M2 M3 M4 (labels r1D ... r3M4), then the count runs bpfM1 and bpfM2 (trip.bt) and the
#                    perf run perfM3 (perf record -a -g -F 999 -e cycles -k CLOCK_MONOTONIC), with the time-budget drops below
#   proc             prof.sh process perfM3 M3 on the load box (flat and callchain dumps, perf reports), in parallel with the
#                    server's push
#   push | sync
# Every run: irq.sh split (stops irqbalance, pins the ENA IRQs, verifies), srv.sh start (fresh server and DB), cpu.sh sample,
# cpu.sh irqsnap start, [prof.sh start]; load.sh run LABEL IP 192 0 1 100000 25000; then [prof.sh stop], cpu.sh irqsnap end,
# irq.sh show, irq.sh pct, srv.sh stop, cpu.sh report/report2 for the play window, memory and the server's shutdown lines.
# The post step of one run and the pre step of the next go in one SSM call (as trip 2), so the post block of run X is logged
# after the header of run X+1: analyze by the labels inside the lines.
# Time budget (from out/launch_epoch, the server's run-instances time): a run starts only if it would end by minute 38
# (estimated from the mean run time so far); the class-0 runs (all of D/M1/M2/M3 rounds 1-3, M4 rounds 1-2, bpfM1, bpfM2,
# perfM3) are never dropped for a lower class; r3M4 (class 1) is dropped first, then r3D (class 2), when they would push the
# remaining higher-priority runs past minute 38.
G=/Users/alex/Dev/foolish-bench-results/grav; source $G/state.env; S=$G/ssm.sh
T=/Users/alex/Dev/foolish-bench-results/campaign/trip3; L=$T/out/steps.log
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
IP=$(aws ec2 describe-instances --instance-ids $SRV --query 'Reservations[0].Instances[0].PrivateIpAddress' --output text)
LE=$(cat $T/out/launch_epoch)
STOP_S=$((38*60))
log() { echo "[$(date -u +%T) +$(elapsed)s] $*" | tee -a $L; }
elapsed() { echo $(( $(date +%s) - LE )); }
ssm_out() { local o; o=$(bash $S "$1" "$2" ${3:-300}); echo "$o" | tee -a $L; }
build_of() { case $1 in D|M1|M2|M3|M4) echo $1;; bpfM1) echo M1;; bpfM2) echo M2;; perfM3) echo M3;; esac; }
prof_of() { case $1 in bpf*) echo bpf;; perf*) echo cy;; *) echo none;; esac; }
mode_of() { echo split; }
pre_cmd() {   # pre_cmd LABEL BUILD MODE PROF
  local l=$1 b=$2 m=$3 p=$4 x=""
  [ $p != none ] && x="; bash /opt/mt/prof.sh start $p $l"
  echo "echo '== pre $l build=$b irq=$m prof=$p'; bash /opt/mt/irq.sh $m $l || { sleep 1; bash /opt/mt/irq.sh $m $l; }; bash /opt/mt/srv.sh start $l $b 8080 27 auto; bash /opt/mt/cpu.sh sample $l; sleep 1; grep -h pinned /var/tmp/mt/server_$l.log; grep Cpus_allowed_list /proc/\$(cat /var/tmp/mt/pid_$l)/status; bash /opt/mt/irq.sh show; bash /opt/mt/cpu.sh irqsnap $l start; sha256sum /opt/mt/cbase_$b | cut -c1-16$x"
}
post_cmd() {  # post_cmd LABEL P0 P1 PLAYMOVES PROF
  local l=$1 p0=$2 p1=$3 pm=$4 p=$5 x=""
  [ $p != none ] && x="bash /opt/mt/prof.sh stop $p $l; "
  echo "echo '== post $l'; ${x}bash /opt/mt/cpu.sh irqsnap $l end; bash /opt/mt/irq.sh show; bash /opt/mt/irq.sh pct $l; bash /opt/mt/srv.sh stop $l; echo '== play window'; bash /opt/mt/cpu.sh report $l $p0 $p1 $pm; bash /opt/mt/cpu.sh report2 $l $p0 $p1 $pm; grep -E 'VmHWM|VmRSS' /var/tmp/mt/status_${l}_end.txt; echo \"rssmax \$(sort -g -k2 /var/tmp/mt/rss_$l.log | tail -1)\"; grep -E 'inline dispatch|journal:|valve|ERROR' /var/tmp/mt/server_$l.log | tail -6; echo \"warn_lines \$(grep -c WARN /var/tmp/mt/server_$l.log)\"; free -m | head -2; echo '== end post $l'"
}
PENDING=""     # the post command of the last run, sent with the next pre (or alone by flush_post)
NRUN=0; SUMRUN=0
flush_post() { [ -n "$PENDING" ] && ssm_out $SRV "$PENDING" 600; PENDING=""; }
run1() {   # run1 LABEL ARM
  local l=$1 arm=$2 tmin=0 tmax=1 dur=25000 pw0=2 pw1=23 b m p o mv pm p0 p1 t0 pre
  b=$(build_of $arm); m=$(mode_of $arm); p=$(prof_of $arm); t0=$(date +%s)
  log "run $l arm=$arm build=$b irq=$m prof=$p knob=auto IP=$IP pairs=192 think=$tmin-$tmax dur_ms=$dur window=[t_go+$pw0,t_go+$pw1]"
  pre=$(pre_cmd $l $b $m $p); [ -n "$PENDING" ] && pre="$PENDING; $pre"; PENDING=""
  o=$(ssm_out $SRV "$pre" 600)
  if ! echo "$o" | grep -q "== pre $l" || ! echo "$o" | grep -q "start $l build $b pid [0-9]* 1 listening"; then log "PRE FAILED $l"; ssm_out $SRV "bash /opt/mt/srv.sh stop $l" 120; return 1; fi
  if echo "$o" | sed -n "/== pre $l/,\$p" | grep -q IRQ_FAIL; then log "IRQ FAILED $l"; ssm_out $SRV "bash /opt/mt/srv.sh stop $l" 120; return 1; fi
  if [ $p = cy ] && ! echo "$o" | grep -q "perf cycles running pid"; then log "PERF START FAILED $l"; ssm_out $SRV "bash /opt/mt/srv.sh stop $l" 120; return 1; fi
  if [ $p = bpf ] && ! echo "$o" | grep -q -E "bpftrace attached|FALLBACK strace"; then log "BPF START FAILED $l"; ssm_out $SRV "bash /opt/mt/prof.sh stop bpf $l; bash /opt/mt/srv.sh stop $l" 300; return 1; fi
  o=$(ssm_out $LOAD "PW0=$pw0 PW1=$pw1 bash /opt/mt/load.sh run $l $IP 192 $tmin $tmax 100000 $dur; bash /opt/mt/cpu.sh loadcheck $l" 420)
  mv=$(echo "$o" | grep -o 'moves=[0-9]*' | head -1 | cut -d= -f2)
  pm=$(echo "$o" | grep 'play moves=' | grep -o 'moves=[0-9]*' | cut -d= -f2)
  p0=$(echo "$o" | grep -o 'epoch0=[0-9.]*' | cut -d= -f2); p1=$(echo "$o" | grep -o 'epoch1=[0-9.]*' | cut -d= -f2)
  log "run $l moves=$mv play_moves=$pm play=[$p0,$p1]"
  PENDING=$(post_cmd $l ${p0:-0} ${p1:-0} ${pm:-1} $p)
  if [ -z "$pm" ] || [ -z "$p0" ]; then log "LOAD FAILED $l"; flush_post; return 1; fi
  NRUN=$((NRUN+1)); SUMRUN=$((SUMRUN + $(date +%s) - t0))
}
avg_run() { if [ $NRUN -gt 0 ]; then echo $(( SUMRUN / NRUN + 12 )); else echo 90; fi; }   # +12 s: the post step of the last run
extra_of() { case $1 in bpf*) echo 25;; perf*) echo 20;; *) echo 0;; esac; }   # bpftrace attach / perf.data push
cls() { case $1 in 3M4) echo 1;; 3D) echo 2;; *) echo 0;; esac; }
rounds() {
  local plan=() i j r a k ahead e av need
  for r in 1 2 3; do for a in D M1 M2 M3 M4; do plan+=("$r$a"); done; done
  plan+=("XbpfM1" "XbpfM2" "XperfM3")
  for ((i=0; i<${#plan[@]}; i++)); do
    r=${plan[$i]:0:1}; a=${plan[$i]:1}; k=$(cls ${plan[$i]}); e=$(elapsed); av=$(avg_run)
    need=$((e + av + $(extra_of $a)))
    if [ $need -gt $STOP_S ]; then log "STOP: elapsed ${e}s + run ${av}s passes minute 38; not starting ${plan[$i]} or later"; return 0; fi
    ahead=0; for ((j=i+1; j<${#plan[@]}; j++)); do [ $(cls ${plan[$j]}) -lt $k ] && ahead=$((ahead + av + $(extra_of ${plan[$j]:1}))); done
    if [ $k -gt 0 ] && [ $((need + ahead)) -gt $STOP_S ]; then log "DROP ${plan[$i]} (class $k): ${e}s + ${av}s + ${ahead}s of higher-priority runs passes minute 38"; continue; fi
    if [ $r = X ]; then lbl=$a; else lbl=r$r$a; fi
    run1 $lbl $a || { log "run $lbl failed, retrying once"; run1 $lbl $a || { log "$lbl failed twice: stopping the rounds"; flush_post; return 1; }; }
  done
}
for st in "$@"; do
  case $st in
  check) log check
    ssm_out $SRV "uname -r; nproc; free -m | head -3; echo chronyd \$(systemctl is-active chronyd); chronyc tracking 2>&1 | grep -E 'Leap|System time'; ls -l /opt/mt; sha256sum /opt/mt/cbase_* | awk '{print substr(\$1,1,16), \$2}'; test -f /opt/mt/READY && echo READY; test -f /opt/mt/UD_DONE && echo UD_DONE; echo irqbalance \$(systemctl is-active irqbalance); grep -E 'ena|ens5' /proc/interrupts; rpm -q perf bpftrace strace 2>&1 | tr '\n' ' '; echo; swapon --show; echo dmesg_oom_lines \$(dmesg | grep -c -i -E 'out of memory|oom-kill|killed process'); dmesg | grep -i -E 'oom|killed process' | tail -3"
    ssm_out $LOAD "ls -l /opt/mt | wc -l; test -f /opt/mt/UD_DONE && echo UD_DONE; grep -c failed /var/tmp/build_out.txt; tail -1 /var/tmp/build_out.txt; nproc; uname -r; echo chronyd \$(systemctl is-active chronyd); perf --version" ;;
  install) log install
    ssm_out $SRV "free -m | head -2; if ! swapon --show | grep -q swapfile; then fallocate -l 768M /swapfile && chmod 600 /swapfile && mkswap /swapfile >/dev/null && swapon /swapfile; fi; swapon --show; dnf install -y perf bpftrace strace > /var/tmp/dnf_srv.txt 2>&1; echo dnf_rc=\$?; tail -3 /var/tmp/dnf_srv.txt; perf --version; bpftrace --version; strace -V | head -1; echo chronyd \$(systemctl is-active chronyd); chronyc tracking 2>&1 | grep -E 'Leap|System time'; echo dmesg_oom_lines \$(dmesg | grep -c -i -E 'out of memory|oom-kill|killed process'); bash /opt/mt/prof.sh swapoff" 600 ;;
  symcheck) log symcheck
    ssm_out $SRV "bash /opt/mt/prof.sh symsetup M3; bash /opt/mt/srv.sh start symchk M3 8080 '' auto; grep -h pinned /var/tmp/mt/server_symchk.log" 120
    bash $S $LOAD "bash /opt/mt/load.sh run symchk $IP 16 0 1 100000 30000" 200 > $T/out/symchk_load.txt 2>&1 &
    lp=$!; sleep 12
    ssm_out $SRV "bash /opt/mt/prof.sh smoke 6 M3 | grep -v '^ *|' | grep -v '^ *-' | tail -16; bash /opt/mt/prof.sh start bpf symchk; sleep 3; bash /opt/mt/prof.sh stop bpf symchk; grep -c '^ENDTICK' /var/tmp/mt/bpf_symchk.txt; grep -m3 -E '^@sc\[' /var/tmp/mt/bpf_symchk.txt; bash /opt/mt/prof.sh swapoff" 240
    wait $lp; head -2 $T/out/symchk_load.txt | tee -a $L
    ssm_out $SRV "bash /opt/mt/srv.sh stop symchk; echo chronyd \$(systemctl is-active chronyd); echo dmesg_oom_lines \$(dmesg | grep -c -i -E 'out of memory|oom-kill|killed process')" ;;
  warm) run1 wD D; flush_post ;;
  rounds) rounds; flush_post ;;
  proc) log "proc perfM3 (load box) and server push in parallel"
    ( o=$(bash $S $SRV "bash /opt/mt/srv.sh push" 300); echo "srv push: $o" >> $L ) &
    sp=$!
    ssm_out $LOAD "bash /opt/mt/prof.sh process perfM3 M3" 900
    wait $sp; log "proc done" ;;
  push) log push; ssm_out $SRV "bash /opt/mt/srv.sh push" 300; ssm_out $LOAD "bash /opt/mt/load.sh push" 900 ;;
  sync) log sync; mkdir -p $T/out/raw; aws s3 sync --only-show-errors s3://$B/res/ $T/out/raw/; du -sh $T/out/raw | tee -a $L; find $T/out/raw -type f | wc -l | tee -a $L ;;
  esac
done
