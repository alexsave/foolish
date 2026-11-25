#!/bin/bash
# build_twin.sh: a copy of c_base/providers/aws/ops/build_bootstrap.sh's `serve` recipe (foolish 9c5057b64) that also copies
# the UNSTRIPPED build/cbase out of the container, for symbolization of perf samples of the stripped cbase_D.
# Source tree: the detached worktree /Users/alex/Dev/foolish-wt-trip1 (9c5057b64). Output: ../pkg/cbase_D.stripped, ../pkg/cbase_D.unstripped,
# ../pkg/apk_versions.txt (package versions in the container, so a non-reproducing sha can be explained).
set -euo pipefail
IMAGE="alpine:3.24@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6"
root=/Users/alex/Dev/foolish-wt-trip1
out=/Users/alex/Dev/foolish-bench-results/campaign/trip1/pkg
mkdir -p "$out"
cpus="${BUILD_CPUS:-2}"
docker run --rm --platform linux/arm64 --cpus="$cpus" -e JOBS="$cpus" -e TARGET=build/cbase \
  -v "$root":/repo:ro -v "$out":/out \
  "$IMAGE" sh -euc '
    apk upgrade --no-cache >/dev/null
    pending="$(apk version -l "<" | grep -v "^Installed:" || true)"
    if [ -n "$pending" ]; then echo "packages with pending upgrades after apk upgrade:" >&2; echo "$pending" >&2; exit 1; fi
    apk add --no-cache build-base openssl-dev openssl-libs-static sqlite-dev sqlite-static linux-headers zip >/dev/null
    apk info -v 2>/dev/null | sort > /out/apk_versions.txt
    mkdir -p /w
    cd /repo
    tar cf - --exclude="./c_base/build" --exclude="./c_base/dist" --exclude="./foolish/c/build" \
      ./c_base ./foolish/c ./foolish/server ./shared | tar xf - -C /w
    cd /w/c_base
    make -s -j"$JOBS" STATIC=1 "$TARGET"
    strip -o /out/cbase_D.stripped "$TARGET"
    cp "$TARGET" /out/cbase_D.unstripped
  '
shasum -a 256 "$out/cbase_D.stripped" "$out/cbase_D.unstripped"
ls -l "$out/cbase_D.stripped" "$out/cbase_D.unstripped"
