#!/usr/bin/env bash
# Round-robin NxN win-rate matrix: every strategy (protagonist, seat 0) vs
# every strategy (opponent) in 2-player games. Each cell = protagonist's
# first-place rate. Parallelized one cnitro_eval per cell across all cores,
# with a live \r progress line; renders the full matrix when done.
#
# Usage:   nxn_matrix.sh <games> <outdir> [seed-start]
# Env:     TRIANGLE=1  compute only the upper triangle+diagonal (2x faster;
#                      Durak 1v1 is zero-sum so cell(B,A)=100-cell(A,B)).
#          PAR=<n>     override parallelism (default: cores-1).
# Resumable: existing cell files are skipped, so Ctrl-C then rerun continues.
set -u
GAMES="${1:?usage: nxn_matrix.sh <games> <outdir> [seed-start]}"
OUT="${2:?outdir}"; SEED="${3:-700001}"
HERE="$(cd "$(dirname "$0")" && pwd)"
EVAL="$HERE/../build/cnitro_eval"
mkdir -p "$OUT/cells"

STRATS=(random espresso handwritten robusta firecracker gunpowder blackpowder \
        cordite astrolite cordite_old simple_heuristic champion ultimate_champion \
        hacker fulminate espresso_prod handwritten_prod distilled semtex \
        semtex_oracle octogen octogen_oracle torpex novichok)
printf '%s\n' "${STRATS[@]}" > "$OUT/strats.txt"
N=${#STRATS[@]}
TRIANGLE="${TRIANGLE:-0}"
if [ "$TRIANGLE" = 1 ]; then TOTAL=$(( N*(N+1)/2 )); else TOTAL=$(( N*N )); fi

# Build the job list (skip cells already on disk so runs resume).
JOBS="$OUT/jobs.txt"; : > "$JOBS"
i=0
for a in "${STRATS[@]}"; do
  j=0
  for b in "${STRATS[@]}"; do
    if [ "$TRIANGLE" = 1 ] && [ "$j" -lt "$i" ]; then j=$((j+1)); continue; fi
    [ -s "$OUT/cells/$a.__.$b" ] || echo "$a $b" >> "$JOBS"
    j=$((j+1))
  done
  i=$((i+1))
done

run_cell() {
  a="$1"; b="$2"
  line=$("$EVAL" --strategy="$a" --opp="$b" --players=2 --games="$GAMES" \
                 --seed-start="$SEED" 2>/dev/null | awk '$1==2{print $4}')
  echo "$a $b ${line:-NA}" > "$OUT/cells/$a.__.$b"
}
export -f run_cell
export EVAL GAMES SEED OUT

NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)
PAR="${PAR:-$(( NPROC > 2 ? NPROC - 1 : 1 ))}"
QUEUED=$(wc -l < "$JOBS" | tr -d ' ')
DONE0=$(( TOTAL - QUEUED ))
echo "NxN matrix: $TOTAL cells ($GAMES games each), $PAR-way parallel, resuming with $DONE0 done." >&2

# Launch the workers in the background, then repaint the whole grid in place
# (top-of-screen home + reprint) as cells fill — an empty table appears first
# and its cells turn from · into percentages live.
xargs -P "$PAR" -L1 bash -c 'run_cell "$0" "$1"' < "$JOBS" &
XPID=$!

fmt() { printf '%d:%02d' $(( $1/60 )) $(( $1%60 )); }
START=$(date +%s)
TTY=0; [ -t 1 ] && TTY=1

status() {   # progress footer, printed under the grid every tick
  local done pct el newc eta last lr
  done=$(ls "$OUT/cells" 2>/dev/null | wc -l | tr -d ' ')
  pct=$(( TOTAL ? done*100/TOTAL : 0 ))
  el=$(( $(date +%s) - START )); newc=$(( done - DONE0 )); eta="--:--"
  [ "$newc" -gt 0 ] && eta=$(fmt $(( el * (TOTAL-done) / newc )))
  last=$(ls -t "$OUT/cells" 2>/dev/null | head -1)
  lr=""; [ -n "$last" ] && lr=$(awk '{printf "%s>%s %s%%",$1,$2,$3}' "$OUT/cells/$last" 2>/dev/null)
  local bar=$(( pct*30/100 )) fill pad
  fill=$(printf '%*s' "$bar" '' | tr ' ' '#'); pad=$(printf '%*s' $((30-bar)) '')
  printf '[%s%s] %3d%%  %d/%d  elapsed %s  eta %s   last %-28s' \
         "$fill" "$pad" "$pct" "$done" "$TOTAL" "$(fmt "$el")" "$eta" "$lr"
}

if [ "$TTY" = 1 ]; then printf '\033[2J\033[H'; fi   # clear once; grid pins to top
while kill -0 "$XPID" 2>/dev/null; do
  if [ "$TTY" = 1 ]; then
    { printf '\033[H'; python3 "$HERE/nxn_render.py" "$OUT" --live
      printf '\033[K\n\033[K%s\033[K' "$(status)"; } 2>/dev/null
  else
    status >&2; echo >&2
  fi
  sleep 1
done
wait "$XPID"
if [ "$TTY" = 1 ]; then printf '\033[2J\033[H'; fi
echo "done: $(ls "$OUT/cells"|wc -l|tr -d ' ')/$TOTAL cells in $(fmt $(( $(date +%s)-START )))." >&2
echo >&2
python3 "$HERE/nxn_render.py" "$OUT"
