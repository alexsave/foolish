#!/usr/bin/env bash
# Paired per-seed OUTCOME test: does a candidate CD_TT_BITS change how a solver
# bot FINISHES games vs a reference table? This is the decision-grade metric —
# move-hash divergence overcounts (it flags tie-break reshuffles between equally
# winning lines); an outcome flip is a real strength change. Same seed = same
# deal + same (TT-independent) opponent, so any fin= difference is attributable
# to the bot's table size alone. Use a strong opponent (espresso) — weak ones
# fail to punish degraded moves (measured: the TT8 flips only appear vs espresso).
#
#   tools/tt_divergence_viz/outcome_pair.sh [games] [seed0]
#   CAND=12 tools/tt_divergence_viz/outcome_pair.sh 2500 720000
#
# Env: BOT (octogen) OPP (espresso) PC (2) CAND (13) BASE (22) J (cores) CC
# Reference numbers (std build): TT8 flips 2/1000, TT13 flips 1/2500 — a candidate
# at TT12 should be <= 1/2500 with no excess win->loss.
set -euo pipefail
trap 'echo "[error] line $LINENO: \"$BASH_COMMAND\" exited $?" >&2' ERR
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CNITRO="$(cd "$HERE/../.." && pwd)"
CC=${CC:-cc}
BOT=${BOT:-octogen}; OPP=${OPP:-espresso}; PC=${PC:-2}
CAND=${CAND:-13}; BASE=${BASE:-22}
J=${J:-$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )}
N=${1:-1000}; SEED0=${2:-700000}
# EXTRA: extra compile flags for BOTH builds (e.g. EXTRA=-DCD_TT_2WAY to
# gate a candidate design; the CAND/BASE pair then measures it at two sizes).
EXTRA=${EXTRA:-}
throttle() { while [ "$(jobs -rp | wc -l)" -ge "$J" ]; do wait "$(jobs -rp | head -1)"; done; }

build_bin() { local b=$1
  local out="$HERE/eval_pair${b}$(echo "$EXTRA" | tr -dc 'A-Z0-9')"
  BIN_OUT="$out"
  [ -x "$out" ] && [ "$out" -nt "$CNITRO/src/cordite_sim.c" ] && return
  local CORE; CORE=$(cd "$CNITRO" && make -s print-core)
  ( cd "$CNITRO" && $CC -O2 -ffast-math -Isrc -Wno-deprecated-declarations \
      $EXTRA -DCD_TT_BITS="$b" $CORE src/main_eval.c -o "$out" -lm ); }

echo "[pair] $BOT vs $OPP pc$PC  TT$CAND vs TT$BASE  +$N seeds from $SEED0  extra='$EXTRA'"
build_bin "$BASE"; BASE_BIN="$BIN_OUT"
build_bin "$CAND"; CAND_BIN="$BIN_OUT"

per=$(( N / J )); rem=$(( N - per*J ))
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
run() { local bin=$1 tag=$2 i off=$SEED0 g
  for (( i=0; i<J; i++ )); do
    g=$per; [ "$i" -lt "$rem" ] && g=$(( per+1 )); [ "$g" -gt 0 ] || continue
    GAME_SIG=1 CD_BUDGET=prod CD_RACE=1 CD_RACE_C=75 "$bin" \
      --strategy="$BOT" --opp="$OPP" --players="$PC" --games="$g" --seed-start="$off" \
      2>/dev/null | awk '/^SIG/{print $2, $3, $4}' > "$work/${tag}_$i.txt" &
    off=$(( off+g ))
  done; wait
  sort -n "$work"/${tag}_*.txt > "$work/$tag.txt"
}
t0=$SECONDS
run "$BASE_BIN" base
run "$CAND_BIN" cand
echo "[pair] runs done ($(( SECONDS-t0 ))s)"

# join on seed -> "seed hash_base fin_base hash_cand fin_cand" (5 fields)
join "$work/base.txt" "$work/cand.txt" | awk -v C="$CAND" -v B="$BASE" '
{
  hb=$2; fb=$3; hc=$4; fc=$5; tot++;
  if (hb != hc) movediv++;
  if (fb != fc) {
    flip++;
    if (fb=="fin=1" && fc!="fin=1")      w2l++;
    else if (fb!="fin=1" && fc=="fin=1") l2w++;
    printf "  OUTCOME FLIP seed=%s TT%s=%s TT%s=%s\n", $1, B, fb, C, fc > "/dev/stderr";
  }
}
END {
  printf "games=%d  move-diverged=%d (%.3f%%)  OUTCOME-flipped=%d (%.3f%%)\n",
         tot, movediv, (tot?movediv/tot*100:0), flip, (tot?flip/tot*100:0);
  printf "  of flips: TT%s-win->TT%s-lose=%d   TT%s-lose->TT%s-win=%d\n",
         B, C, w2l+0, B, C, l2w+0;
}'
