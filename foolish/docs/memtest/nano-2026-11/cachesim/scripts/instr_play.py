# instr_play.py FULL SETUP OUT_JSON: per-instruction play-only self counts (full minus setup) from two callgrind files written
# with --dump-instr=yes and the cache simulation on. Event names come from each file's "events:" line. Callgrind's
# "calls=" lines are followed by an inclusive cost line, which is skipped so only self cost is counted. Addresses use
# callgrind's compression: 0xHEX absolute, +N / -N relative to the previous address, * the same as the previous one.
import sys, json
def parse(path):
    data = {}; names = {}; fn = None; prev = 0; ev = None; skip_next = False
    def resolve(tok):
        if tok.startswith("("):
            i = tok.index(")"); ident = tok[:i+1]; rest = tok[i+1:].strip()
            if rest: names[ident] = rest
            return names.get(ident, ident)
        return tok
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("events:"):
            ev = line.split()[1:]; continue
        if line.startswith("fn="):
            fn = resolve(line[3:]); continue
        if line.startswith(("fl=", "fi=", "fe=", "cfl=", "cfi=")):
            resolve(line.split("=", 1)[1]); continue
        if line.startswith("cfn="):
            resolve(line[4:]); continue
        if line.startswith("calls="):
            skip_next = True; continue
        parts = line.split()
        if len(parts) < 3 or fn is None or ev is None: continue
        if skip_next:
            skip_next = False; continue
        a = parts[0]
        if a.startswith("0x"): cur = int(a, 16)
        elif a == "*": cur = prev
        elif a[0] in "+-" and a[1:].isdigit(): cur = prev + int(a)
        else: continue
        prev = cur
        cnt = [int(x) for x in parts[2:2+len(ev)]]
        d = data.setdefault(fn, {})
        acc = d.get(cur, [0]*len(ev))
        for i, v in enumerate(cnt): acc[i] += v
        d[cur] = acc
    return ev, data
full, setup, out = sys.argv[1], sys.argv[2], sys.argv[3]
evf, F = parse(full); evs, S = parse(setup)
assert evf == evs, (evf, evs)
res = {}
for f, addrs in F.items():
    for a, v in addrs.items():
        s = S.get(f, {}).get(a, [0]*len(evf))
        p = [x - y for x, y in zip(v, s)]
        if any(p):
            res.setdefault(f, {})[hex(a)] = {k: p[i] for i, k in enumerate(evf)}
json.dump({"events": evf, "funcs": res}, open(out, "w"))
tot = sum(sum(x["Ir"] for x in d.values()) for d in res.values())
print("events", evf); print("functions", len(res), "play Ir total", tot)
