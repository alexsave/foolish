# -*- coding: utf-8 -*-
"""Measure Ultimate Tic-Tac-Toe's actual information content.

Not estimated. Plays complete legal games and sums log2(legal moves) at every
ply, which IS what an arithmetic or rANS coder against a uniform model costs.
"""
import math, random, json
from collections import Counter

LINES = [(0,1,2),(3,4,5),(6,7,8),(0,3,6),(1,4,7),(2,5,8),(0,4,8),(2,4,6)]

def winner(c):
    for a,b,d in LINES:
        if c[a] and c[a] == c[b] == c[d]: return c[a]
    return 'd' if all(c) else None

def play(rng):
    cells = [[0]*9 for _ in range(9)]
    status = [None]*9
    forced, turn = None, 1
    legal_counts, moves = [], []
    while True:
        blocks = ([forced] if forced is not None and status[forced] is None
                  else [b for b in range(9) if status[b] is None])
        opts = [(b,c) for b in blocks for c in range(9) if cells[b][c] == 0]
        if not opts: return legal_counts, moves, 'draw'
        legal_counts.append(len(opts))
        b, c = rng.choice(opts)
        moves.append(b*9+c)
        cells[b][c] = turn
        w = winner(cells[b])
        if w: status[b] = w
        meta = winner([{1:1,2:2,'d':3}.get(s,0) if s else 0 for s in status])
        if meta in (1,2): return legal_counts, moves, 'win'
        if all(s is not None for s in status): return legal_counts, moves, 'draw'
        forced, turn = c, 3 - turn

rng = random.Random(11)
games = [play(rng) for _ in range(20000)]

bits   = [sum(math.log2(n) for n in g[0]) for g in games]
plies  = [len(g[0]) for g in games]
first  = games[0][0]

def pct(v, p): 
    v = sorted(v); return v[min(len(v)-1, int(len(v)*p))]

out = {
 "games": len(games),
 "plies_mean": sum(plies)/len(plies),
 "plies_p50": pct(plies,.5), "plies_p95": pct(plies,.95), "plies_max": max(plies),
 "bits_mean": sum(bits)/len(bits),
 "bits_p50": pct(bits,.5), "bits_p95": pct(bits,.95), "bits_max": max(bits),
 "bytes_p95": pct(bits,.95)/8,
 "naive_state_bits": 81*2,
 "naive_move_bits_p95": pct(plies,.95)*7,
 "first_game_counts": first,
 "mean_legal_by_ply": None,
}
# average legal-move count at each ply, for the chart
mx = max(plies)
acc = [0.0]*mx; cnt = [0]*mx
for g in games:
    for i, n in enumerate(g[0]):
        acc[i] += math.log2(n); cnt[i] += 1
out["mean_bits_by_ply"] = [round(acc[i]/cnt[i], 3) for i in range(mx) if cnt[i] > 40]
out["plies_hist"] = sorted(Counter(plies).items())
print(json.dumps(out))
