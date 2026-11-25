# iclass.py PLAYJSON BINARY FUNCS...: instruction-class histogram for the named functions, from objdump -d of BINARY
# (the non-PIE cbase_dyn, so cachegrind/callgrind addresses are file addresses) weighted by the play-only Ir per address.
# Also prints the unweighted static count of each class over the whole function, so the two can be compared.
import json, re, subprocess, sys, collections
pj, binp = sys.argv[1], sys.argv[2]; funcs = sys.argv[3:]
raw = json.load(open(pj))["funcs"]
play = {}
for name, addrs in raw.items():
    base = name.split("'")[0]          # callgrind recursion suffix 'N
    tgt = play.setdefault(base, {})
    for a, ev in addrs.items():
        if a in tgt:
            for k in ev: tgt[a][k] = tgt[a].get(k, 0) + ev[k]
        else:
            tgt[a] = dict(ev)
def cls(mn):
    m = mn
    if m.startswith(("ldaxr", "stlxr", "ldxr", "stxr", "casal", "cas", "ldadd", "swp", "stadd", "ldclr", "ldeor", "ldset", "ldsmax", "ldumax")) or m.startswith(("casp", "ldaxp", "stlxp")): return "atomic"
    if m.startswith(("ldp",)) : return "ldp"
    if m.startswith(("stp",)) : return "stp"
    if m.startswith(("ldr", "ldur", "ldrb", "ldrh", "ldrs", "ldtr", "ldar", "ldapr", "ldnp", "ld1", "ld2", "ld3", "ld4")): return "load"
    if m.startswith(("str", "stur", "strb", "strh", "sttr", "stlr", "stnp", "st1", "st2", "st3", "st4")): return "store"
    if m.startswith(("sdiv", "udiv")): return "div"
    if m.startswith(("madd", "msub", "mul", "smull", "umull", "smulh", "umulh", "smaddl", "umaddl", "smsubl", "umsubl")): return "mul"
    if m.startswith(("b.", "b", "cbz", "cbnz", "tbz", "tbnz", "ret", "br", "blr", "bl")): return "branch"
    if m.startswith(("dmb", "dsb", "isb")): return "barrier"
    if m.startswith(("prfm",)): return "prefetch"
    if m.startswith(("ld", "st")): return "load_store_other"
    if re.match(r"^(f|s|u|cm|ext|zip|uzp|trn|dup|ins|movi|mov|add|sub|and|orr|eor|bic|shl|ushr|sshr|tbl|xtn|sxt|uxt|ushll|sshll|saddl|uaddl|cnt|addv|umov|smov|shl|sqadd|sqsub|rev|rbit)\b", m) and False: return "x"
    return "int_alu_other"
def simd(mn, ops):
    return "simd" if re.search(r"\b[vq]\d+\.|\b[vq]\d+\b", ops) else None
# objdump -d (one call for the whole binary) and per-function ranges from nm
nm = subprocess.run(["nm", "-n", binp], capture_output=True, text=True).stdout.splitlines()
syms = []
for l in nm:
    p = l.split()
    if len(p) == 3 and p[1] in "Tt": syms.append((int(p[0], 16), p[2]))
rng = {}
for i, (a, n) in enumerate(syms):
    b = syms[i+1][0] if i+1 < len(syms) else a+1
    rng.setdefault(n, (a, b))
dis = subprocess.run(["objdump", "-d", "--no-show-raw-insn", binp], capture_output=True, text=True).stdout
insns = []
for l in dis.splitlines():
    m = re.match(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$", l)
    if m: insns.append((int(m.group(1), 16), m.group(2), m.group(3)))
out = {}
for f in funcs:
    if f not in rng:
        out[f] = {"error": "symbol not in binary (libc or inlined)"}; continue
    a, b = rng[f]
    static = collections.Counter(); weighted = collections.Counter(); wtot = 0
    pmap = {}
    for addr_s, ev in play.get(f, {}).items():
        pmap[int(addr_s, 16)] = ev["Ir"]
    for addr, mn, ops in insns:
        if not (a <= addr < b): continue
        c = cls(mn)
        if c == "int_alu_other" and simd(mn, ops): c = "simd"
        static[c] += 1
        w = pmap.get(addr, 0)
        if w: weighted[c] += w; wtot += w
    out[f] = {"static_insns": sum(static.values()), "static": dict(static),
              "weighted_Ir_total": wtot, "weighted_pct": {k: round(100*v/wtot, 2) for k, v in weighted.items()} if wtot else {},
              "hot_addrs_with_ir": len(pmap)}
json.dump(out, sys.stdout, indent=1)
