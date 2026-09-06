#!/usr/bin/env python3
"""SQL analyzer: the Postgres side of the app, as the migrations leave it.

Migrations are a timeline, not a snapshot - `commit_game` is created, dropped
and recreated half a dozen times as its signature changes. So the files are
replayed in order and only the LIVE definition of each routine survives, which
is what the database actually ends up holding.

Captured:
  * `CREATE [OR REPLACE] FUNCTION name(...)` bodies (plpgsql and sql)
  * `CREATE TRIGGER t ... EXECUTE FUNCTION f()` - how a function gets called
    with no call site anywhere in the app
  * `SELECT cron.schedule('job', 'sched', $$ ... $$)` - likewise, on a timer
  * calls between them, by name
  * `net.http_post(url := '.../functions/v1/<name>')`, which is a cron job
    reaching back into a TypeScript edge function - a real edge across the
    language boundary, marked so merge.py can anchor it

Not captured: views, RLS policies, and anything a routine reaches through
dynamic SQL (`EXECUTE format(...)`).
"""
import json, os, re, subprocess, sys
from collections import OrderedDict

ROOT = os.path.abspath(sys.argv[1])
OUT = sys.argv[2]

_sql = [f for f in subprocess.check_output(["git", "ls-files"], cwd=ROOT)
        .decode().split("\n") if f.endswith(".sql")]
# Baseline dumps first (e2e/schema.sql, supabase/seed.sql - snapshots of a live
# database, and the only place the Supabase-internal routines appear), then the
# migrations in timestamp order. The migrations are the source of truth, so
# where both define a routine the migration's definition is the one that wins.
files = ([f for f in sorted(_sql) if "/migrations/" not in f] +
         sorted(f for f in _sql if "/migrations/" in f))

# A dollar-quoted body ($$ ... $$ or $tag$ ... $tag$) can hold anything,
# semicolons included, so bodies are matched as whole units first.
CREATE_FN = re.compile(
    r"CREATE\s+(?:OR\s+REPLACE\s+)?FUNCTION\s+(?:(\w+)\.)?(\w+)\s*\(", re.I)
DROP_FN = re.compile(r"DROP\s+FUNCTION\s+(?:IF\s+EXISTS\s+)?(?:(\w+)\.)?(\w+)\s*\(?", re.I)
CREATE_TRG = re.compile(
    r"CREATE\s+TRIGGER\s+(\w+)\b(.*?)EXECUTE\s+(?:FUNCTION|PROCEDURE)\s+(?:(\w+)\.)?(\w+)\s*\(",
    re.I | re.S)
DROP_TRG = re.compile(r"DROP\s+TRIGGER\s+(?:IF\s+EXISTS\s+)?(\w+)", re.I)
CRON = re.compile(r"cron\.schedule\s*\(\s*'([^']+)'", re.I)
DOLLAR = re.compile(r"\$(\w*)\$")
EDGE_FN = re.compile(r"/functions/v1/([A-Za-z0-9_-]+)")
CALL = re.compile(r"\b(\w+)\s*\(")

# Reserved words and the built-ins that would otherwise read as calls.
SQL_NOISE = set("""
select insert update delete from where values into returning if elsif else end
begin case when then loop for while return returns declare exception raise
perform execute using as on and or not in exists is null true false coalesce
array row jsonb_build_object jsonb_agg json_build_object to_jsonb count sum min
max avg now current_timestamp extract cast concat format left right substring
length lower upper trim replace nullif greatest least generate_series unnest
any all distinct order group by limit offset language security definer invoker
set search_path stable volatile immutable strict cascade restrict function
trigger procedure exists notice warning check constraint references default
""".split())


def bodies(src):
    """Yield (start, end) spans of every dollar-quoted block."""
    spans, i = [], 0
    while True:
        m = DOLLAR.search(src, i)
        if not m:
            return spans
        close = src.find(m.group(0), m.end())
        if close < 0:
            return spans
        spans.append((m.end(), close))
        i = close + len(m.group(0))


live = OrderedDict()   # name -> node dict (the definition currently in force)
order = []             # (file, kind, name, ...) statements, in migration order

for f in files:
    src = open(os.path.join(ROOT, f), encoding="utf-8", errors="replace").read()
    spans = bodies(src)

    def line_at(pos):
        return src.count("\n", 0, pos) + 1

    def body_after(pos):
        for a, b in spans:
            if a >= pos:
                return src[a:b], line_at(a)
        return "", 0

    events = []
    for m in DROP_FN.finditer(src):
        events.append((m.start(), "dropfn", m.group(2).lower(), None))
    for m in CREATE_FN.finditer(src):
        body, _ = body_after(m.end())
        events.append((m.start(), "fn", m.group(2).lower(), body))
    for m in DROP_TRG.finditer(src):
        events.append((m.start(), "droptrg", m.group(1).lower(), None))
    for m in CREATE_TRG.finditer(src):
        events.append((m.start(), "trg", m.group(1).lower(), m.group(4).lower()))
    for m in CRON.finditer(src):
        body, _ = body_after(m.end())
        events.append((m.start(), "cron", m.group(1).lower(), body))
    events.sort()

    for pos, kind, name, extra in events:
        # A trigger and the function it fires routinely share a name
        # (enforce_username_not_bot), so they are keyed apart.
        ns = "trg" if kind in ("trg", "droptrg") else "fn"
        if kind in ("dropfn", "droptrg"):
            live.pop((ns, name), None)
            continue
        nid = "sql:%s#%s%s" % (f, "trigger " if ns == "trg" else "", name)
        live[(ns, name)] = dict(id=nid, name=name, file=f, line=line_at(pos), lang="sql",
                          kind={"fn": "function", "trg": "trigger", "cron": "cron"}[kind],
                          exported=(kind != "fn"), loc=(extra or "").count("\n") + 1
                          if kind != "trg" else 1,
                          body=extra if kind != "trg" else "", target=extra if kind == "trg" else None)

nodes = {}
edges = {}
for key, n in live.items():
    node = {k: v for k, v in n.items() if k not in ("body", "target")}
    nodes[node["id"]] = node

by_name = {name: n["id"] for (ns, name), n in live.items() if ns == "fn"}

for (ns, name), n in live.items():
    src_id = n["id"]
    if n["kind"] == "trigger":
        t = by_name.get(n["target"])
        if t:
            edges[(src_id, t)] = "high"
        continue
    body = n["body"] or ""
    for m in CALL.finditer(body):
        callee = m.group(1).lower()
        if callee in SQL_NOISE or callee == name:
            continue
        t = by_name.get(callee)
        if t:
            edges[(src_id, t)] = "high"
    # A cron job or trigger firing an HTTP call into a Deno edge function is a
    # genuine call across the language boundary; merge.py anchors it on the file.
    for m in EDGE_FN.finditer(body):
        edges[(src_id, "edgefn:" + m.group(1))] = "cross"

out = dict(nodes=list(nodes.values()),
           edges=[dict(s=s, t=t, conf=c) for (s, t), c in edges.items()])
json.dump(out, open(OUT, "w"))
sys.stderr.write(
    "sql: %d live routines (%d functions, %d triggers, %d cron), %d edges over %d files\n"
    % (len(nodes), sum(1 for n in nodes.values() if n["kind"] == "function"),
       sum(1 for n in nodes.values() if n["kind"] == "trigger"),
       sum(1 for n in nodes.values() if n["kind"] == "cron"),
       len(out["edges"]), len(files)))
