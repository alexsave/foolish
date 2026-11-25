#!/bin/bash
# cpu.sh sample LABEL | report LABEL T0 T1 MOVES | report2 LABEL T0 T1 MOVES | irqsnap LABEL TAG | loadcheck LABEL
#        lsample LABEL | lstop LABEL | lreport LABEL T0 T1
# Trip 1 copy of grav/pkg2/cpu.sh (E1). The S/P/T lines and `report` are unchanged; added:
# sample: every 0.2 s also
#   C <epoch> cpuN user nice system idle iowait irq softirq steal guest guest_nice   (per-CPU lines of /proc/stat)
#   R <epoch> procs_running procs_blocked
#   Q <epoch> <tid> <on_cpu_ns> <runq_wait_ns> <timeslices> <voluntary_ctxt_switches> <nonvoluntary_ctxt_switches>
#     (/proc/PID/task/TID/schedstat and the last two lines of .../status)
# report2: per-thread exact on-CPU ns and run-queue wait per move (Q, interpolated at T0 and T1) and per-CPU
#   busy/irq/softirq/steal/idle shares of the window (C). busy = everything but idle and iowait (steal included, as E1).
# irqsnap LABEL TAG: copies /proc/interrupts and /proc/softirqs to irq_LABEL_TAG.txt and softirq_LABEL_TAG.txt.
# lsample/lstop/lreport: the same per-CPU C and R lines for the LOAD box (no process), to lcpu_LABEL.log; lreport
#   prints the busy share of every CPU and the mean procs_running in [T0, T1].
M=/opt/mt; R=/var/tmp/mt; mkdir -p $R; CLK=$(getconf CLK_TCK)
bg() { local u=$1; shift; if [ -d /run/systemd/system ]; then systemd-run --quiet --collect --unit=$u bash -c "$*"; else nohup bash -c "$*" >/dev/null 2>&1 & fi; }
statlines() {   # statlines EPOCH: C and R lines of /proc/stat appended to $buf
  local ln; local -a ST; mapfile -t ST < /proc/stat
  for ln in "${ST[@]}"; do case $ln in cpu[0-9]*) buf+="C $1 $ln"$'\n';; procs_running*) pr=${ln#* };; procs_blocked*) buf+="R $1 $pr ${ln#* }"$'\n';; esac; done
}
sampler() {   # sampler PID LABEL
  local p=$1 out=$R/cpu_$2.log t line rest u n sy id w irq sirq st d tid buf pr sr v nv c
  local -a SS
  while kill -0 $p 2>/dev/null; do
    t=$EPOCHREALTIME
    read -r _ u n sy id w irq sirq st _ < /proc/stat
    buf="S $t $u $n $sy $id $w $irq $sirq $st"$'\n'
    statlines $t
    if read -r line < /proc/$p/stat 2>/dev/null; then rest=${line##*) }; set -- $rest; buf+="P $t ${12} ${13}"$'\n'; fi
    for d in /proc/$p/task/*; do
      tid=${d##*/}; read -r line < $d/stat 2>/dev/null || continue
      c=${line#*(}; c=${c%%)*}; rest=${line##*) }; set -- $rest
      buf+="T $t $tid ${12} ${13} ${37} $c"$'\n'
      read -r sr < $d/schedstat 2>/dev/null || continue
      mapfile -t SS < $d/status 2>/dev/null || continue
      v=${SS[-2]##*:}; nv=${SS[-1]##*:}
      buf+="Q $t $tid $sr $((v)) $((nv))"$'\n'
    done
    printf '%s' "$buf" >> $out
    sleep 0.2
  done
}
lsampler() {   # lsampler LABEL: until $R/lstop_LABEL exists
  local out=$R/lcpu_$1.log t buf pr
  while [ ! -f $R/lstop_$1 ]; do t=$EPOCHREALTIME; buf=; statlines $t; printf '%s' "$buf" >> $out; sleep 0.2; done
}
# at(): linear interpolation of series k, field f, at time T (the E1 report's function)
AT='function at(k,T,f,  i,n) { n=cnt[k]; if (n==0) return 0; if (T<=tt[k,1]) return v[k,1,f]; if (T>=tt[k,n]) return v[k,n,f];
    for (i=1;i<n;i++) if (tt[k,i+1]>=T) return v[k,i,f]+(v[k,i+1,f]-v[k,i,f])*(T-tt[k,i])/(tt[k,i+1]-tt[k,i]); return v[k,n,f] }'
CREP='if ($1=="C") { k="C" $3; n=++cnt[k]; tt[k,n]=$2; for (f=4;f<=11;f++) v[k,n,f-3]=$f; cpus[k]=1 }
      if ($1=="R") { if ($2>=T0 && $2<=T1) { rs+=$3; rn++; if ($3>rmax) rmax=$3 } }'
CEND='for (k in cpus) { tot=0; for (f=1;f<=8;f++) { d[f]=at(k,T1,f)-at(k,T0,f); tot+=d[f] } if (tot<=0) continue;
      printf "%s %s busy_pct=%.1f user=%.1f nice=%.1f system=%.1f irq=%.1f softirq=%.1f steal=%.1f idle=%.1f iowait=%.1f\n", TAGC, substr(k,2), 100*(d[1]+d[2]+d[3]+d[6]+d[7]+d[8])/tot, 100*d[1]/tot, 100*d[2]/tot, 100*d[3]/tot, 100*d[6]/tot, 100*d[7]/tot, 100*d[8]/tot, 100*d[4]/tot, 100*d[5]/tot }
      if (rn) printf "%s procs_running_mean=%.2f max=%d samples=%d\n", TAGC, rs/rn, rmax, rn'
case $1 in
sample) l=$2; p=$(cat $R/pid_$l); : > $R/cpu_$l.log; bg cbcpu-$l "bash $M/cpu.sh _sampler $p $l" ;;
_sampler) sampler $2 $3 ;;
lsample) l=$2; rm -f $R/lstop_$l; : > $R/lcpu_$l.log; bg cblcpu-$l "bash $M/cpu.sh _lsampler $l" ;;
_lsampler) lsampler $2 ;;
lstop) touch $R/lstop_$2 ;;
irqsnap) { cat /proc/interrupts; echo "EPOCH $EPOCHREALTIME"; } > $R/irq_$2_$3.txt; cat /proc/softirqs > $R/softirq_$2_$3.txt ;;
report) l=$2; T0=$3; T1=$4; mv=$5
  awk -v T0=$T0 -v T1=$T1 -v CLK=$CLK -v MOVES=$mv -v LBL=$l "$AT"'
  { if ($1=="S") { k="S"; s=3; e=10 } else if ($1=="P") { k="P"; s=3; e=4 } else if ($1=="T") { k="T" $3; s=4; e=5; cpu[k]=$6; comm[k]=$7 } else next;
    n=++cnt[k]; tt[k,n]=$2; for (f=s;f<=e;f++) v[k,n,f-s+1]=$f }
  END {
    w=T1-T0; pt=(at("P",T1,1)+at("P",T1,2)-at("P",T0,1)-at("P",T0,2))/CLK;
    printf "cpu label=%s window_s=%.3f proc_cpu_s=%.3f moves=%d cpu_us_per_move=%.2f cpu_busy_of_one_vcpu_pct=%.1f\n", LBL, w, pt, MOVES, pt*1e6/MOVES, 100*pt/w;
    tot=0; busy=0; for (f=1;f<=8;f++) { d=at("S",T1,f)-at("S",T0,f); tot+=d; if (f!=4 && f!=5) busy+=d }
    printf "box busy_pct=%.2f (idle+iowait excluded; 2 vCPUs)\n", 100*busy/tot;
    for (k in cnt) if (k ~ /^T/) { dt=(at(k,T1,1)+at(k,T1,2)-at(k,T0,1)-at(k,T0,2))/CLK; if (dt>0) printf "thread tid=%s comm=%s last_cpu=%s cpu_s=%.3f cpu_us_per_move=%.2f\n", substr(k,2), comm[k], cpu[k], dt, dt*1e6/MOVES }
  }' $R/cpu_$l.log | sort ;;
report2) l=$2; T0=$3; T1=$4; mv=$5
  awk -v T0=$T0 -v T1=$T1 -v MOVES=$mv -v LBL=$l -v TAGC=srvcpu "$AT"'
  { '"$CREP"'
    if ($1=="T") comm["Q" $3]=$7
    if ($1=="Q") { k="Q" $3; n=++cnt[k]; tt[k,n]=$2; for (f=4;f<=8;f++) v[k,n,f-3]=$f; th[k]=1 } }
  END {
    w=T1-T0; printf "report2 label=%s window_s=%.3f moves=%d\n", LBL, w, MOVES; sr=0; sw=0
    for (k in th) { r=at(k,T1,1)-at(k,T0,1); q=at(k,T1,2)-at(k,T0,2); sl=at(k,T1,3)-at(k,T0,3); vo=at(k,T1,4)-at(k,T0,4); nv=at(k,T1,5)-at(k,T0,5); if (r<=0) continue; sr+=r; sw+=q
      printf "sched tid=%s comm=%s oncpu_s=%.3f oncpu_us_per_move=%.3f runq_wait_s=%.3f runq_wait_us_per_move=%.3f slices_per_move=%.4f vol_cs_per_move=%.4f invol_cs_per_move=%.4f\n", substr(k,2), comm[k], r/1e9, r/1e3/MOVES, q/1e9, q/1e3/MOVES, sl/MOVES, vo/MOVES, nv/MOVES }
    printf "sched total oncpu_us_per_move=%.3f runq_wait_us_per_move=%.3f\n", sr/1e3/MOVES, sw/1e3/MOVES
    '"$CEND"'
  }' $R/cpu_$l.log | sort ;;
lreport) l=$2; T0=$3; T1=$4
  awk -v T0=$T0 -v T1=$T1 -v TAGC=loadcpu "$AT"'{ '"$CREP"' } END { '"$CEND"'
    tb=0; tt2=0; for (k in cpus) { for (f=1;f<=8;f++) { x=at(k,T1,f)-at(k,T0,f); tt2+=x; if (f!=4 && f!=5) tb+=x } }
    if (tt2>0) printf "loadcpu all busy_pct_of_all_vcpus=%.1f\n", 100*tb/tt2 }' $R/lcpu_$l.log | sort ;;
loadcheck) l=$2; cd $R/$l || exit 1
  echo "ready=$(ls rdv*/ready.* 2>/dev/null | wc -l) done=$(ls rdv*/done.* 2>/dev/null | wc -l) err_files_with_failed=$(grep -l failed *.err 2>/dev/null | wc -l) t_go=$(cat t_go 2>/dev/null) t_done=$(cat t_done 2>/dev/null)" ;;
esac
