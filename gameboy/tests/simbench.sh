#!/bin/sh
# The rollout engine timed on the Game Boy: the ROM must play the clang
# build's plies; then the time per playout, and (second ROM, with phase marks)
# how a ply's time splits.
cd "$(dirname "$0")/.." || exit 2
want=$(build/simbench_host) || exit 1
printf '@1 until _sb_done 01\n+0 expect _sb_plies %s\n+0 peek _sb_lines_sum 4\n+0 peek _sb_lines_max 4\n' "$want" > build/simbench_script.txt
out=$(build/gbrun build/simbench.gb build/simbench_script.txt build/simbench.noi) || { echo "$out"; exit 1; }
printf '@1 until _sb_done 01\n+0 expect _sb_plies %s\n+0 peek _sb_phase 12\n' "$want" > build/simprof_script.txt
prof=$(build/gbrun build/simprof.gb build/simprof_script.txt build/simprof.noi) || { echo "$prof"; exit 1; }
printf '%s\n%s\n' "$out" "$prof" | python3 -c '
import sys
v = {}
for l in sys.stdin:
    if ": peek " in l or ": expect " in l:
        name = l.split()[3]; b = [int(x, 16) for x in l.split("=")[1].split() if x != "ok"]
        if name == "_sb_phase": v["ph"] = [sum(x << (8 * i) for i, x in enumerate(b[4 * k:4 * k + 4])) for k in range(3)]
        else: v[name] = sum(x << (8 * i) for i, x in enumerate(b))
runs = 64
avg = v["_sb_lines_sum"] / runs; plies = v["_sb_plies"] / runs
print("gb: %d playouts, %.1f plies each, %.0f scanlines each (%.1f ms), the slowest %d (%.1f ms); %.2f ms a ply" % (
    runs, plies, avg, avg * 0.1087, v["_sb_lines_max"], v["_sb_lines_max"] * 0.1087, avg * 0.1087 / plies))
ph = v["ph"]; t = sum(ph)
print("gb: a ply is %.0f%% the policy, %.0f%% apply and refill, %.0f%% the draws (from the marked ROM)" % tuple(100.0 * x / t for x in ph))'
