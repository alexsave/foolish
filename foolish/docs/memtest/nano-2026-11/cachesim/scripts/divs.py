# divs.py PLAYJSON BINARY MOVES: every executed udiv/sdiv (and the msub that follows, which is the modulo) in the play-only
# callgrind data, with the function, address, source line (addr2line) and executions per move.
import json, re, subprocess, sys, collections
pj, binp, moves = sys.argv[1], sys.argv[2], int(sys.argv[3])
raw = json.load(open(pj))["funcs"]
ir = collections.defaultdict(int); fn = {}
for name, addrs in raw.items():
    base = name.split("'")[0]
    for a, ev in addrs.items():
        ir[int(a, 16)] += ev.get("Ir", 0); fn[int(a, 16)] = base
dis = subprocess.run(["objdump", "-d", "--no-show-raw-insn", binp], capture_output=True, text=True).stdout
rows = []; prev = None; insns = []
for l in dis.splitlines():
    m = re.match(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$", l)
    if m: insns.append((int(m.group(1), 16), m.group(2), m.group(3)))
for i, (a, mn, ops) in enumerate(insns):
    if mn in ("udiv", "sdiv") and ir.get(a, 0):
        nxt = insns[i+1][1] if i+1 < len(insns) else ""
        rows.append((ir[a], a, fn.get(a, "?"), mn, ops, nxt))
rows.sort(reverse=True)
addrs = [hex(r[1]) for r in rows]
src = subprocess.run(["addr2line", "-e", binp, "-f", "-i"] + addrs, capture_output=True, text=True).stdout.splitlines() if addrs else []
tot = sum(r[0] for r in rows)
print(f"total executed divides per move: {tot/moves:.1f} over {len(rows)} div sites ({moves} moves)")
j = 0
for r in rows:
    # addr2line -f -i prints function then file:line per frame; take the first pair per address
    func, loc = src[j], src[j+1]; j += 2
    while j < len(src) and src[j-0] and False: pass
    print(f"{r[0]/moves:7.2f}/move  {r[3]} {r[4]:<20} next={r[5]:<5} {r[2]:<32} {loc}")
