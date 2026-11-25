#!/bin/bash
# run_one.sh LABEL CACHE_ARGS... : cachegrind the dynamic cbase serve under sat2 load (2 pairs, think 0-1 ms, 25 s).
# Server: container cbsrv (2 CPUs, 412 MiB, like the nano). Load: container cbload on the same network (p3load via load.sh).
# The server is stopped with SIGTERM to valgrind, which forwards it to cbase (it drains) and then writes the cachegrind file.
set -uo pipefail
L=$1; shift; CACHE_ARGS="$*"; MODE=${MODE:-play}; TOOL=${TOOL:-cachegrind}
H=/Users/alex/Dev/foolish-bench-results/cachesim; IMG=cachesim-al2023:latest
docker rm -f cbsrv >/dev/null 2>&1
if [ "$TOOL" = callgrind ]; then TOOLARGS="--tool=callgrind --callgrind-out-file=/out/out/cg_$L.out"; else TOOLARGS="--tool=cachegrind --cachegrind-out-file=/out/out/cg_$L.out"; fi
docker run -d --name cbsrv --platform linux/arm64 --cpus=2 --memory=412m --network cachesim \
  -v $H:/out -v $H/scripts:/scripts:ro $IMG bash -c "
  S=\$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n'); A=\$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')
  mkdir -p /var/lib/cbase_$L && cd /var/lib/cbase_$L && ulimit -n 65536
  export CBASE_JWT_SECRET=\$S CBASE_ADMIN_TOKEN=\$A CBASE_PUBLIC_URL=http://nano.example CBASE_STORE=sqlite \
    CBASE_DB=/var/lib/cbase_$L/cbase.sqlite CBASE_LISTEN=0.0.0.0:8080 SSL_CERT_FILE=/etc/pki/tls/certs/ca-bundle.crt \
    CBASE_RATED_MIN_AGE=0 CBASE_MAX_CONNS=16384 CBASE_LISTEN_BACKLOG=4096 \
    CBASE_LIMIT_CREATE_USER=1000000 CBASE_LIMIT_BUMP_USER=1000000 CBASE_LIMIT_CHAT_USER=1000000 CBASE_LIMIT_SIGNUP_ALL=1000000 CBASE_LIMIT_SIGNUP_IP=1000000
  exec valgrind $TOOLARGS --cache-sim=yes --branch-sim=yes $CACHE_ARGS --trace-children=no \
    --log-file=/out/out/vg_$L.log /out/bin/cbase_dyn serve > /out/out/server_$L.log 2>&1" >/dev/null
for i in $(seq 300); do grep -q "listening on" $H/out/server_$L.log 2>/dev/null && break; sleep 1; done
grep -c "listening on" $H/out/server_$L.log
IP=$(docker inspect -f '{{with index .NetworkSettings.Networks "cachesim"}}{{.IPAddress}}{{end}}' cbsrv); echo "server ip $IP"
# MODE=play: sat2 25 s. MODE=setup: sign-ups and a 1 s play window (the baseline for the play). MODE=idle: no client, 25 s.
if [ "$MODE" = idle ]; then sleep 25
else
DUR=25000; [ "$MODE" = setup ] && DUR=1000
docker run --rm --platform linux/arm64 --network cachesim --memory=2g -v $H:/out -v $H/scripts:/scripts:ro $IMG bash -c "
  dnf install -y -q python3 > /dev/null 2>&1
  mkdir -p /opt/mt /var/tmp/mt && cp /out/bin/p3load /opt/mt/ && cp /scripts/load.sh /opt/mt/ && echo bucket > /opt/mt/bucket
  bash /opt/mt/load.sh run $L $IP 2 0 1 100000 $DUR > /out/out/load_$L.txt 2>&1
  cp -r /var/tmp/mt/$L /out/out/ 2>/dev/null; cat /out/out/load_$L.txt | tail -4"
fi
echo "load done: $(date -u +%T)"
docker kill -s TERM cbsrv >/dev/null 2>&1; timeout_s=0; until [ "$(docker inspect -f '{{.State.Running}}' cbsrv 2>/dev/null)" != true ] || [ $timeout_s -gt 600 ]; do sleep 2; timeout_s=$((timeout_s+2)); done
echo "server exit: $(docker inspect -f '{{.State.ExitCode}}' cbsrv) after ${timeout_s}s"
ls -l $H/out/cg_$L.out 2>&1
