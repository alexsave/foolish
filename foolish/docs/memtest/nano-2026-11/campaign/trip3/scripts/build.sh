#!/bin/bash
# build.sh ROOT NAME [MAKEVAR=VALUE...]: trip 3 build (trip 2 copy, outputs under campaign/trip3, docker under nice -n 10), itself trip 2's build, a copy of trip1/scripts/build_twin.sh (itself a copy of the `serve`
# recipe of c_base/providers/aws/ops/build_bootstrap.sh at foolish 9c5057b64) that takes the source tree, the output name
# and extra make variables (CPU=neoverse-n1 for cbase_N1), passed on the `make ... STATIC=1 build/cbase` line.
# Output: ../bin/cbase_NAME (stripped, the shipped form), ../bin/unstripped/cbase_NAME (for checking the flags),
# ../out/builds/apk_NAME.txt. Docker capped at BUILD_CPUS (default 2).
set -euo pipefail
IMAGE="alpine:3.24@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6"
root=$1; name=$2; shift 2
extra="$*"
T=/Users/alex/Dev/foolish-bench-results/campaign/trip3
out=$T/bin; mkdir -p "$out/unstripped" "$T/out/builds"
cpus="${BUILD_CPUS:-2}"
echo "build $name from $root at $(git -C "$root" rev-parse HEAD) status: [$(git -C "$root" status --short | tr '\n' ' ')] extra make vars: [$extra] start $(date -u +%FT%TZ)"
nice -n 10 docker run --rm --platform linux/arm64 --cpus="$cpus" -e JOBS="$cpus" -e TARGET=build/cbase -e NAME="$name" -e EXTRA="$extra" \
  -v "$root":/repo:ro -v "$out":/out -v "$T/out/builds":/blog \
  "$IMAGE" sh -euc '
    apk upgrade --no-cache >/dev/null
    pending="$(apk version -l "<" | grep -v "^Installed:" || true)"
    if [ -n "$pending" ]; then echo "packages with pending upgrades after apk upgrade:" >&2; echo "$pending" >&2; exit 1; fi
    apk add --no-cache build-base openssl-dev openssl-libs-static sqlite-dev sqlite-static linux-headers zip >/dev/null
    apk info -v 2>/dev/null | sort > /blog/apk_$NAME.txt
    gcc --version | head -1
    mkdir -p /w
    cd /repo
    tar cf - --exclude="./c_base/build" --exclude="./c_base/dist" --exclude="./foolish/c/build" \
      ./c_base ./foolish/c ./foolish/server ./shared | tar xf - -C /w
    cd /w/c_base
    test ! -e build || { echo "build dir present in the copy" >&2; exit 1; }
    make -j"$JOBS" STATIC=1 $EXTRA "$TARGET" > /blog/make_$NAME.log 2>&1 || { tail -40 /blog/make_$NAME.log; exit 1; }
    echo "make lines with -mcpu: $(grep -c -- -mcpu= /blog/make_$NAME.log)"
    echo "make compile lines (cc -c): $(grep -c -- " -c " /blog/make_$NAME.log)"
    strip -o /out/cbase_$NAME "$TARGET"
    cp "$TARGET" /out/unstripped/cbase_$NAME
  '
shasum -a 256 "$out/cbase_$name" "$out/unstripped/cbase_$name"
ls -l "$out/cbase_$name"
echo "done $name $(date -u +%FT%TZ)"
