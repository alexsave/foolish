#!/bin/sh
# One random playout on the Game Boy, timed: the ROM must play the clang
# build's moves (the total is compared), and the time per playout is printed.
cd "$(dirname "$0")/.." || exit 2
want=$(build/playout_host) || exit 1
printf '@1 until _po_done 01\n+0 expect _po_moves_total %s\n+0 peek _po_lines_sum 4\n+0 peek _po_lines_max 4\n+0 peek _po_moves_max 2\n+0 peek _po_phase 16\n' "$want" > build/playout_script.txt
out=$(build/gbrun build/playout.gb build/playout_script.txt build/playout.noi) || { echo "$out"; exit 1; }
echo "$out"
echo "$out" | python3 -c '
import sys
v = {}
for l in sys.stdin:
    if ": peek " in l or ": expect " in l:
        name = l.split()[3]; b = [int(x, 16) for x in l.split("=")[1].split() if x != "ok"]
        if name == "_po_phase":
            v["phase"] = [sum(x << (8 * i) for i, x in enumerate(b[4 * k:4 * k + 4])) for k in range(4)]
        else:
            v[name] = sum(x << (8 * i) for i, x in enumerate(b))
runs = 64
avg = v["_po_lines_sum"] / runs
print("gb: %d playouts, %.0f scanlines each (%.1f ms), the slowest %d (%.0f ms); %.1f moves a playout, %.2f ms a move" % (
    runs, avg, avg * 0.1087, v["_po_lines_max"], v["_po_lines_max"] * 0.1087,
    v["_po_moves_total"] / runs, avg * 0.1087 / (v["_po_moves_total"] / runs)))
ph = v["phase"]; tot = sum(ph)
print("gb: a move is %.0f%% the two menu counts, %.0f%% the draw, %.0f%% the unrank, %.0f%% the apply (from a second, marked pass)" % tuple(100.0 * x / tot for x in ph))'
