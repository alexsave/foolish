#!/bin/bash
# local_veth_test.sh : functional test of both backends over a veth pair, inside a privileged Linux container
# (docker run --privileged -v $PWD:/src:ro amazonlinux:2023, with gcc clang libbpf-devel kernel-headers iproute ethtool).
# veth XDP is driver mode on a software device and AF_XDP is copy mode: this proves correctness, not performance.
set -u
OUT=${1:-/tmp/xt}
rm -rf "$OUT" && mkdir -p "$OUT" && cd "$OUT"
cp -r /src/. src/ && src/build.sh "$OUT/bin" || exit 1
B=$OUT/bin
mount -t bpf bpf /sys/fs/bpf 2>/dev/null
mount -t tracefs tracefs /sys/kernel/tracing 2>/dev/null
ip netns del cli 2>/dev/null; ip link del veth0 2>/dev/null
ip netns add cli
ip link add veth0 type veth peer name veth1
ip link set veth1 netns cli
ip addr add fd00::1/64 dev veth0 nodad && ip link set veth0 up
ip netns exec cli ip addr add fd00::2/64 dev veth1 nodad
ip netns exec cli ip link set veth1 up; ip netns exec cli ip link set lo up
ethtool -K veth0 tx off >/dev/null; ip netns exec cli ethtool -K veth1 tx off >/dev/null
ping -6 -c1 -W2 fd00::2 >/dev/null || { echo "veth ping failed"; exit 1; }
cl() { ip netns exec cli "$B/xload" --host fd00::1 "$@" --out res.txt; echo "client rc=$?"; }
srv() { "$B/xsrv" "$@" --if veth0 --port 7000 --ctl 7100 > "srv_$1_$$.log" 2>&1 & P=$!; sleep 1; cat "srv_$1_$$.log"; }
stop() { kill $P; wait $P; tail -c 400 srv_*_$$.log | grep -o 'moves=[0-9]* frames=[0-9]*\|rexmit=[0-9]*\|xdp_drop=[0-9]*\|xdp_redirect=[0-9]*' | tr '\n' ' '; echo; rm -f srv_*_$$.log; }

echo "== kernel backend"
srv kern --spin 5 --tp-sys 1
cl --mode smoke --k 2 --size 300 --conns 16 --moves 5000 --label kern_smoke
cl --mode sat --k 8 --size 1200 --conns 64 --warm 1 --dur 2 --label kern_sat8
stop
echo "== attach"
"$B/xsrv" attach --if veth0 --mode drv --obj "$B/xdp_prog.o" || "$B/xsrv" attach --if veth0 --mode skb --obj "$B/xdp_prog.o" || exit 1
echo "== kernel backend with the program attached (passing)"
srv kern --spin 5
cl --mode smoke --k 2 --size 300 --conns 16 --moves 3000 --label kernx_smoke
stop
MAC=$(ip netns exec cli cat /sys/class/net/veth1/address)
echo "== xdp backend"
srv xdp --ip6 fd00::1 --gw-mac "$MAC" --spin 5 --tp-sys 1
cl --mode smoke --k 2 --size 300 --conns 16 --moves 10000 --label xdp_smoke
cl --mode smoke --k 8 --size 1200 --conns 64 --moves 5000 --label xdp_smoke8
cl --mode hum --k 2 --size 300 --conns 40 --warm 1 --dur 4 --label xdp_hum
cl --mode sat --k 8 --size 1200 --conns 64 --warm 1 --dur 2 --label xdp_sat8
stop
echo "== xdp backend, poll as the TX kick, learnt MAC"
srv xdp --ip6 fd00::1 --spin 5 --kick-poll 1
cl --mode smoke --k 2 --size 300 --conns 16 --moves 5000 --label xdpP_smoke
stop
echo "== xdp backend, 1% drops each way"
srv xdp --ip6 fd00::1 --gw-mac "$MAC" --spin 0 --drop-rx 10 --drop-tx 10
cl --mode smoke --k 2 --size 300 --conns 16 --moves 10000 --label xdp_drop
stop
"$B/xsrv" detach --if veth0
grep '^RUN' res.txt | sed 's/ warm=.*moves_sent/ moves_sent/' | cut -c1-330
