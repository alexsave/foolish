#!/bin/bash
# run.sh STEP...: the Graviton confirmation's runs, driven from the Mac through SSM (ssm.sh). Every step's output is
# appended to out/steps.log. Builds: A=400a65d09 (before #480), B=57a40a1f5 (#480), C=1270832e0 (#481), D=f99698319 (#483).
D=/Users/alex/Dev/cdom-memtest/grav; source $D/state.env; S=$D/ssm.sh; L=$D/out/steps.log
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
hum2() {
  log "hum2 $1 build $2"
  on srv "bash /opt/mt/srv.sh start $1 $2 8080 40"
  on load "bash /opt/mt/load.sh run $1 $IP 192 2000 4000 40 30000" 300
  on srv "bash /opt/mt/srv.sh stop $1; cat /var/tmp/mt/capout_$1_mid.txt"
}
bot() {
  log "bot $1 build $2"
  on srv "SRVENV=CBASE_BOT_SPEEDUP=100 bash /opt/mt/srv.sh start $1 $2 8080 27"
  on load "bash /opt/mt/load.sh bot $1 $IP 64 25000" 200
  on srv "bash /opt/mt/srv.sh stop $1; cat /var/tmp/mt/capout_$1_mid.txt; grep -h . /var/tmp/mt/tls_$1_mid.txt"
}
idle() {
  log "idle $1 build $2"
  on srv "bash /opt/mt/srv.sh start $1 $2 8080; sleep 20; bash /opt/mt/srv.sh idle $1_idle; grep -h . /var/tmp/mt/tls_$1_idle.txt | tail -1; bash /opt/mt/srv.sh stop $1"
}
for st in "$@"; do
  case $st in
  check) log check; on srv "uname -r; nproc; cat /sys/kernel/mm/transparent_hugepage/enabled /sys/kernel/mm/transparent_hugepage/defrag; grep -E 'MemTotal|MemAvailable' /proc/meminfo; ls -l /opt/mt; test -f /opt/mt/READY && echo READY"
     on load "ls -l /opt/mt; cat /var/tmp/build_out.txt; nproc" ;;
  idle) for b in A B C D; do idle i_$b $b; done ;;
  sat_r1) for b in A B C D; do sat2 s1_$b $b; done ;;
  sat_r2) for b in D C B A; do sat2 s2_$b $b; done ;;
  sat_r3) for b in B D A C; do sat2 s3_$b $b; done ;;
  hum) for b in C A D B; do hum2 h_$b $b; done ;;
  bot) for x in C1:C D1:D D2:D C2:C; do bot b_${x%%:*} ${x#*:}; done ;;
  g8) log "g8 two instances of D"
      on srv "bash /opt/mt/srv.sh start g8a D 8080; sleep 15; grep -E 'MemAvailable|AnonPages|^Cached' /proc/meminfo; free -k | sed -n 2p; bash /opt/mt/srv.sh idle g8a_idle;
              bash /opt/mt/srv.sh start g8b D 8081; sleep 15; grep -E 'MemAvailable|AnonPages|^Cached' /proc/meminfo; free -k | sed -n 2p; bash /opt/mt/srv.sh idle g8b_idle; bash /opt/mt/srv.sh idle g8a2_idle;
              grep -E '^(Rss|Pss|Pss_Anon|Pss_File|Shared_Clean|Private_Clean|Private_Dirty):' /var/tmp/mt/rollup_g8a_idle.txt /var/tmp/mt/rollup_g8b_idle.txt /var/tmp/mt/rollup_g8a2_idle.txt;
              bash /opt/mt/srv.sh stop g8b; sleep 3; grep -E 'MemAvailable|AnonPages' /proc/meminfo; free -k | sed -n 2p; bash /opt/mt/srv.sh stop g8a" ;;
  g11) log "g11 rematch after eviction, build D"
       on srv "bash /opt/mt/srv.sh start g11 D 8080"
       on load "bash /opt/mt/load.sh rematch g11 $IP 30 2000,50000,70000 3" 400
       on srv "bash /opt/mt/srv.sh stop g11" ;;
  g7) log "g7 standard credits, build D"
      aws ec2 modify-instance-credit-specification --instance-credit-specifications InstanceId=$SRV,CpuCredits=standard --query 'SuccessfulInstanceCreditSpecifications' --output text | tee -a $L
      aws ec2 describe-instance-credit-specifications --instance-ids $SRV --output text | tee -a $L
      on srv "bash /opt/mt/srv.sh start g7 D 8080; head -1 /proc/stat > /var/tmp/mt/stat0_g7.txt; cat /var/tmp/mt/stat0_g7.txt"
      on load "bash /opt/mt/load.sh run g7 $IP 8 0 1 100000 ${G7_MS:-150000}" 400
      on srv "head -1 /proc/stat | tee /var/tmp/mt/stat1_g7.txt; bash /opt/mt/srv.sh stop g7" ;;
  push) log push; on srv "bash /opt/mt/srv.sh push"; on load "bash /opt/mt/load.sh push" 300 ;;
  esac
done
