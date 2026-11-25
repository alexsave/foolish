#!/bin/bash
# irq.sh MODE [LABEL]: trip 2, on the nano. Stops irqbalance and pins the ENA interrupts, then prints the resulting affinity.
#   split: ens5-Tx-Rx-0 on CPU 1, ens5-Tx-Rx-1 on CPU 0, ena-mgmnt on CPU 1 (trip 1's base2 layout: about half on CPU 0)
#   cpu1:  ens5-Tx-Rx-0, ens5-Tx-Rx-1 and ena-mgmnt all on CPU 1
#   show:  prints irqbalance state and the affinity only (changes nothing)
#   pct LABEL: the interrupt counts per CPU between cpu.sh irqsnap LABEL start and end, and the share of queue IRQs on CPU 0
# Exits 1 (and prints IRQ_FAIL) if irqbalance is still running or an affinity is not what was asked for.
# With LABEL, writes the mode to /var/tmp/mt/irqmode_LABEL.txt.
R=${IRQ_TEST_R:-/var/tmp/mt}; mkdir -p $R
mode=$1; l=${2:-}
if [ "$mode" = pct ]; then   # pct LABEL: per-IRQ CPU0/CPU1 counts between cpu.sh irqsnap LABEL start and end, and the queue share on CPU 0
  awk 'FNR==1 { f++ } $NF ~ /^(ens5-Tx-Rx-[01]|ena-mgmnt)/ { k=$NF; sub(/@.*/,"",k); c0[f,k]=$2; c1[f,k]=$3; ks[k]=1 }
       END { for (k in ks) { d0=c0[2,k]-c0[1,k]; d1=c1[2,k]-c1[1,k]; printf "irqdelta %s cpu0=%d cpu1=%d\n", k, d0, d1; if (k ~ /Tx-Rx/) { q0+=d0; q1+=d1 } }
             printf "irqpct queue_on_cpu0_pct=%.1f queue_total=%d\n", (q0+q1 ? 100*q0/(q0+q1) : -1), q0+q1 }' $R/irq_${l}_start.txt $R/irq_${l}_end.txt | sort
  exit 0
fi
irqnum() { awk -v n="$1" '$NF ~ n { sub(":","",$1); print $1; exit }' /proc/interrupts; }
q0=$(irqnum '^ens5-Tx-Rx-0$'); q1=$(irqnum '^ens5-Tx-Rx-1$'); mg=$(irqnum '^ena-mgmnt')
[ -n "$q0" ] && [ -n "$q1" ] && [ -n "$mg" ] || { echo "IRQ_FAIL names not found: q0=$q0 q1=$q1 mgmt=$mg"; grep -E 'ena|ens|eth' /proc/interrupts; exit 1; }
case $mode in
split) want0=1; want1=0; wantm=1 ;;
cpu1)  want0=1; want1=1; wantm=1 ;;
show)  want0=; want1=; wantm= ;;
*) echo "usage: irq.sh split|cpu1|show [LABEL]"; exit 2 ;;
esac
if [ $mode != show ]; then
  systemctl stop irqbalance 2>/dev/null
  n=0; while pgrep -x irqbalance >/dev/null && [ $n -lt 25 ]; do sleep 0.2; n=$((n+1)); done
  echo $want0 > /proc/irq/$q0/smp_affinity_list || echo "IRQ_FAIL write q0"
  echo $want1 > /proc/irq/$q1/smp_affinity_list || echo "IRQ_FAIL write q1"
  echo $wantm > /proc/irq/$mg/smp_affinity_list || echo "IRQ_FAIL write mgmt"
  [ -n "$l" ] && echo "$mode q0=$q0:$want0 q1=$q1:$want1 mgmt=$mg:$wantm $EPOCHREALTIME" > $R/irqmode_$l.txt
fi
ia=$(systemctl is-active irqbalance 2>&1); ip=$(pgrep -x irqbalance | wc -l)
a0=$(cat /proc/irq/$q0/smp_affinity_list); a1=$(cat /proc/irq/$q1/smp_affinity_list); am=$(cat /proc/irq/$mg/smp_affinity_list)
e0=$(cat /proc/irq/$q0/effective_affinity_list 2>/dev/null); e1=$(cat /proc/irq/$q1/effective_affinity_list 2>/dev/null); em=$(cat /proc/irq/$mg/effective_affinity_list 2>/dev/null)
echo "irq mode=$mode irqbalance=$ia procs=$ip ens5-Tx-Rx-0(irq $q0) aff=$a0 eff=$e0 ens5-Tx-Rx-1(irq $q1) aff=$a1 eff=$e1 ena-mgmnt(irq $mg) aff=$am eff=$em"
bad=
[ "$ip" = 0 ] || bad="irqbalance running"
if [ $mode != show ]; then
  [ "$e0" = "$want0" ] && [ "$e1" = "$want1" ] && [ "$em" = "$wantm" ] || bad="$bad effective affinity differs"
fi
[ -z "$bad" ] || { echo "IRQ_FAIL $bad"; exit 1; }
echo IRQ_OK
