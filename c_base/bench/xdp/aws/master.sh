#!/bin/bash
# master.sh SRV_IP6 BUCKET : the whole experiment, unattended, on the load box. Every run's output is uploaded to
# s3://BUCKET/xdp/ as soon as it exists; at the end both boxes power off (and terminate: shutdown behaviour).
#   phase 0  facts and coexistence check with the production NIC config
#   phase 1  the XDP attach ladder (srvctl /nic?cfg=xdp)
#   phase 2  correctness: 10k-move smokes through lwIP, drop tests, the syscall-count cross-check, coexistence check
#            while the AF_XDP sockets are live
#   phase 3  the matrix, 3 repetitions; inside each: hum then sat; inside each: the production NIC (kern), then the
#            XDP NIC config (kernx = kernel TCP with the program attached and passing, xdp, xdpP = xdp with the poll as
#            the TX kick) alternating per combination
S=$1; B=$2; D=/var/tmp/xs
export AWS_USE_DUALSTACK_ENDPOINT=true AWS_DEFAULT_REGION=us-east-1
ulimit -n 65536
cd $D
mkdir -p srv
log() { echo "$(date -u +%FT%T) $*" >> master.log; }
up() {
  aws s3 cp --only-show-errors master.log s3://$B/xdp/master.log
  [ -f results.txt ] && aws s3 cp --only-show-errors results.txt s3://$B/xdp/results.txt
  aws s3 sync --only-show-errors srv/ s3://$B/xdp/srv/
}
ctl() { curl -s -m ${2:-200} "http://[$S]:9000/$1"; }
XLOAD=$D/bin/xload

log "master start srv=$S bucket=$B"
for i in $(seq 1 240); do
  p=$(ctl ping 5)
  case "$p" in *BUILD_OK*|*BUILD_FAILED*) break;; esac
  sleep 15
done
log "srvctl: $p"; up
case "$p" in *BUILD_OK*) ;; *) log "server build not ok, stopping"; up; shutdown -h now; exit 1;; esac

# ---- phase 0
ctl facts > srv/facts.txt
ctl check > srv/check_prod.txt
{ uname -a; ethtool -i ens5 2>/dev/null; nproc; } > srv/load_facts.txt 2>&1
up

run() { # label backend mode k size spin conns warm dur [server-extra] [client-extra]
  local label=$1 be=$2 mode=$3 k=$4 size=$5 spin=$6 conns=$7 warm=$8 dur=$9 sx=${10:-} cx=${11:-}
  ctl "start?backend=$be&spin=$spin&label=$label&extra=$(printf %s "$sx" | sed 's/ /%20/g')" > srv/$label.start
  if ! grep -q READY srv/$label.start; then
    log "START FAILED $label"; ctl "stop?label=$label" > srv/$label.stop; up; return 1
  fi
  timeout 600 $XLOAD --host $S --mode $mode --k $k --size $size --conns $conns --warm $warm --dur $dur --label $label --out results.txt $cx > srv/$label.client 2>&1
  local rc=$?
  ctl "stop?label=$label" > srv/$label.stop
  log "done $label rc=$rc $(grep -o 'p50_us=[0-9.]* p95_us=[0-9.]* p99_us=[0-9.]*' srv/$label.client) $(grep -o 'lost=[0-9]* dups=[0-9]* gaps=[0-9]* badbytes=[0-9]*' srv/$label.client)"
  up
}

# ---- phase 1
ctl "nic?cfg=xdp" 900 > srv/nic_xdp_ladder.txt
XDP_OK=0; grep -q XDP_OK srv/nic_xdp_ladder.txt && XDP_OK=1
log "xdp ladder: $(grep -E 'XDP_OK|XDP_FAILED' srv/nic_xdp_ladder.txt)"; up

# ---- phase 2 (in the XDP config)
if [ $XDP_OK = 1 ]; then
  # coexistence while the sockets are bound and the redirect is on
  ctl "start?backend=xdp&spin=0&label=coexist_xdp" > srv/coexist_xdp.start
  ctl check > srv/check_xdp_live.txt
  python3 -c "import socket,sys; s=socket.create_connection(('$S',22),3); print('ssh banner:', s.recv(64))" > srv/ssh_xdp_live.txt 2>&1
  timeout 60 $XLOAD --host $S --mode smoke --k 2 --size 300 --conns 16 --moves 2000 --label coexist_xdp --out results.txt > srv/coexist_xdp.client 2>&1
  ctl check > srv/check_xdp_live2.txt
  ctl "stop?label=coexist_xdp" > srv/coexist_xdp.stop
  up
  run smoke_xdp_k2_300 xdp smoke 2 300 0 16 0 0 "--tp-sys 1" "--moves 10000"
  run smoke_xdp_k8_1200 xdp smoke 8 1200 0 64 0 0 "--tp-sys 1" "--moves 10000"
  run smoke_xdpP_k2_300 xdp smoke 2 300 0 16 0 0 "--tp-sys 1 --kick-poll 1" "--moves 10000"
  run drop_rx_xdp_k2_300 xdp smoke 2 300 0 16 0 0 "--drop-rx 10" "--moves 10000"
  run drop_tx_xdp_k2_300 xdp smoke 2 300 0 16 0 0 "--drop-tx 10" "--moves 10000"
  run drop_both_xdp_k8_1200 xdp smoke 8 1200 0 64 0 0 "--drop-rx 10 --drop-tx 10" "--moves 10000"
  run smoke_kernx_k2_300 kern smoke 2 300 0 16 0 0 "--tp-sys 1" "--moves 10000"
fi
ctl "nic?cfg=prod" 600 > srv/nic_prod_0.txt
run smoke_kern_k2_300 kern smoke 2 300 0 16 0 0 "--tp-sys 1" "--moves 10000"
run smoke_kern_k8_1200 kern smoke 8 1200 0 64 0 0 "--tp-sys 1" "--moves 10000"

# ---- phase 3
COMBOS="2:300:20 8:300:20 2:1200:20 8:1200:20 2:300:0 8:300:0"
conns_for() { [ $1 = 8 ] && echo 304 || echo 300; }
for r in 1 2 3; do
  for sc in hum sat; do
    if [ $sc = hum ]; then W=10; T=45; else W=5; T=20; fi
    ctl "nic?cfg=prod" 600 > srv/nic_prod_r${r}_$sc.txt
    run idle_kern_r${r}_$sc kern idle 2 300 20 300 5 30
    for c in $COMBOS; do
      IFS=: read k size spin <<< "$c"
      run ${sc}_k${k}_s${size}_w${spin}_kern_r$r kern $sc $k $size $spin $(conns_for $k) $W $T
    done
    [ $XDP_OK = 1 ] || continue
    ctl "nic?cfg=xdp" 600 > srv/nic_xdp_r${r}_$sc.txt
    run idle_kernx_r${r}_$sc kern idle 2 300 20 300 5 30
    run idle_xdp_r${r}_$sc xdp idle 2 300 20 300 5 30
    for c in $COMBOS; do
      IFS=: read k size spin <<< "$c"
      run ${sc}_k${k}_s${size}_w${spin}_kernx_r$r kern $sc $k $size $spin $(conns_for $k) $W $T
      run ${sc}_k${k}_s${size}_w${spin}_xdp_r$r xdp $sc $k $size $spin $(conns_for $k) $W $T
      if [ $size = 300 ] && [ $spin = 20 ]; then
        run ${sc}_k${k}_s${size}_w${spin}_xdpP_r$r xdp $sc $k $size $spin $(conns_for $k) $W $T "--kick-poll 1"
      fi
    done
  done
  log "repetition $r finished"
done

ctl "nic?cfg=prod" 600 > srv/nic_prod_end.txt
ctl check > srv/check_end.txt
log "ALL FINISHED"; up
ctl poweroff 10
sleep 5
up
shutdown -h now
