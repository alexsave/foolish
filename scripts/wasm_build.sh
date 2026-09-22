#!/usr/bin/env bash
# Build the shipped wasm modules. THE ONE ENTRY POINT, and the one list of what
# "the shipped modules" are.
#
#   bots    sdk/ts/wasm/bots.wasm.gz   the kernel every host loads - the browser
#                                      by fetching it, the edge functions as a
#                                      static asset, Node's suites off disk
#   oracle  public/oracle.wasm.gz      the client-side replay analyser (Mode A)
#           public/oracle-mt.wasm.gz   the same analyser's shared-memory Mode B
#
# NONE OF IT IS COMMITTED, and that is the point of this script. The argument is
# one this repo has already made and already paid for twice.
#
# What it used to be: three .gz files in git, rebuilt BY HAND, on a Mac, by
# whoever remembered. That arrangement needed two gates to hold it up -
# scripts/check_wasm_freshness.sh, which compared COMMIT ORDER because it could
# not compare bytes, and sdk/ts/wasm/WASM_STAMP, a source hash committed beside
# the artifacts to prove a human had run make. Both are gone with this script,
# because both were treating one cause: an artifact nothing can regenerate is an
# artifact nothing can check. Neither ever stopped the drift it was written for -
# public/oracle.wasm.gz sat behind the kernel for three weeks and served one seat
# the opposite endgame verdict, and it was BEHIND again, by exactly one raw byte,
# on the day this script replaced them. One byte is not something review finds.
#
# THE MEASUREMENT THAT MADE THIS SAFE, and it is the one the old gates assumed
# was impossible. check_wasm_freshness.sh's header said "the toolchain that
# produced the committed bytes is not the toolchain any given machine has, so a
# byte gate would be red forever". That is true of the toolchain VERSION and
# false of the MACHINE, and nobody had separated the two. From one tree,
# c/build/bots.wasm is BYTE-IDENTICAL on all three of
#
#   macOS arm64, Homebrew clang 22.1.8 + binaryen 130
#   Linux arm64, apt.llvm.org clang 22.1.8 + binaryen 130
#   Linux x86_64, apt.llvm.org clang 22.1.8 + binaryen 130   <- ubuntu-latest
#
# all md5 ac53b4dc5484aabad381d2d7bc451088, 191,485 B, and the same holds for
# oracle.wasm (177,835 B) and oracle-mt.wasm (1,091,163 B). A PINNED toolchain
# is reproducible across operating systems AND across host architectures, so the
# modules can be built by the lane that ships them and there is nothing left to
# keep fresh.
#
# WHICH IS WHY THE TOOLCHAIN IS PINNED rather than inherited - scripts/ci_wasm.sh
# installs it, and the version really does decide the module:
#
#   clang 22.1.8 + binaryen 130   191,485 B raw   the pin
#   clang 22.1.8 + binaryen 108   191,729 B raw   +244 B
#   clang 18.1.3 + binaryen 108   194,997 B raw   +3,512 B
#
# clang 18 is what scripts/ci_llvm.sh installs for libclang, and on the shipped
# module it costs 3.5 KB of kernel and ~1.7 KB of download. A lane that builds a
# module someone downloads uses the pin; a lane that only needs libclang does
# not care, and that script is left exactly as it was.
#
# WHY A CALLER MAY ASK FOR ONE GROUP. `wasm-bots` has real per-object make rules
# and is ~0s when nothing changed; `wasm-oracle` and `wasm-oracle-mt` are phony
# targets whose recipes loop over all 34 sources in the shell, so they recompile
# everything every run - measured on this Mac, warm: bots ~0s, oracle 6.3s,
# oracle-mt ~7s. A lane that cannot load an Oracle module should not pay 13s for
# one, because a 13s tax on `npm run typecheck` is how a build step gets deleted.
# Giving those two recipes object rules would make the whole thing ~0s warm and
# is worth doing; it is a separate change, and this comment is the measurement it
# should start from.
#
# Usage:
#   scripts/wasm_build.sh                 build every group
#   scripts/wasm_build.sh bots            just the kernel every host loads
#   scripts/wasm_build.sh oracle          just the two replay-analyser modules
#   scripts/wasm_build.sh --print-paths [group]
#                                         the paths it writes, repo-relative.
#                                         Needs NO toolchain: it is how the gate
#                                         that refuses a tracked artifact
#                                         (e2e/validation/wasm_outputs_validation.test.ts)
#                                         asks what is generated instead of
#                                         keeping a second list.
#   scripts/wasm_build.sh --print-groups   the group names
#   scripts/wasm_build.sh --check         build TWICE, as two separate
#                                         processes, and refuse a difference.
#                                         This is what replaced the freshness
#                                         gate, and it is strictly stronger: the
#                                         old check could only ask "was a
#                                         committed copy touched after the C
#                                         was", which any edit to the .gz
#                                         satisfied; this asks "is the module a
#                                         function of this tree and nothing
#                                         else", which is the property that
#                                         makes building it in CI safe at all.
#                                         Same check gen.sh --check makes of the
#                                         generated modules, for the same reason:
#                                         a build that wrote a timestamp, an
#                                         address or an uninitialised pad would
#                                         hand two lanes of one commit two
#                                         different kernels.
set -euo pipefail
cd "$(dirname "$0")/.."

# THE ONE LIST: group, path, and the make target that writes that path. Kept
# here rather than derived from c/Makefile because a .gz is not named by any
# variable the Makefile exports - it appears only inside a recipe's gzip line.
# This list moved here from scripts/check_wasm_freshness.sh, which is deleted:
# the same list, now owned by the thing that builds them instead of the thing
# that audited them.
ARTIFACTS=(
  "bots|sdk/ts/wasm/bots.wasm.gz|wasm-bots"
  "oracle|public/oracle.wasm.gz|wasm-oracle"
  "oracle|public/oracle-mt.wasm.gz|wasm-oracle-mt"
)
# NOT `GROUPS`: that is a bash special variable holding the caller's unix
# group ids, and assigning to it is silently IGNORED - every lookup below then
# compared a group name against a list of gids and refused everything.
WASM_GROUPS=(bots oracle)

field() { printf '%s\n' "${ARTIFACTS[@]}" | cut -d'|' -f"$1"; }
# rows of one group, or every row when the group is empty
rows() {
  if [ -z "${1:-}" ]; then printf '%s\n' "${ARTIFACTS[@]}"
  else printf '%s\n' "${ARTIFACTS[@]}" | grep "^$1|"; fi
}

want=""
check=0
case "${1:-}" in
  --print-groups) printf '%s\n' "${WASM_GROUPS[@]}"; exit 0 ;;
  --check) check=1 ;;
  --print-paths)
    if [ -n "${2:-}" ]; then
      printf '%s\n' "${WASM_GROUPS[@]}" | grep -qx "$2" || { echo "wasm_build.sh: no such group '$2'" >&2; exit 2; }
      rows "$2" | cut -d'|' -f2
    else
      field 2
    fi
    exit 0 ;;
  "") ;;
  -*) echo "wasm_build.sh: unknown option '$1' (want nothing, a group, --print-paths or --print-groups)" >&2; exit 2 ;;
  *)
    printf '%s\n' "${WASM_GROUPS[@]}" | grep -qx "$1" \
      || { echo "wasm_build.sh: no such group '$1' (have: ${WASM_GROUPS[*]})" >&2; exit 2; }
    want="$1" ;;
esac

# Same default as tools/structgen/gen.sh, and for the same reason: WASM_CC
# defaults to plain `clang`, which on a Mac is APPLE clang and cannot target
# wasm32 at all ("No available targets are compatible with triple wasm32").
# Naming Homebrew's here is what stops that being a daily rediscovery. On Linux
# it is scripts/ci_wasm.sh that exports WASM_CC and this default is unused.
if [ -z "${WASM_CC:-}" ] && [ "$(uname -s)" = "Darwin" ]; then
  WASM_CC=/opt/homebrew/opt/llvm/bin/clang
fi

TARGETS=()
while IFS= read -r t; do [ -n "$t" ] && TARGETS+=("$t"); done < <(rows "$want" | cut -d'|' -f3)
[ "${#TARGETS[@]}" -gt 0 ] || { echo "wasm_build.sh: nothing to build - is the ARTIFACTS list empty?" >&2; exit 1; }

# Passed as make VARIABLES rather than exported, so they beat c/Makefile's own
# defaults. Built as an array: an unquoted ${X:+...} would split a path with a
# space in it, and a Homebrew prefix on a renamed volume is exactly that path.
VARS=()
[ -n "${WASM_CC:-}" ]     && VARS+=("WASM_CC=$WASM_CC")
[ -n "${LLVM_PREFIX:-}" ] && VARS+=("LLVM_PREFIX=$LLVM_PREFIX")
# CC too: every wasm target runs tools/structgen first for the layout hash it
# compiles in, and structgen's Makefile builds the generator with $(CC). A
# container that has clang but no `cc` fails there rather than in the link - the
# same reason scripts/ci_llvm.sh names gcc explicitly.
[ -n "${CC:-}" ] && VARS+=("CC=$CC")

if [ "$check" = 0 ]; then
  # ONE make invocation for all the targets, not one each: they share ~100
  # translation units' worth of headers and the layout-hash step, and make is
  # what knows that.
  exec make -s -C c "${VARS[@]}" "${TARGETS[@]}"
fi

# ---- --check: two builds of one tree must agree ----------------------------
#
# Two independent PROCESSES, not two passes in one, for the reason gen.sh gives:
# whatever a run leaves behind in memory must not be what makes the second agree
# with the first. c/build is wiped between them so the second is a real compile
# and not make deciding there is nothing to do.
#
# BOTH BUILDS WRITE THE SAME PATHS, and that is deliberate. wasm-ld records a
# `name` custom section whose module-name subsection is the OUTPUT FILE'S
# BASENAME, so comparing a build written to `bots.wasm` against one written to a
# mktemp name reports a difference that is only the filename - it cost this repo
# a red check on every Linux CI run once already. Here the second build
# overwrites the first's paths and the copies are taken away to a temp dir, so
# the only thing left that can differ is what this check is for.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

PATHS=()
while IFS= read -r p; do [ -n "$p" ] && PATHS+=("$p"); done < <(rows "$want" | cut -d'|' -f2)
# The RAW modules too, not just the .gz. The .gz is a function of the raw bytes
# and the compressor, so a raw comparison is the sharper one: it isolates the
# compiler from `gzip`, which is the distinction this whole arrangement rests on.
RAW=()
while IFS= read -r p; do RAW+=("c/build/$(basename "${p%.gz}")"); done < <(printf '%s\n' "${PATHS[@]}")

build_once() { rm -rf c/build; make -s -C c "${VARS[@]}" "${TARGETS[@]}" >/dev/null; }

echo "wasm --check: first build"
build_once
mkdir -p "$tmp/a"
for f in "${PATHS[@]}" "${RAW[@]}"; do cp "$f" "$tmp/a/$(basename "$f")"; done

echo "wasm --check: second build"
build_once

bad=0
for f in "${PATHS[@]}" "${RAW[@]}"; do
  if cmp -s "$f" "$tmp/a/$(basename "$f")"; then
    printf '  same  %8d B  %s\n' "$(wc -c < "$f")" "$f"
  else
    printf '  DIFFER %s (%d B then %d B)\n' "$f" "$(wc -c < "$tmp/a/$(basename "$f")")" "$(wc -c < "$f")"
    bad=1
  fi
done

if [ "$bad" = 0 ]; then
  echo "wasm: reproducible - two builds of this tree wrote the same modules"
  exit 0
fi
cat >&2 <<'MSG'

::error::the wasm build is NOT a function of this tree - two builds of one
commit disagree

This is the property that lets CI build the shipped modules instead of a human
committing them, so it is a real failure and not a flake. Look for a __DATE__,
an address, an uninitialised pad, or an ordering that depends on a hash table's
memory layout. scripts/wasm_build.sh's header has the cross-platform
measurements this check defends.
MSG
exit 1
