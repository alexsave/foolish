#!/usr/bin/env bash
# Rebuild the transposition-table divergence page from the banked measurements.
#
#   tools/tt_divergence_viz/generate.sh        # data/ -> docs/tt-divergence.html
#
# The page is self-contained; open docs/tt-divergence.html directly.
#
# Data (all under data/, checked in so the page renders without re-measuring):
#   data/W/<bot>_pc<pc>.gw     per-game seed-keyed working sets
#   data/ccdf.json             aggregated CCDF curves (rebuilt from data/W)
#   data/measured.json         hand-entered direct-divergence points (tt_divergence.sh)
#
# Taking NEW measurements is no longer possible from this tree: W came from the
# solver's -DCD_TT_STATS occupancy census and main_eval's CD_GW emitter, both
# deleted once the research concluded. Restore them from git history (the commit
# that removed the dead C build flags) if a fresh sweep is ever needed.
set -euo pipefail
trap 'echo "[error] line $LINENO: \"$BASH_COMMAND\" exited $?" >&2' ERR
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

node "$HERE/ccdf.mjs" "$HERE/data/W" > "$HERE/data/ccdf.json"
node "$HERE/build.mjs"
echo "[done] refresh docs/tt-divergence.html in your browser"
