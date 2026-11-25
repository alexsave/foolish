#!/bin/bash
# prof.sh: trip 3 copy of trip1/pkg/prof.sh. Changes: the binary is an argument (BIN, e.g. M3: /opt/mt/cbase_M3 and its
# unstripped twin /opt/mt/cbase_M3.unstripped) instead of the fixed cbase_D; `start` records the profiler's PID (the
# transient unit's MainPID) in $R/profpid_LABEL and `stop` sends SIGINT to that PID only (no pkill); `swapoff` added.
# Trip 1 header:
# prof.sh: trip 1 profilers. On the SERVER (nano):
#   info                         box facts: versions, memory, irqbalance, ENA IRQs and their affinity, RPS/XPS masks
#   symsetup BIN                 adds /opt/mt/cbase_BIN.unstripped to perf's build-id cache (same build id as the stripped cbase_BIN)
#   smoke SECS BIN               perf record -a -g cycles for SECS seconds, then checks that cbase_BIN user symbols resolve
#   start cy|clk|bpf LABEL       starts perf record (cycles or cpu-clock) or bpftrace (trip.bt) on the server LABEL, detached;
#                                writes clk_LABEL.txt (epoch and CLOCK_MONOTONIC taken together) just before
#   stop cy|clk|bpf LABEL        stops it (SIGINT, waits), then for perf pushes perf_LABEL.data, kallsyms and modules to the bucket
#   irq CPU LABEL                stops irqbalance and points every ENA interrupt (ena-mgmnt and the queue IRQs) at CPU
# On the LOAD box:
#   process LABEL BIN            pulls perf_LABEL.data, the server's kallsyms and the unstripped twin from the bucket, writes
#                                flat (no callchain) and full perf script dumps and two perf reports (gzipped), pushes them
# perf data lives in /var/tmp/mtperf (not /var/tmp/mt, so srv.sh push stays the E1 one).
export HOME=${HOME:-/root}   # SSM runs commands with HOME unset, and perf keeps its build-id cache in $HOME/.debug
M=/opt/mt; R=/var/tmp/mt; PD=/var/tmp/mtperf; mkdir -p $R $PD; B=$(cat $M/bucket); E=https://s3.dualstack.us-east-1.amazonaws.com
bg() { local u=$1; shift; if [ -d /run/systemd/system ]; then systemd-run --quiet --collect --unit=$u bash -c "$*"; else nohup bash -c "$*" >/dev/null 2>&1 & fi; }
clk() { python3 -c 'import time; e=time.time(); m=time.clock_gettime(time.CLOCK_MONOTONIC); print("epoch %.6f mono %.6f" % (e, m))'; }
enairqs() { awk -F: '/ena|eth[0-9]|Tx-Rx|-Tx|-Rx/ { gsub(/ /,"",$1); print $1 }' /proc/interrupts; }
waitpid_() { local n=0; while [ -n "$1" ] && kill -0 $1 2>/dev/null && [ $n -lt ${2:-300} ]; do sleep 0.2; n=$((n+1)); done; [ -n "$1" ] && kill -0 $1 2>/dev/null && echo "WARN pid $1 still running"; true; }
waitgone() { local n=0; while pgrep -x $1 >/dev/null && [ $n -lt ${2:-300} ]; do sleep 0.2; n=$((n+1)); done; pgrep -x $1 >/dev/null && echo "WARN $1 still running"; true; }
case $1 in
info)
  uname -r; nproc; free -m | head -2; perf --version 2>&1; bpftrace --version 2>&1; strace -V 2>&1 | head -1; python3 -V
  echo "irqbalance: $(systemctl is-active irqbalance 2>&1)"; cat /proc/sys/kernel/kptr_restrict /proc/sys/kernel/perf_event_paranoid
  for i in $(enairqs); do echo "irq $i $(grep -E "^ *$i:" /proc/interrupts | awk '{$1=""; print}' | tr -s ' ') aff=$(cat /proc/irq/$i/smp_affinity_list) eff=$(cat /proc/irq/$i/effective_affinity_list 2>/dev/null)"; done
  for q in /sys/class/net/e*/queues/*; do echo "$q rps=$(cat $q/rps_cpus 2>/dev/null) xps=$(cat $q/xps_cpus 2>/dev/null)"; done
  ls /sys/kernel/btf/vmlinux 2>&1 ;;
symsetup)
  bn=$2; echo "HOME=[$HOME]"; perf buildid-cache -a $M/cbase_$bn.unstripped 2>&1; perf buildid-cache -l 2>&1 | head; file $M/cbase_$bn $M/cbase_$bn.unstripped 2>/dev/null | tr ',' '\n' | grep -E 'BuildID|stripped' ;;
smoke)
  s=${2:-6}; bn=$3; cd $PD || exit; perf record -a -g -F 999 -e cycles -k CLOCK_MONOTONIC --no-buildid-cache -o $PD/smoke.data -- sleep $s > $PD/smoke.log 2>&1; tail -2 $PD/smoke.log
  perf buildid-list -i $PD/smoke.data 2>&1 | head
  perf report -i $PD/smoke.data --stdio --no-children --sort comm,dso,sym 2>/dev/null | grep -v '^#' | grep -v '^$' | head -25
  echo "== cbase_$bn samples: resolved vs [unknown] (by period)"
  perf script -i $PD/smoke.data -G -F period,ip,sym,dso 2>/dev/null | awk -v bn="cbase_$bn" '$NF ~ bn { t+=$1; if ($(NF-1)=="[unknown]") u+=$1; n++ } END { printf "%s samples=%d unknown_share=%.3f\n", bn, n, (t? u/t : -1) }'
  echo "== top resolved cbase_$bn symbols in the smoke"; perf script -i $PD/smoke.data -G -F period,ip,sym,dso 2>/dev/null | awk -v bn="cbase_$bn" '$NF ~ bn { c[$(NF-1)]+=$1 } END { for (k in c) print c[k], k }' | sort -rn | head -8 ;;
start)
  k=$2; l=$3; p=$(cat $R/pid_$l); clk > $R/clk_prof_$l.txt; echo "pid $p" >> $R/clk_prof_$l.txt
  case $k in
  cy|clk) ev=cycles; [ $k = clk ] && ev=cpu-clock
    bg cbperf-$l "cd $PD; exec perf record -a -g -F 999 -e $ev -k CLOCK_MONOTONIC --no-buildid-cache -o $PD/perf_$l.data > $PD/perf_$l.log 2>&1"
    sleep 2; pp=$(systemctl show -p MainPID --value cbperf-$l 2>/dev/null); echo "$pp" > $R/profpid_$l
    [ -n "$pp" ] && [ "$pp" != 0 ] && kill -0 $pp 2>/dev/null && echo "perf $ev running pid $pp" || { echo "PERF FAILED"; cat $PD/perf_$l.log; } ;;
  bpf)
    if ! swapon --show | grep -q swapfile; then fallocate -l 768M /swapfile && chmod 600 /swapfile && mkswap /swapfile >/dev/null && swapon /swapfile; fi; free -m | head -3
    ok=
    for syn in arrow dot; do
      if [ $syn = dot ]; then sed 's/args->/args./g' $M/trip.bt > $M/trip_dot.bt; f=$M/trip_dot.bt; else f=$M/trip.bt; fi
      : > $R/bpf_$l.txt; : > $R/bpf_$l.err
      bg cbbpf-$l "exec bpftrace $f $p > $R/bpf_$l.txt 2> $R/bpf_$l.err"
      sleep 0.3; bp=$(systemctl show -p MainPID --value cbbpf-$l 2>/dev/null); echo "$bp" > $R/profpid_$l
      n=0; while [ $n -lt 300 ]; do grep -q '^BT_BEGIN' $R/bpf_$l.txt && { ok=1; break; }; kill -0 $bp 2>/dev/null || break; sleep 0.2; n=$((n+1)); done
      [ -n "$ok" ] && { echo "bpftrace attached ($syn syntax) after $((n/5)) s pid $bp"; head -3 $R/bpf_$l.txt; break; }
      echo "bpftrace failed with $syn syntax:"; head -20 $R/bpf_$l.err; kill -INT $bp 2>/dev/null; waitpid_ $bp 50
    done
    if [ -z "$ok" ]; then
      echo "FALLBACK strace -c -f for 5 s, starting 45 s after start"
      : > $R/strace_fallback_$l
      bg cbstrace-$l "sleep 45; echo strace_start \$(date +%s.%N) > $R/strace_$l.time; timeout -s INT 5 strace -c -f -p $p -o $R/strace_$l.txt; echo strace_end \$(date +%s.%N) >> $R/strace_$l.time"
    fi
    free -m | head -2 ;;
  esac ;;
stop)
  k=$2; l=$3; clk > $R/clk_profstop_$l.txt
  case $k in
  cy|clk)
    pp=$(cat $R/profpid_$l); kill -INT $pp; waitpid_ $pp 600; tail -3 $PD/perf_$l.log; ls -l $PD/perf_$l.data
    cat /proc/kallsyms > $PD/kallsyms_$l.txt; cat /proc/modules > $PD/modules_$l.txt
    for f in perf_$l.data kallsyms_$l.txt modules_$l.txt perf_$l.log; do aws s3 cp --quiet $PD/$f s3://$B/res/perf/$f --endpoint-url $E; done
    echo "pushed perf $l" ;;
  bpf)
    if [ -f $R/strace_fallback_$l ]; then waitgone strace 100; cat $R/strace_$l.time; head -30 $R/strace_$l.txt
    else bp=$(cat $R/profpid_$l); kill -INT $bp; waitpid_ $bp 300; echo "bpf lines $(wc -l < $R/bpf_$l.txt)"; tail -3 $R/bpf_$l.err; grep -c '^TICK' $R/bpf_$l.txt; fi
    free -m | head -3; dmesg | grep -i -E 'oom|killed process' | tail -3 ;;
  esac ;;
swapoff)
  swapoff /swapfile 2>/dev/null; swapon --show; free -m | head -3 ;;
irq)
  c=$2; l=$3; systemctl stop irqbalance 2>/dev/null; echo "irqbalance: $(systemctl is-active irqbalance 2>&1)"
  for i in $(enairqs); do echo $c > /proc/irq/$i/smp_affinity_list 2>&1 || echo "irq $i: set failed"; echo "irq $i aff=$(cat /proc/irq/$i/smp_affinity_list) eff=$(cat /proc/irq/$i/effective_affinity_list 2>/dev/null)"; done
  echo "$c" > $R/irqset_$l.txt ;;
process)
  l=$2; bn=$3; P=/var/tmp/proc/$l; mkdir -p $P; cd $P || exit
  for f in perf_$l.data kallsyms_$l.txt modules_$l.txt; do aws s3 cp --quiet s3://$B/res/perf/$f $P/$f --endpoint-url $E; done
  [ -f $M/cbase_$bn ] || aws s3 cp --quiet s3://$B/bin/cbase_$bn.unstripped $M/cbase_$bn --endpoint-url $E; chmod +x $M/cbase_$bn
  perf buildid-cache -a $M/cbase_$bn 2>&1 | head -2; perf buildid-cache -l 2>&1 | head -3
  K="--kallsyms=$P/kallsyms_$l.txt"
  perf buildid-list -i perf_$l.data > buildids_$l.txt 2>&1
  perf report -i perf_$l.data $K --header-only > header_$l.txt 2>&1
  perf script -i perf_$l.data $K --ns -G -F comm,pid,tid,cpu,time,period,event,ip,sym,dso 2> script_flat_$l.err | gzip -1 > flat_$l.txt.gz
  perf script -i perf_$l.data $K --ns -F comm,pid,tid,cpu,time,period,event,ip,sym,dso 2> script_cc_$l.err | gzip -1 > cc_$l.txt.gz
  perf report -i perf_$l.data $K --stdio --no-children --sort comm,dso,sym 2> /dev/null | gzip -1 > rep_self_$l.txt.gz
  perf report -i perf_$l.data $K --stdio --children --sort comm,dso,sym 2> /dev/null | gzip -1 > rep_children_$l.txt.gz
  zcat flat_$l.txt.gz | awk '{ p=0; for (i=1;i<NF;i++) if ($i ~ /^[0-9]+[.][0-9]+:$/) { p=$(i+1); break } d=$NF; s=$(NF-1); T[d]+=p; if (s=="[unknown]") U[d]+=p; A+=p } END { for (d in T) if (T[d]>A*0.001) printf "dso %s share=%.4f unknown_within=%.4f\n", d, T[d]/A, U[d]/T[d] }' | sort -t= -k2 -rn > resolve_$l.txt
  cat resolve_$l.txt; head -3 script_flat_$l.err; ls -l
  for f in *_$l.txt *_$l.txt.gz *_$l.err; do aws s3 cp --quiet $f s3://$B/res/proc/$l/$f --endpoint-url $E; done; echo "pushed proc $l" ;;
esac
