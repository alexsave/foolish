#!/bin/bash
# cpu.sh sample LABEL | report LABEL T0 T1 MOVES | loadcheck LABEL   (E1 copy of the harness, pkg2; runs on either box)
# sample: a detached sampler of the server process LABEL (pid from /var/tmp/mt/pid_LABEL), every 0.2 s, appending to
#   /var/tmp/mt/cpu_LABEL.log:  S <epoch> <aggregate /proc/stat cpu jiffies: user nice system idle iowait irq softirq steal>
#                               P <epoch> <process utime stime, clock ticks (/proc/PID/stat fields 14 and 15)>
#                               T <epoch> <tid> <utime> <stime> <last cpu (field 39)> <comm>   (every /proc/PID/task/TID/stat)
# report: linearly interpolates every series at T0 and T1 (the load box's t_go and t_done, epoch seconds, so the window is
#   exactly the one the clients measured), then prints process and per-thread CPU seconds, CPU microseconds per move
#   (process total over MOVES) and the box busy share of the window.
# loadcheck: counts ready, done and failed client files in /var/tmp/mt/LABEL, so a run shows how many of the clients ran.
M=/opt/mt; R=/var/tmp/mt; CLK=$(getconf CLK_TCK)
bg() { local u=$1; shift; if [ -d /run/systemd/system ]; then systemd-run --quiet --collect --unit=$u bash -c "$*"; else nohup bash -c "$*" >/dev/null 2>&1 & fi; }
sampler() {   # sampler PID LABEL
  local p=$1 out=$R/cpu_$2.log t line rest u n sy id w irq sirq st d tid
  while kill -0 $p 2>/dev/null; do
    t=$EPOCHREALTIME
    read -r _ u n sy id w irq sirq st _ < /proc/stat
    echo "S $t $u $n $sy $id $w $irq $sirq $st" >> $out
    if read -r line < /proc/$p/stat 2>/dev/null; then rest=${line##*) }; set -- $rest; echo "P $t ${12} ${13}" >> $out; fi
    for d in /proc/$p/task/*; do
      tid=${d##*/}; read -r line < $d/stat 2>/dev/null || continue
      c=${line#*(}; c=${c%%)*}; rest=${line##*) }; set -- $rest
      echo "T $t $tid ${12} ${13} ${37} $c" >> $out
    done
    sleep 0.2
  done
}
case $1 in
sample) l=$2; p=$(cat $R/pid_$l); : > $R/cpu_$l.log; bg cbcpu-$l "bash $M/cpu.sh _sampler $p $l" ;;
_sampler) sampler $2 $3 ;;
report) l=$2; T0=$3; T1=$4; mv=$5
  awk -v T0=$T0 -v T1=$T1 -v CLK=$CLK -v MOVES=$mv -v LBL=$l '
  function at(k,T,f,  i,n) { n=cnt[k]; if (n==0) return 0; if (T<=tt[k,1]) return v[k,1,f]; if (T>=tt[k,n]) return v[k,n,f];
    for (i=1;i<n;i++) if (tt[k,i+1]>=T) return v[k,i,f]+(v[k,i+1,f]-v[k,i,f])*(T-tt[k,i])/(tt[k,i+1]-tt[k,i]); return v[k,n,f] }
  { if ($1=="S") { k="S"; s=3; e=10 } else if ($1=="P") { k="P"; s=3; e=4 } else if ($1=="T") { k="T" $3; s=4; e=5; cpu[k]=$6; comm[k]=$7 } else next;
    n=++cnt[k]; tt[k,n]=$2; for (f=s;f<=e;f++) v[k,n,f-s+1]=$f }
  END {
    w=T1-T0; pt=(at("P",T1,1)+at("P",T1,2)-at("P",T0,1)-at("P",T0,2))/CLK;
    printf "cpu label=%s window_s=%.3f proc_cpu_s=%.3f moves=%d cpu_us_per_move=%.2f cpu_busy_of_one_vcpu_pct=%.1f\n", LBL, w, pt, MOVES, pt*1e6/MOVES, 100*pt/w;
    tot=0; busy=0; for (f=1;f<=8;f++) { d=at("S",T1,f)-at("S",T0,f); tot+=d; if (f!=4 && f!=5) busy+=d }
    printf "box busy_pct=%.2f (idle+iowait excluded; 2 vCPUs)\n", 100*busy/tot;
    for (k in cnt) if (k ~ /^T/) { dt=(at(k,T1,1)+at(k,T1,2)-at(k,T0,1)-at(k,T0,2))/CLK; if (dt>0) printf "thread tid=%s comm=%s last_cpu=%s cpu_s=%.3f cpu_us_per_move=%.2f\n", substr(k,2), comm[k], cpu[k], dt, dt*1e6/MOVES }
  }' $R/cpu_$l.log | sort ;;
loadcheck) l=$2; cd $R/$l || exit 1
  echo "ready=$(ls rdv*/ready.* 2>/dev/null | wc -l) done=$(ls rdv*/done.* 2>/dev/null | wc -l) err_files_with_failed=$(grep -l failed *.err 2>/dev/null | wc -l) t_go=$(cat t_go 2>/dev/null) t_done=$(cat t_done 2>/dev/null)" ;;
esac
