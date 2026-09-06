#!/usr/bin/env python3
"""Pack the laid-out graph into a compact columnar payload for the page.

Artifacts and the docs/ HTML cannot fetch anything at runtime, so the whole
graph ships inline. Columnar arrays + interned strings take it from megabytes
of pretty JSON to something the browser parses in a blink.
"""
import json, os, sys, collections

WORK = os.path.abspath(sys.argv[1])
g = json.load(open(os.path.join(WORK, "graph_laid.json")))
nodes, edges = g["nodes"], g["edges"]

LANGS = ["c", "ts", "swift", "rust", "sql"]
CATS = ["rules", "bots", "anim", "wire", "bridge", "ui", "state", "network",
        "server", "imessage", "test", "tools", "platform"]
KINDS = ["function", "method", "arrow", "closure", "module-init", "trigger",
         "cron", "platform"]
CONFS = ["high", "med", "low", "cross"]
ABSENT = -32768

files = sorted({n["file"] for n in nodes})
fx = {f: i for i, f in enumerate(files)}
mods = sorted({("/".join(f.split("/")[:-1]) if "/" in f else f) for f in files})
mx = {m: i for i, m in enumerate(mods)}

# One stable node order for every layout: grouped by file, densest first.
nodes.sort(key=lambda n: (fx[n["file"]], -(n["in"] * 2 + n["out"]), n["name"]))
nx = {n["id"]: i for i, n in enumerate(nodes)}

payload = {
    "langs": LANGS, "cats": CATS, "kinds": KINDS, "confs": CONFS,
    "files": files,
    "mods": mods,
    "fileMod": [mx["/".join(f.split("/")[:-1]) if "/" in f else f] for f in files],
    "name": [n["name"] for n in nodes],
    "file": [fx[n["file"]] for n in nodes],
    "line": [n.get("line", 0) for n in nodes],
    "lang": [LANGS.index(n["lang"]) for n in nodes],
    "cat": [CATS.index(n["cat"]) for n in nodes],
    "kind": [KINDS.index(n.get("kind", "function")) for n in nodes],
    "exp": [1 if n.get("exported") else 0 for n in nodes],
    "loc": [n.get("loc", 0) for n in nodes],
    "es": [nx[e["s"]] for e in edges],
    "et": [nx[e["t"]] for e in edges],
    "ec": [CONFS.index(e["c"]) for e in edges],
    "layouts": {},
}

for key, lay in g["layouts"].items():
    xy = lay["xy"]
    payload["layouts"][key] = {
        # ABSENT marks a node (or file) this view does not show at all.
        "xy": [v for n in nodes for v in
               (tuple(round(c) for c in xy[n["id"]]) if n["id"] in xy else (ABSENT, ABSENT))],
        "fileXY": [v for f in files for v in
                   ((round(lay["files"][f]["x"]), round(lay["files"][f]["y"]),
                     round(lay["files"][f]["r"]))
                    if f in lay["files"] else (ABSENT, ABSENT, 0))],
        "nodes": lay["nodes"],
        "edges": lay["edges"],
    }

st = collections.defaultdict(collections.Counter)
for n in nodes:
    st["lang"][n["lang"]] += 1
    st["cat"][n["cat"]] += 1
for e in edges:
    st["conf"][e["c"]] += 1
    st["pair"]["%s->%s" % (nodes[nx[e["s"]]]["lang"], nodes[nx[e["t"]]]["lang"])] += 1
payload["stats"] = {k: dict(v) for k, v in st.items()}
payload["stats"]["files"] = len([f for f in files if not f.startswith("(")])

out = os.path.join(WORK, "payload.json")
json.dump(payload, open(out, "w"), separators=(",", ":"))
print("packed %d nodes / %d edges / %d files -> %.2f MB"
      % (len(nodes), len(edges), len(files), os.path.getsize(out) / 1e6))
for k, v in payload["layouts"].items():
    print("  %-7s %5d nodes  %6d edges" % (k, v["nodes"], v["edges"]))
print("  lang", payload["stats"]["lang"])
print("  cross", {k: v for k, v in payload["stats"]["pair"].items()
                  if k.split("->")[0] != k.split("->")[1]})
