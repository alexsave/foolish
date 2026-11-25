#!/bin/bash
# calrun.sh: on the nano, idle (no cbase running). Fetch calbench from the run bucket, record the box (lscpu, cpuinfo,
# sysfs cache levels), then 3 passes of env core mem str stream, 180 s apart (CAL_GAP_S as in the README's 2026-10-20
# sqlite run). Output in /var/tmp/cal; the TSVs and runner.log are pushed to the bucket under res/cal/.
B=$(cat /opt/mt/bucket); E=https://s3.dualstack.us-east-1.amazonaws.com
export CAL_CPU=1 CAL_DIR=/var/tmp/cal
R=/var/tmp/cal; mkdir -p $R; cd $R
aws s3 cp --quiet s3://$B/bin/calbench ./calbench --endpoint-url $E && chmod +x calbench
{
  date -u; uname -a; echo; lscpu; echo; cat /proc/cpuinfo; echo
  for c in /sys/devices/system/cpu/cpu0/cache/index*; do echo "$c level=$(cat $c/level) type=$(cat $c/type) size=$(cat $c/size) line=$(cat $c/coherency_line_size) ways=$(cat $c/ways_of_associativity) sets=$(cat $c/number_of_sets) shared=$(cat $c/shared_cpu_list)"; done
  cat /sys/kernel/mm/transparent_hugepage/enabled /sys/devices/system/clocksource/clocksource0/current_clocksource
  free -m; ls /sys/bus/event_source/devices/ 2>&1
} > spec.txt 2>&1
for pass in 1 2 3; do
  for g in env core mem str stream; do
    s=$(date +%s); ./calbench $g $pass > p${pass}_$g.tsv 2> p${pass}_$g.err; rc=$?
    echo "$(date -u +%FT%TZ) pass $pass group $g rc=$rc secs=$(( $(date +%s)-s ))" >> runner.log
  done
  [ $pass = 3 ] || sleep 180
done
echo "$(date -u +%FT%TZ) ALLDONE" >> runner.log
aws s3 cp --quiet --recursive $R s3://$B/res/cal/ --endpoint-url $E
echo pushed
