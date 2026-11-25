# fn_sums.py DIFF MOVES OUTPREFIX: per-function sums of every event from a cachegrind-format diff (cg_diff output, the
# play-only counts), top 25 by each of Ir, D1mr, DLmr, D1mw, and per-move values (divided by MOVES).
import sys, re
path, moves, pref = sys.argv[1], int(sys.argv[2]), sys.argv[3]
ev = None; names = {}; fn = None; sums = {}
def resolve(tok):
    if tok.startswith("("):
        i = tok.index(")"); ident = tok[:i+1]; rest = tok[i+1:].strip()
        if rest: names[ident] = rest
        return names.get(ident, ident)
    return tok
for line in open(path):
    line = line.rstrip("\n")
    if line.startswith("events:"): ev = line.split()[1:]; continue
    if line.startswith("fn="): fn = resolve(line[3:]); continue
    if line.startswith(("fl=", "fi=", "fe=")): resolve(line.split("=", 1)[1]); continue
    p = line.split()
    if len(p) < 2 or fn is None or ev is None or not p[0].lstrip("-+").isdigit(): continue
    v = [int(x) for x in p[1:1+len(ev)]]
    acc = sums.setdefault(fn, [0]*len(ev))
    for i, x in enumerate(v): acc[i] += x
tot = [sum(s[i] for s in sums.values()) for i in range(len(ev))]
print("events", ev); print("total", dict(zip(ev, tot)))
for key in ["Ir", "D1mr", "DLmr", "D1mw"]:
    i = ev.index(key)
    rows = sorted(sums.items(), key=lambda kv: -kv[1][i])[:25]
    with open(f"{pref}_{key}.txt", "w") as f:
        f.write(f"top 25 functions by play-only {key}; moves={moves}; per-move column = total/moves\n")
        f.write(f"{'total':>16} {'per_move':>10} {'share%':>7}  function\n")
        for name, v in rows:
            f.write(f"{v[i]:>16,} {v[i]/moves:>10.2f} {100*v[i]/max(1,tot[i]):>7.2f}  {name}\n")
    print(key, "written", f"{pref}_{key}.txt")
