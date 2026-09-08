#!/usr/bin/env bash
# Does this change leave a committed wasm artifact behind the kernel it is
# built from?
#
# The wasm modules are BUILT BY HAND, on a Mac, by whoever remembers. Twice that
# has cost us: `public/oracle.wasm.gz` sat unrebuilt from 2026-08-23, so the
# live Infinite Oracle was missing podkidnoy and the solver seat fix and served
# one of the two seats the opposite endgame verdict - shipped, and nothing
# noticed. This is the gate that notices.
#
# WHY NOT REBUILD AND DIFF THE BYTES, which is the obvious check: because a
# rebuild of UNMODIFIED main already differs from the committed artifact.
# Measured 2026-09-06 with Homebrew clang 22.1.8 + binaryen 130 on an untouched
# tree: bots.wasm.gz 67049 -> 67132 B, oracle.wasm.gz 71794 -> 71858 B, while
# guards_wasm.ts came out identical. The toolchain that produced the committed
# bytes is not the toolchain any given machine has, so a byte gate would be red
# forever and switched off within a week. Commit ORDER is the honest thing this
# repo can actually check.
#
# THE RULE, deliberately scoped to the change under review rather than to the
# whole history: if this branch touches a source a shipped wasm module is
# compiled from, it must also update at least one committed artifact - or
# re-stamp sdk/ts/wasm/WASM_STAMP, which the make targets write and which proves
# the build ran even when the output bytes are identical (see the stamp section
# below, and scripts/wasm_stamp.sh). It does
# not demand that the backlog of already-stale artifacts be paid off by whoever
# happens to touch the kernel next; it stops NEW drift at the door. The standing
# backlog is printed as a report either way, so it stays visible and measurable.
#
# Usage:
#   scripts/check_wasm_freshness.sh [base-ref]      # default origin/main
set -euo pipefail
cd "$(dirname "$0")/.."

BASE="${1:-origin/main}"

# The artifacts, and the make target that rebuilds each. Kept here rather than
# derived because a .gz is not mentioned by a variable the Makefile exports.
ARTIFACTS=(
  "sdk/ts/wasm/rules_wasm.ts|make -C c wasm"
  "sdk/ts/wasm/guards_wasm.ts|make -C c wasm-guards"
  "sdk/ts/wasm/bots.wasm.gz|make -C c wasm-bots"
  "public/oracle.wasm.gz|make -C c wasm-oracle"
  "public/oracle-mt.wasm.gz|make -C c wasm-oracle-mt"
)

# The sources, read out of the Makefile's own WASM_*_SRC lists - not a second
# copy - plus every header they can include and the Makefile that chooses the
# flags. The native CLI mains are deliberately NOT here: `main_elo.c` is in no
# wasm module, and reading a change to it as a stale artifact is how a gate
# earns its reputation for crying wolf.
# Deliberate exceptions: sources a change may edit without rebuilding, each with
# the reason beside it. The reason is the point - the entry is the diff someone
# gets to argue with (same contract as ALLOW in scripts/check_determinism.mjs).
# Empty today.
EXCEPTIONS=(
  # c/src/example.c   # why a change here cannot reach the shipped modules
)

# The source set comes from scripts/wasm_stamp.sh, which is also what the make
# targets hash into the stamp - one derivation, so the gate and the stamp cannot
# come to disagree about what a "wasm source" is.
SOURCES=()
while IFS= read -r f; do SOURCES+=("$f"); done < <(scripts/wasm_stamp.sh --list)
[ "${#SOURCES[@]}" -gt 0 ] || { echo "wasm_stamp.sh --list listed nothing - did the Makefile move?" >&2; exit 1; }
STAMP=sdk/ts/wasm/WASM_STAMP

# c/Makefile is NOT in that list, and the omission is deliberate. The flags it
# chooses do change the modules, but most edits to it are a new target or a
# comment, and a gate that demands a fresh binary for those is a gate somebody
# switches off by the end of the week. It is watched instead at LINE level
# below: a changed WASM_* assignment counts as a source change, a new phony
# target does not.

# ---- the report: where every artifact stands against those sources ----------
newest_src_commit=$(git log -1 --format=%H -- "${SOURCES[@]}")
echo "wasm sources last changed in $(git log -1 --format='%h %ad %s' --date=short "$newest_src_commit")"
echo
behind=0
for entry in "${ARTIFACTS[@]}"; do
  path="${entry%%|*}"; target="${entry##*|}"
  [ -e "$path" ] || { echo "  MISSING  $path (nothing has ever built it?)"; continue; }
  art_commit=$(git log -1 --format=%H -- "$path")
  if [ "$art_commit" = "$newest_src_commit" ] \
     || git merge-base --is-ancestor "$newest_src_commit" "$art_commit"; then
    printf '  fresh    %-30s %s\n' "$path" "$(git log -1 --format='%h %ad' --date=short "$art_commit")"
  else
    printf '  BEHIND   %-30s %s   (rebuild: %s)\n' "$path" \
      "$(git log -1 --format='%h %ad' --date=short "$art_commit")" "$target"
    behind=$((behind + 1))
  fi
done
echo
echo "$behind of ${#ARTIFACTS[@]} committed artifacts are behind the kernel."
echo "(reported, not failed - see the note at the top of this script)"
echo

# ---- the gate: does THIS change add to that? -------------------------------
if ! git rev-parse --verify --quiet "$BASE" >/dev/null; then
  echo "no $BASE to compare against - skipping the change-scoped gate"
  exit 0
fi
merge_base=$(git merge-base "$BASE" HEAD)
changed=$(git diff --name-only "$merge_base"...HEAD)
[ -n "$changed" ] || { echo "nothing changed against $BASE"; exit 0; }

watched=$(printf '%s\n' "${SOURCES[@]}" | sort -u)
if [ "${#EXCEPTIONS[@]}" -gt 0 ]; then
  watched=$(comm -23 <(printf '%s\n' "$watched") <(printf '%s\n' "${EXCEPTIONS[@]}" | sort -u))
fi
touched_src=$(comm -12 \
  <(printf '%s\n' "$watched") \
  <(printf '%s\n' "$changed" | sort -u))
# …plus the Makefile, but only when the diff moved a WASM_* assignment - the
# flags, caps and source lists that decide what the modules ARE. A new target or
# a comment does not qualify (see the note where SOURCES is built).
if printf '%s\n' "$changed" | grep -qx 'c/Makefile' \
   && git diff "$merge_base"...HEAD -- c/Makefile \
      | grep -qE '^[+-][A-Za-z0-9_]*WASM[A-Za-z0-9_]*[[:space:]]*[:?+]?='; then
  touched_src=$(printf '%s\nc/Makefile\n' "$touched_src" | sed '/^$/d' | sort -u)
fi
touched_art=$(comm -12 \
  <(printf '%s\n' "${ARTIFACTS[@]}" | sed 's/|.*//' | sort -u) \
  <(printf '%s\n' "$changed" | sort -u))

if [ -z "$touched_src" ]; then
  echo "this change touches no wasm source - nothing to rebuild"
  exit 0
fi
echo "this change touches wasm sources:"
printf '  %s\n' $touched_src
if [ -n "$touched_art" ]; then
  echo "…and rebuilds:"
  printf '  %s\n' $touched_art
fi

# THE STAMP is the proof, and it is the ONLY proof - a changed artifact is
# reported above but does not on its own satisfy this gate any more.
#
# Two reasons it replaced "at least one artifact moved" rather than joining it.
# First, an artifact touch was never evidence: it is satisfied by any edit to
# those files, including one made to get a red check green. Second, and this is
# what forced the change, a REAL rebuild can move nothing - four functions landed
# in msg_wire.c on the 1.1(52) release branch, msg_wire.c is compiled into all
# five modules, and every artifact came back byte-identical because no wasm
# export reaches those functions and the linker drops them. There was nothing to
# commit and no way to say "I built this". The alternatives were to exempt
# msg_wire.c forever, blinding the gate to exactly the drift it exists to catch,
# or to fake an artifact touch.
#
# The stamp is checked, not merely present: its recorded hash must equal the hash
# of the sources IN THIS TREE, so a stamp written before the last C edit fails
# just as a stale artifact would - and now fails even if an artifact did move.
if [ ! -f "$STAMP" ]; then
  echo "::error::$STAMP is missing - run: make -C c wasm wasm-guards wasm-bots wasm-oracle wasm-oracle-mt" >&2
  exit 1
fi
want=$(scripts/wasm_stamp.sh --hash)
got=$(awk '$1=="sha256"{print $2}' "$STAMP")
if [ "$want" = "$got" ]; then
  echo "…and $STAMP matches these sources ($want)."
  [ -n "$touched_art" ] || echo "No artifact moved, so the rebuild changed no shipped byte."
  exit 0
fi
cat >&2 <<MSG

::error::$STAMP does not match the sources in this tree

  stamp says   $got
  sources are  $want

The stamp is written by the wasm make targets, so this says the C moved after
the last build - whether or not an artifact is in this diff. Rebuild and commit
the result (a Mac needs WASM_CC=/opt/homebrew/opt/llvm/bin/clang; plain clang
there cannot target wasm32):

  make -C c wasm wasm-guards wasm-bots wasm-oracle wasm-oracle-mt

If a rebuild genuinely is not wanted in this PR, say so in the PR body and add
the paths you changed to the deliberate-exceptions list at the top of this
script - the entry is the diff someone gets to argue with.
MSG
exit 1
