#!/bin/bash
# build.sh [OUT_DIR] : build xsrv (the server, both backends), xload (the client) and xdp_prog.o into OUT_DIR (./bin).
# lwIP comes from lwip-2_2_1.tar.gz next to this script (fetch_lwip.sh downloads it), checked against its sha256.
# Needs gcc, clang (the BPF target), libbpf-devel and kernel-headers (AL2023: dnf install gcc clang libbpf-devel kernel-headers).
# XSRV_ONLY=1 / XLOAD_ONLY=1 build one side only (the load box needs no libbpf).
set -euo pipefail
D=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT=${1:-$D/bin}
mkdir -p "$OUT"
CC=${CC:-gcc}
if [ "${XSRV_ONLY:-0}" != 1 ]; then
  $CC -O2 -g -Wall -Wextra -o "$OUT/xload" "$D/load.c"
fi
[ "${XLOAD_ONLY:-0}" = 1 ] && exit 0
LW_TGZ=$D/lwip-2_2_1.tar.gz
echo "ce0b7461c0ad9602c376f0bf07c5eb7253b48c7bf66f011c6bf3e2a96731c539  $LW_TGZ" | sha256sum -c - >/dev/null
B=$(mktemp -d)
tar xzf "$LW_TGZ" -C "$B"
LW=$B/lwip-STABLE-2_2_1_RELEASE/src
INC="-I$D/port -I$LW/include"
objs=()
for f in "$LW"/core/{init,def,inet_chksum,ip,mem,memp,netif,pbuf,stats,sys,tcp,tcp_in,tcp_out,timeouts}.c \
         "$LW"/core/ipv6/{icmp6,ip6,ip6_addr,ip6_frag,nd6,mld6,inet6,ethip6}.c "$LW"/netif/ethernet.c; do
  o=$B/$(basename "$f" .c).o
  $CC -O2 -g -w $INC -c "$f" -o "$o"
  objs+=("$o")
done
$CC -O2 -g -Wall -Wextra -Wno-unused-parameter $INC -o "$OUT/xsrv" "$D/srv.c" "$D/be_xdp.c" "${objs[@]}" -lbpf
clang -O2 -g -target bpf -Wall -c "$D/xdp_prog.c" -o "$OUT/xdp_prog.o"
rm -rf "$B"
echo "built: $(ls "$OUT" | tr '\n' ' ')"
