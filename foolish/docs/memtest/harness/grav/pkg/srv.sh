#!/bin/bash
# srv.sh start LABEL BUILD [PORT] [CAPDELAY_S] | cap LABEL | idle LABEL | stop LABEL | push
# On the nano. A fresh static musl `cbase serve` (/opt/mt/cbase_BUILD) with the cbase.service env shape plus the
# vm_boot keys (throwaway secrets made on the box, never printed) and the bench limits raised; THP left as the host has it.
# The sampler logs "epoch VmRSS VmHWM RssAnon" (kB) every 0.2 s. CAPDELAY_S: a detached capture (status, smaps,
# smaps_rollup, per-thread TLS residency) that many seconds after start. Results go to /var/tmp/mt, pushed to S3 by push.
M=/opt/mt; R=/var/tmp/mt; mkdir -p $R; B=$(cat $M/bucket)
op=$1; l=$2
# bg UNIT CMD: CMD detached from the caller (an SSM command's processes end with it): a transient systemd unit where
# systemd runs, else nohup (the local container test).
bg() { local u=$1; shift; if [ -d /run/systemd/system ]; then systemd-run --quiet --collect --unit=$u -p LimitNOFILE=65536 bash -c "$*"; else nohup bash -c "$*" >/dev/null 2>&1 & fi; }
pid_of() { cat $R/pid_$1 2>/dev/null; }
cap() {   # cap LABEL PID
  local c=$1 p=$2
  date +%s.%N > $R/tcap_$c.txt
  cat /proc/$p/status > $R/status_$c.txt; cat /proc/$p/smaps > $R/smaps_$c.txt; cat /proc/$p/smaps_rollup > $R/rollup_$c.txt
  for t in /proc/$p/task/*; do echo "$(basename $t) $(cat $t/comm)"; done > $R/tasks_$c.txt
  $M/tlsres $p > $R/tls_$c.txt 2>&1
  grep -E "MemTotal|MemFree|MemAvailable|^Cached|AnonPages|AnonHugePages" /proc/meminfo > $R/meminfo_$c.txt
  grep -E "VmRSS|RssAnon|RssFile" $R/status_$c.txt | tr -s ' \t' ' ' | tr '\n' ' '; echo
}
case $op in
start)
  b=$3; port=${4:-8080}; cd_s=${5:-}
  D=/var/lib/cbase_$l; rm -rf $D; mkdir -p $D; chmod 700 $D; rm -f $R/rss_$l.log
  [ -f $M/secrets.env ] || { umask 077; { echo "CBASE_JWT_SECRET=$(openssl rand -hex 32)"; echo "CBASE_ADMIN_TOKEN=$(openssl rand -hex 32)"; } > $M/secrets.env; }
  bg cbsrv-$l "set -a; . $M/secrets.env; set +a; cd $D; umask 077; ulimit -n 65536
    exec env CBASE_PUBLIC_URL=http://nano.example \
      CBASE_STORE=sqlite CBASE_DB=$D/cbase.sqlite CBASE_LISTEN=:::$port SSL_CERT_FILE=/etc/pki/tls/certs/ca-bundle.crt \
      CBASE_RATED_MIN_AGE=0 CBASE_MAX_CONNS=16384 CBASE_LISTEN_BACKLOG=4096 ${SRVENV:-} \
      CBASE_LIMIT_CREATE_USER=1000000 CBASE_LIMIT_BUMP_USER=1000000 CBASE_LIMIT_CHAT_USER=1000000 CBASE_LIMIT_SIGNUP_ALL=1000000 CBASE_LIMIT_SIGNUP_IP=1000000 \
      $M/cbase_$b serve > $R/server_$l.log 2>&1"
  for i in $(seq 150); do grep -q "listening on" $R/server_$l.log 2>/dev/null && break; sleep 0.2; done
  p=$(pgrep -n -f "^$M/cbase_$b serve"); echo $p > $R/pid_$l; date +%s.%N > $R/t_start_$l
  echo "start $l build $b pid $p $(grep -c 'listening on' $R/server_$l.log) listening"
  bg cbsamp-$l "while kill -0 $p 2>/dev/null; do r=; h=; a=; while read -r k v _; do case \$k in VmRSS:) r=\$v;; VmHWM:) h=\$v;; RssAnon:) a=\$v;; esac; done < /proc/$p/status; echo \"\$EPOCHREALTIME \$r \$h \$a\" >> $R/rss_$l.log; sleep 0.2; done"
  [ -n "$cd_s" ] && bg cbcap-$l "sleep $cd_s; bash $M/srv.sh cap ${l}_mid > $R/capout_${l}_mid.txt 2>&1"
  true ;;
cap) cap $l $(pid_of ${l%_mid}) ;;
idle) cap $l $(pid_of ${l%_idle}) ;;
stop)
  p=$(pid_of $l); [ -n "$p" ] && cat /proc/$p/status > $R/status_${l}_end.txt 2>/dev/null
  grep -E "VmHWM" $R/status_${l}_end.txt
  kill $p 2>/dev/null; n=0; while kill -0 $p 2>/dev/null && [ $n -lt 50 ]; do sleep 0.2; n=$((n+1)); done; kill -9 $p 2>/dev/null
  rm -rf /var/lib/cbase_$l; true ;;
push) aws s3 cp --quiet --recursive $R s3://$B/res/srv/ --endpoint-url https://s3.dualstack.us-east-1.amazonaws.com; echo pushed ;;
esac
