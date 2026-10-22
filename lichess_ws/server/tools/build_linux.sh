#!/bin/bash
# build_linux.sh - dist/lws-linux-arm64: the gateway as a statically linked Linux arm64 executable,
# built the way c_base builds its own (c_base/providers/aws/ops/build_bootstrap.sh: an arm64 Alpine
# container, musl, static OpenSSL and SQLite), for the lichess mono container (the drop-in test in
# ../DESIGN.md) and for any Linux arm64 host. Run from lichess_ws/server.
set -euo pipefail
cd "$(dirname "$0")/.."
root="$(cd ../.. && pwd)"
mkdir -p dist
IMAGE="alpine:3.24@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6"
cpus="${BUILD_CPUS:-2}"
docker run --rm --platform linux/arm64 --cpus="$cpus" -e JOBS="$cpus" \
  -v "$root":/repo:ro -v "$PWD/dist":/out \
  "$IMAGE" sh -euc '
    apk add --no-cache build-base openssl-dev openssl-libs-static sqlite-dev sqlite-static zlib-static linux-headers >/dev/null
    mkdir -p /w
    cd /repo
    tar cf - --exclude="./c_base/build" --exclude="./c_base/dist" --exclude="./lichess_ws/server/build" --exclude="./lichess_ws/server/dist" \
      ./c_base ./lichess_ws/server ./shared | tar xf - -C /w
    cd /w/lichess_ws/server
    make -s -j"$JOBS" STATIC=1 build/lws build/wsbench
    strip -o /out/lws-linux-arm64 build/lws
    strip -o /out/wsbench-linux-arm64 build/wsbench
  '
ls -l dist/lws-linux-arm64 dist/wsbench-linux-arm64
file dist/lws-linux-arm64 2>/dev/null || true
