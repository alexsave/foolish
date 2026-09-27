#!/usr/bin/env python3
"""Does a slower connection make you the fool?

Four wellbehaved clients, identical in every way except how long they take to
answer, on a clean wire - no loss, no duplication, no jitter. The lineup is
rotated so each latency sits in each seat exactly once, because Durak's opening
seat is derived from the deal and seat position would otherwise be confounded
with latency.

CAVEAT worth keeping in view: these clients pick uniformly from the legal menu,
the same policy class as the `random` bot, which tools/speed_sweep.py shows is
the one speed helps. A client playing a deliberate policy would be expected to
look like handwritten/octogen there, i.e. flat.
"""
import subprocess, collections, math
LAT = [50, 200, 800, 2000]
tot = collections.Counter(); games = 0
# rotate so each latency sits in each seat exactly once: seat position is not
# neutral in Durak, and without rotating it would be confounded with latency.
for r in range(len(LAT)):
    order = LAT[r:] + LAT[:r]
    cmd = ["./foolyard", "--lineup", ",".join(f"wellbehaved@{m}" for m in order),
           "--games", "8", "--secs", "1000000", "--until-games", "150",
           "--jitter", "0", "--loss", "0", "--dup", "0", "--seed", str(7000+r), "--csv"]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    for line in out.splitlines():
        if line.startswith("CSV,"):
            _, _, seat, _n, ms, fools, fin, moves = line.split(",")
            tot[int(ms)] += int(fools); games = games and games or 0
            if int(seat) == 0: games += int(fin)
n = sum(tot.values())
print(f"{n} finished games, 4 rotations pooled, no loss/dup/jitter\n")
print(f"{'latency':>9}{'fool':>7}{'share':>9}{'z':>8}")
for ms in LAT:
    obs = tot[ms]/n; exp = 0.25
    z = (obs-exp)/math.sqrt(exp*(1-exp)/n)
    print(f"{ms:>7}ms{tot[ms]:>7}{obs:>9.3f}{z:>8.1f}")
