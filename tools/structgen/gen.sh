#!/usr/bin/env bash
# Regenerate every structgen output with the libclang tool, one run per build.
# Layout flags come from c/Makefile itself, so a -D cap change there is picked up.
#
#   sdk/ts/gen/             the modules production TS imports: accessors, and
#                           layout_hash.<build>.ts, the LAYOUT_HASH each wasm
#                           module is checked against (its own module, so the
#                           browser can check it without importing a reader of
#                           the unmasked Game - e2e/security_client_boundary)
#   tools/structgen/gen/    the generator's own genericity fixtures
#
#   gen.sh           write both
#   gen.sh --check   regenerate into a temp dir and fail if either is stale (the
#                    freshness gate, in the style of scripts/check_wasm_freshness.sh)
#
# build/verify.wasm is a BUILD OUTPUT, so it lives with the generator's own
# binary and is not committed. It is a wasm32 link of test/verify.c whose bytes
# are the toolchain's, so a macOS homebrew clang and CI's clang-22 on Linux
# write two different modules from the same source. It USED to sit in gen/ and
# be committed, with `diff -x verify.wasm` excusing it from the freshness check
# for exactly that reason - and the exclusion is what let it rot: it was last
# written at 05192715 and still held the layouts anim_plan.h had before 546565fe
# grew AnimPlan, while `gen.sh --check` reported everything fresh. It is built
# on demand now (test/verify.test.ts reads it, CI runs this script before the
# test), and two checks keep that true: --check refuses a tracked build output
# under gen/, and the link is run twice and compared, so a source that stops
# building reproducibly (a __DATE__, a path, an uninitialised pad) fails here
# rather than turning up as a mystery diff.
#
# game_layout.<build>.ts and layout_hash.<build>.ts are ALSO written by the wasm make targets (c/Makefile,
# "Layout hash"), from the same specs/game_layout.args and the same flags, so
# `make -C c wasm-bots` after a header edit leaves the module and the wasm in
# agreement. This script is the full regeneration and the CI check.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
CLANG="${WASM_CC:-/opt/homebrew/opt/llvm/bin/clang}"
make -s -C "$here" build/structgen
SG="$here/build/structgen"
flags() { make -s -C "$root/c" -f Makefile -f "$here/print.mk" "sg-print-$1"; }
spec() { grep -v '^[[:space:]]*#' "$here/specs/$1.args"; }
BOTS="$(flags WASM_BOT_CFLAGS)"
prod="$root/sdk/ts/gen"
fixtures="$here/gen"
check=0
if [ "${1:-}" = "--check" ]; then
  check=1
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
  prod="$tmp/prod"; fixtures="$tmp/fixtures"
fi
mkdir -p "$prod" "$fixtures"

# The resident Game prefix the TS marshal reads and writes, per wasm build.
set -f   # the spec is split on whitespace, never globbed
# shellcheck disable=SC2207
GAME=(--cwd "$root/c" $(spec game_layout))
set +f
"$SG" "${GAME[@]}" --build "bots=$BOTS" --ts "$prod/game_layout.bots.ts" --hash-ts "$prod/layout_hash.bots.ts"
"$SG" --cwd "$root/c" --header anim_plan.h --header legal.h --build "bots=$BOTS" \
  --root AnimPlan --root AnimFrame --root AnimBeats --root AnimEvent --root LegalMoves \
  --const ANIM_TIME_MS --const ANIM_GAP_MS --const ANIM_STEP_NONE --const ANIM_NEVER \
  --const ANIM_EVT_ --const ANIM_LOC_ --const ANIM_CONFLICT_ --ts "$prod/anim.bots.ts"

# The web client's reader of its slot (c/src/client_table.h): snapshot readers
# only, no accessor over the struct (specs/view_layout.args).
set -f
# shellcheck disable=SC2207
VIEW=(--cwd "$root/c" $(spec view_layout))
set +f
"$SG" "${VIEW[@]}" --build "bots=$BOTS" --ts "$prod/view_layout.bots.ts"

# The FMSG bridge's header (c/src/msg_wire.h MsgHeader): a snapshot reader, a
# writer and the codec's constants (specs/msg_layout.args).
set -f
# shellcheck disable=SC2207
MSG=(--cwd "$root/c" $(spec msg_layout))
set +f
"$SG" "${MSG[@]}" --build "bots=$BOTS" --ts "$prod/msg_layout.bots.ts"

# The Oracle's Mode B candidate table (c/src/oracle_mt.h), read back from
# oracle-mt.wasm (specs/oracle_layout.args).
set -f
# shellcheck disable=SC2207
ORACLE=(--cwd "$root/c" $(spec oracle_layout))
set +f
"$SG" "${ORACLE[@]}" --build "oracle_mt=$(flags WASM_ORACLE_MT_CFLAGS)" --ts "$prod/oracle_layout.oracle_mt.ts"

# Genericity fixtures (test/verify.test.ts).
"$SG" --cwd "$here/test" --header kinds.h --root Kinds --build wasm= --const K_ --const KFLAG_ --ts "$fixtures/kinds.ts"
"$SG" --cwd "$here/test" --header snap.h --root Snap --root SPtr --build wasm= --snapshot Snap --snapshot SPtr --snapshot-only \
  --count Snap.pairs=n_pairs --count Snap.items=n_items --count Snap.text=n_text --count SItem.text=len --writer Snap \
  --count SPtr.vals=n_vals --count SPtr.items=n_items --count SPtr.name=name_len --count SPtr.none=n_none --ts "$fixtures/snap.ts"

if [ "$check" = 1 ]; then
  stale=0
  diff -r "$root/sdk/ts/gen" "$prod" || stale=1
  # No exclusions: a generated file the diff does not look at is a generated
  # file nothing keeps fresh. gen/ holds generated MODULES only; the wasm the
  # verify test reads is a build output and lives in build/.
  diff -r "$here/gen" "$fixtures" || stale=1
  # A build output tracked in the repo goes stale the moment its source changes
  # and nobody reruns the build. Everything under gen/ the repo knows about must
  # be a generated MODULE the diff above compares.
  tracked_junk="$(GIT_OPTIONAL_LOCKS=0 git -C "$root" ls-files "tools/structgen/gen" | grep -v '\.ts$' || true)"
  if [ -n "$tracked_junk" ]; then
    echo "gen: a build output is committed under tools/structgen/gen - remove it from the repo:"
    echo "$tracked_junk"
    stale=1
  fi
  if [ "$stale" = 0 ]; then echo "gen: fresh"; else echo "gen: STALE - run tools/structgen/gen.sh"; exit 1; fi
  exit 0
fi
verify_link() {
  "$CLANG" --target=wasm32 -nostdlib -ffreestanding -O1 -I"$here/test" -I"$root/c/src" -isystem "$root/c/wasm/include" \
    -D_Thread_local= -DMAX_LOG_PAIRS=64 -DMAX_LEGAL_MOVES=4096 -DMAX_MOVE_CARDS=28 -DMAX_BATTLES=64 \
    -Wl,--no-entry -Wl,--export-all "$here/test/verify.c" -o "$1"
}
mkdir -p "$here/build"
verify_link "$here/build/verify.wasm"
# The same source, linked again: the module the test reads has to be a function
# of verify.c and the headers it includes, and of nothing else.
twin="$(mktemp -t verify.XXXXXX).wasm"
verify_link "$twin"
if ! cmp -s "$here/build/verify.wasm" "$twin"; then
  echo "gen: verify.wasm does not build reproducibly - two links of the same source differ"
  rm -f "$twin"
  exit 1
fi
rm -f "$twin"
