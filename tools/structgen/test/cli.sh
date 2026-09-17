#!/usr/bin/env bash
# structgen must FAIL (non-zero, with a message) on every request it cannot honour.
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
make -s -C "$here" build/structgen
SG="$here/build/structgen"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
base=(--cwd "$here/test" --header kinds.h --root Kinds --build wasm= --ts "$tmp/out.ts")
fails=0
expect_fail() { # description pattern args...
    local what="$1" pat="$2"; shift 2
    local err; err="$("$SG" "$@" 2>&1 >/dev/null)"; local rc=$?
    if [ $rc -ne 0 ] && printf '%s' "$err" | grep -q "$pat"; then echo "ok   $what"
    else echo "FAIL $what (rc=$rc): $err"; fails=$((fails + 1)); fi
}
expect_fail "missing requested field"      "no field named nope"            "${base[@]}" --fields Kinds=tag,nope
expect_fail "--fields for an unreached record" "no such record"             "${base[@]}" --fields Nope=a
expect_fail "--const prefix matching nothing" "matches no enum constant"    "${base[@]}" --const NOPE_
expect_fail "non-integer #define under --const" "compile error"             --cwd "$here/test" --header kinds.h --root Kinds --build wasm= --ts "$tmp/o.ts" --const KBAD_
expect_fail "unknown argument"             "unknown argument"               "${base[@]}" --frobnicate 1
expect_fail "flag without a value"         "needs a value"                  "${base[@]}" --root
expect_fail "two --build in one run"       "one --build per run"            "${base[@]}" --build other=
expect_fail "root that is not a record"    "not a struct or union"          --cwd "$here/test" --header kinds.h --root KEnum --build wasm= --ts "$tmp/o.ts"
expect_fail "header that does not compile" "compile error"                  --cwd "$here/test" --header missing.h --root Kinds --build wasm= --ts "$tmp/o.ts"
expect_fail "colliding generated names"     "emitted twice"                  --cwd "$here/test" --header collide.h --root Collide --build wasm= --ts "$tmp/o.ts"
"$SG" "${base[@]}" --print-hash > "$tmp/hash" && grep -qE '^0x[0-9a-f]{8}$' "$tmp/hash" && echo "ok   --print-hash prints the hash" || { echo "FAIL --print-hash"; fails=$((fails + 1)); }
[ $fails -eq 0 ] && echo "cli: all pass" || { echo "cli: $fails failed"; exit 1; }
