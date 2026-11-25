#!/usr/bin/env python3
# pushlat.py RUNDIR [PW0 PW1]: move latency and push latency from the ev.* files of one load.sh run directory (scratch analysis).
# Moves: the M lines (M gid play_v t_send_us t_reply_us version_after) whose t_reply_us (load-box CLOCK_MONOTONIC) is in
# [t_go+PW0, t_go+PW1] (t_go from t_go_clk), the same rule as load.sh playwin. A move is applied unless the line before it in
# the same file is an X line (a refusal) for the same game and play_v.
# Push latency of an applied move: in the OTHER player's ev file of the same rdv directory, the first R line
# (R gid version t_recv_us kind) of the same game with version >= version_after, minus the move's t_send_us.
# Both players run on the load box and stamp CLOCK_MONOTONIC, so the difference needs no cross-box clock.
# Prints one line of key=value pairs.
import sys, os, glob, bisect
d = sys.argv[1]; P0 = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0; P1 = float(sys.argv[3]) if len(sys.argv) > 3 else 23.0
w = open(d + '/t_go_clk').read().split(); m0 = float(w[3])
a, b = (m0 + P0) * 1e6, (m0 + P1) * 1e6
lat = []; push = []; missing = 0; refused = 0; nonmono = 0
pc = lambda v, q: v[min(len(v) - 1, int(q * (len(v) - 1) + .5))] if v else float('nan')
for rdv in sorted(glob.glob(d + '/rdv*')):
    evs = sorted(glob.glob(rdv + '/ev.*'))
    if len(evs) != 2: continue
    parsed = []
    for f in evs:
        moves = []; recv = {}; prev = None
        for l in open(f):
            p = l.split()
            if not p: continue
            if p[0] == 'M':
                applied = not (prev and prev[0] == 'X' and prev[1] == p[1] and prev[2] == p[2])
                moves.append((p[1], int(p[3]), int(p[4]), int(p[5]), applied))
            elif p[0] == 'R':
                g = recv.setdefault(p[1], ([], []))
                if g[0] and int(p[2]) < g[0][-1]: nonmono += 1
                g[0].append(int(p[2])); g[1].append(int(p[3]))
            prev = p
        parsed.append((moves, recv))
    for me, other in ((0, 1), (1, 0)):
        for gid, t0, t1, v, applied in parsed[me][0]:
            if not (a <= t1 < b): continue
            lat.append((t1 - t0) / 1000)
            if not applied: refused += 1; continue
            g = parsed[other][1].get(gid)
            if not g: missing += 1; continue
            vs, ts = g
            # the R versions of a game never go down in file order (checked: nonmono counts the exceptions), so the
            # first index with version >= v is the first receipt in time of a version >= v
            i = bisect.bisect_left(vs, v)
            if i >= len(vs): missing += 1; continue
            push.append((ts[i] - t0) / 1000)
lat.sort(); push.sort()
W = P1 - P0
print(f"moves={len(lat)} window_s={W:.2f} moves_per_s={len(lat)/W:.1f} move_p50_ms={pc(lat,.5):.2f} move_p95_ms={pc(lat,.95):.2f} "
      f"move_p99_ms={pc(lat,.99):.2f} move_max_ms={(lat[-1] if lat else float('nan')):.2f} refused={refused} "
      f"push_n={len(push)} push_missing={missing} push_p50_ms={pc(push,.5):.2f} push_p99_ms={pc(push,.99):.2f} "
      f"push_max_ms={(push[-1] if push else float('nan')):.2f} push_over_100ms={sum(1 for x in push if x > 100)} push_neg={sum(1 for x in push if x < 0)} r_nonmono={nonmono}")
