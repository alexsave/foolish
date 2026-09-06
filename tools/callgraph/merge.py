#!/usr/bin/env python3
"""Merge the four per-language graphs into one dataset.

Adds: semantic category per node (the tint), cross-language edges through the
wasm exports and the iOS C bridge, degree/fan-in stats, and a module rollup.
"""
import json, collections, os, re, sys

WORK = os.path.abspath(sys.argv[1])

# --- category classifier ---------------------------------------------------
# Ordered rules: first match wins. Categories are semantic groups that mean the
# same thing in every language, so the same tint reads across C/TS/Swift/Rust.
RULES = [
    # Every .sql file is the database tier, including the schema dump e2e/ uses.
    ("server",   [r"\.sql$"]),
    ("test",     [r"(^|/)(tests?|e2e)/", r"(^|/)[A-Za-z_]*Tests?/", r"\.test\.[a-z]+$", r"_test\.[a-z]+$", r"(^|/)test_"]),
    ("tools",    [r"^c/tools/", r"^scripts/", r"^next\.config", r"^[a-z_.]+\.(m?js|ts)$", r"^offlinefun/", r"^ios/Tools/", r"/bin/", r"^rustpoc/rs/src/bin/"]),
    ("bridge",   [r"^c/wasm/", r"^c/ios/", r"^sdk/ts/wasm/", r"^src/wasm/", r"^sdk/swift/"]),
    ("imessage", [r"^ios/FoolishMessages/", r"^ios/FoolishKit/Messages/"]),
    ("server",   [r"^server/", r"supabase"]),
    ("network",  [r"^ios/FoolishNet/", r"ServerContext", r"AuthContext", r"(^|/)net(work)?\.", r"^src/backend/"]),
    ("bots",     [r"strategy", r"bot", r"cordite", r"_sim\.c$", r"oracle", r"Oracle"]),
    ("anim",     [r"anim", r"Anim", r"beat", r"Beat"]),
    ("wire",     [r"wire", r"Wire", r"replay", r"Replay", r"codec", r"msg_"]),
    ("ui",       [r"^src/components/", r"^src/app/", r"^ios/FoolishKit/Boards/",
                  r"^ios/FoolishKit/DesignSystem/", r"^ios/WatchUI/", r"^ios/HarnessUI/",
                  r"^ios/FoolishApp/", r"\.tsx$", r"View\.swift$", r"^src/localization/"]),
    ("state",    [r"^src/state/", r"^src/contexts/", r"^src/utils/", r"^src/hooks/",
                  r"Flags\.swift$",
                  r"^ios/FoolishKit/Storage/", r"^ios/Entitlements/"]),
    ("rules",    [r"^c/src/", r"^rustpoc/"]),
]
# Everything a language brings with it - libc, the DOM and JS builtins,
# Foundation/SwiftUI, Rust std - plus third-party packages. Called by name from
# our code, defined outside the repo.
PLATFORM_FILE = {
    "sql": "(platform) Postgres built-ins",
    "c": "(platform) C standard library",
    "ts": "(platform) JavaScript, DOM & packages",
    "swift": "(platform) Swift, Foundation & SwiftUI",
    "rust": "(platform) Rust std",
}
COMPILED = [(c, [re.compile(p) for p in ps]) for c, ps in RULES]

def categorize(node):
    f = node.get("file", "")
    if f.startswith("(platform)"):
        return "platform"
    if not f or f == "(unresolved)":
        return "unresolved"
    for cat, pats in COMPILED:
        for p in pats:
            if p.search(f):
                return cat
    return "other"

# --- load ------------------------------------------------------------------
nodes, edges = {}, {}
per_lang = {}
for lang in ("c", "ts", "swift", "rust", "sql"):
    d = json.load(open(os.path.join(WORK, lang + ".json")))
    per_lang[lang] = (len(d["nodes"]), len(d["edges"]))

    # Each analyzer emits its unplaceable callees as a bare `ext:<name>` (or
    # `rust:(unresolved)#<name>`), so C's `map` and Swift's `map` and
    # TypeScript's `map` all collided on one node. Namespace them by the
    # language that called them: a platform symbol belongs to its language.
    def fix(i):
        if i.startswith("ext:"):
            return "ext:%s#%s" % (lang, i[4:])
        if i.startswith("rust:(unresolved)#"):
            return "ext:rust#" + i.split("#", 1)[1]
        return i

    for n in d["nodes"]:
        i = fix(n["id"])
        if i != n["id"]:
            n["id"] = i
            n["lang"] = lang
            n["file"] = PLATFORM_FILE[lang]
            n["kind"] = "platform"
        nodes.setdefault(n["id"], n)
    for e in d["edges"]:
        edges.setdefault((fix(e["s"]), fix(e["t"])), e.get("conf", "med"))

print("per-language raw:", per_lang, file=sys.stderr)

# --- cross-language wiring -------------------------------------------------
# 1. wasm exports: TS calls `ex.wasm_foo()`; the definition is a C function of
#    that exact name in c/wasm/. The TS analyzers left those as `ext:wasm_foo`
#    or as an interface member `ts:...#EngineExports.wasm_foo` with no node.
c_by_name = collections.defaultdict(list)
for n in nodes.values():
    if n["lang"] == "c" and n.get("kind") == "function":
        c_by_name[n["name"]].append(n["id"])

def wasm_target(tid):
    m = re.search(r"(?:^ext:|#(?:[A-Za-z]+\.)?)(wasm_[A-Za-z0-9_]+)$", tid)
    if not m:
        return None
    hits = c_by_name.get(m.group(1)) or []
    # prefer the c/wasm/ definition when the symbol is defined more than once
    pref = [h for h in hits if "/wasm/" in h] or hits
    return pref[0] if len(pref) >= 1 else None

rewired = 0
new_edges = {}
for (s, t), conf in edges.items():
    if t not in nodes:
        wt = wasm_target(t)
        if wt:
            new_edges[(s, wt)] = "cross"
            rewired += 1
            continue
    new_edges[(s, t)] = conf
edges = new_edges
print("wasm cross edges wired:", rewired, file=sys.stderr)

# --- the database seam -----------------------------------------------------
# Two directions. TypeScript calls a Postgres routine by name through PostgREST
# (`supabase.rpc('commit_game')`); a cron job calls back the other way, over
# HTTP into a Deno edge function. Neither is visible to a compiler on either
# side, so both are anchored here.
sql_by_name = {n["name"]: n["id"] for n in nodes.values()
               if n["lang"] == "sql" and n.get("kind") == "function"}
edgefn_by_dir = {}
for n in nodes.values():
    f = n.get("file", "")
    if n["lang"] == "ts" and "/functions/" in f and f.endswith("/index.ts"):
        edgefn_by_dir.setdefault(f.split("/functions/")[1].split("/")[0], []).append(n["id"])

seam = collections.Counter()
new_edges = {}
for (s_, t), conf in edges.items():
    if t.startswith("sqlrpc:"):
        hit = sql_by_name.get(t[7:])
        if hit:
            new_edges[(s_, hit)] = "cross"
            seam["ts->sql"] += 1
        continue
    if t.startswith("edgefn:"):
        # Prefer the module body of the edge function's index.ts: that is what
        # an inbound HTTP request runs.
        cands = edgefn_by_dir.get(t[7:], [])
        pick = ([i for i in cands if i.endswith("#<module>")] or cands or [None])[0]
        if pick:
            new_edges[(s_, pick)] = "cross"
            seam["sql->ts"] += 1
        continue
    new_edges[(s_, t)] = conf
edges = new_edges
print("database seam wired:", dict(seam), file=sys.stderr)

# --- resolve what the analyzers left dangling ------------------------------
# The TypeScript checker resolves a call to a DECLARATION, which for an
# interface member, an overload signature or an ambient type has no body and so
# no node. Most of those names are ours - `SeededRng.chance`, `decodeLogs` -
# and are defined a file away. Match them back by name before writing anything
# off as external.
ts_by_name = collections.defaultdict(list)
ts_by_short = collections.defaultdict(list)
for n in nodes.values():
    if n["lang"] != "ts" or n.get("kind") in ("unresolved", "platform"):
        continue
    ts_by_name[n["name"]].append(n["id"])
    ts_by_short[n["name"].split(".")[-1]].append(n["id"])

resolved = collections.Counter()
new_edges = {}
for (s_, t), conf in edges.items():
    if t in nodes or not t.startswith("ts:"):
        new_edges[(s_, t)] = conf
        continue
    where, _, what = t[3:].partition("#")
    hit, why = None, None
    same = [i for i in ts_by_name.get(what, []) if i[3:].startswith(where + "#")]
    if same:                                   # same file, same name
        hit, why = same[0], "high"
    elif len(ts_by_name.get(what, [])) == 1:   # one definition repo-wide
        hit, why = ts_by_name[what][0], "med"
    else:
        short = what.split(".")[-1]
        cand = ts_by_short.get(short, [])
        if len(cand) == 1:
            hit, why = cand[0], "low"
    if hit:
        resolved[why] += 1
        new_edges[(s_, hit)] = why if conf == "med" else conf
    else:
        # genuinely outside the repo: a library type's method (a supabase
        # `Channel`, a React member), so it belongs with the platform symbols.
        pid = "ext:ts#" + what
        if pid not in nodes:
            nodes[pid] = {"id": pid, "name": what, "file": PLATFORM_FILE["ts"],
                          "line": 0, "lang": "ts", "kind": "platform",
                          "exported": False, "loc": 0}
        new_edges[(s_, pid)] = "low"
        resolved["external"] += 1
edges = new_edges
print("TS declaration targets resolved:", dict(resolved), file=sys.stderr)


# 2. Swift -> C already emitted by the Swift analyzer as conf="cross"; they
#    resolve for free now that the C nodes are in the same map. Anything that
#    still dangles gets matched by bare name.
fixed = 0
new_edges = {}
for (s, t), conf in edges.items():
    if t not in nodes and t.startswith("c:"):
        name = t.split("#", 1)[-1]
        hits = c_by_name.get(name)
        if hits:
            new_edges[(s, hits[0])] = "cross"
            fixed += 1
            continue
    new_edges[(s, t)] = conf
edges = new_edges
print("swift->C re-anchored:", fixed, file=sys.stderr)

# 3. Whatever still dangles is a declaration we never saw a body for (an
#    ambient .d.ts member, a system symbol). Keep it visible as a stub rather
#    than dropping the edge, exactly as the analyzers keep `unresolved`.
stubs = 0
for (s, t) in list(edges.keys()):
    if t not in nodes:
        lang = t.split(":", 1)[0]
        if lang not in ("c", "ts", "swift", "rust"):
            lang = "ts"
        short = t.split("#", 1)[-1]
        nodes[t] = {"id": t, "name": short,
                    "file": PLATFORM_FILE.get(lang, PLATFORM_FILE["ts"]), "line": 0,
                    "lang": lang, "kind": "platform", "exported": False, "loc": 0}
        stubs += 1
    if s not in nodes:
        del edges[(s, t)]
print("declaration stubs:", stubs, file=sys.stderr)

# --- decorate --------------------------------------------------------------
fan_out = collections.Counter()
fan_in = collections.Counter()
for (s, t) in edges:
    fan_out[s] += 1
    fan_in[t] += 1

for n in nodes.values():
    n["cat"] = categorize(n)
    n["out"] = fan_out.get(n["id"], 0)
    n["in"] = fan_in.get(n["id"], 0)
    f = n.get("file", "")
    n["mod"] = "/".join(f.split("/")[:-1]) if "/" in f else f

out = {
    "nodes": sorted(nodes.values(), key=lambda n: n["id"]),
    "edges": [{"s": s, "t": t, "c": c} for (s, t), c in sorted(edges.items())],
}
json.dump(out, open(os.path.join(WORK, "graph.json"), "w"), separators=(",", ":"))

by_lang = collections.Counter(n["lang"] for n in out["nodes"])
by_cat = collections.Counter(n["cat"] for n in out["nodes"])
by_conf = collections.Counter(e["c"] for e in out["edges"])
print("TOTAL nodes", len(out["nodes"]), "edges", len(out["edges"]))
print("by lang", dict(by_lang))
print("by cat ", dict(by_cat))
print("by conf", dict(by_conf))
