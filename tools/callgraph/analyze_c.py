#!/usr/bin/env python3
"""C analyzer: clang -ast-dump=json per translation unit.

Real parsing (clang 18 AST), not regex. For each .c file we dump the AST and
walk FunctionDecls with bodies (definitions) plus every DeclRefExpr/CallExpr
inside them. Callees resolve by clang's own name resolution.
"""
import json, os, subprocess, sys, concurrent.futures, re

ROOT = os.path.abspath(sys.argv[1])
OUT = sys.argv[2]

srcs = subprocess.check_output(["git", "ls-files"], cwd=ROOT).decode().split("\n")
csrc = [f for f in srcs if f.startswith("c/") and f.endswith(".c")]

INCLUDES = ["-Ic/src", "-Ic/wasm/include", "-Ic/ios/include", "-Ic/wasm", "-Ic/tests", "-Ic/tools"]

# Whole translation units and large blocks sit behind build-config #ifdefs -
# c/wasm/wasm_oracle_mt.c is FOOLISH_ORACLE_MT from its first line to its last,
# and octogen_strategy.c / wasm_bots_api.c each gate a block on it. Parsing one
# configuration silently drops that code, so every TU is parsed under each
# configuration below and the definitions are unioned. Defines that REMOVE code
# (GUARDS_VALIDATE_ONLY, DEAL_RNG_DISABLED, FOOLISH_SEEDED_BOTS_ONLY) are
# deliberately absent: the union should be the largest honest view of the tree.
CONFIGS = [
    ("default", ["-DCD_LEAFBOOK", "-DACCELERATE_NEW_LAPACK"]),
    ("oracle-mt", ["-DCD_LEAFBOOK", "-DACCELERATE_NEW_LAPACK",
                   "-DFOOLISH_ORACLE_MT", "-DFOOLISH_ORACLE_BUILD", "-DLEGAL_STATS"]),
]

nodes = {}
edges = {}
file_of_def = {}   # function name -> defining file (for cross-TU linking)
errors = []


def dump(job):
    path, cfg, defs = job
    cmd = ["clang", "-fsyntax-only", "-ferror-limit=0", "-w",
           "-Xclang", "-ast-dump=json"] + INCLUDES + defs + [path]
    try:
        p = subprocess.run(cmd, cwd=ROOT, capture_output=True, timeout=300)
        if not p.stdout:
            return path, None, "%s: %s" % (cfg, p.stderr.decode()[:180] or "no stdout")
        return path, json.loads(p.stdout.decode("utf-8", "replace")), None
    except Exception as e:  # noqa
        return path, None, "%s: %s" % (cfg, str(e)[:180])


def walk(n, cb):
    stack = [n]
    while stack:
        cur = stack.pop()
        if not isinstance(cur, dict):
            continue
        cb(cur)
        inner = cur.get("inner")
        if inner:
            stack.extend(inner)


def main():
    jobs = [(p, cfg, defs) for cfg, defs in CONFIGS for p in csrc]
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        for path, ast, err in ex.map(dump, jobs):
            if err:
                errors.append((path, err))
            else:
                results.append((path, ast))

    # First pass: every function definition in each TU, keyed by the file the
    # body actually lives in (clang tracks includes via loc.file stickiness).
    defs = []  # (tu, name, file, line, static, loc_lines)
    for tu, ast in results:
        cur_file = tu
        for top in ast.get("inner", []):
            f = (top.get("loc") or {}).get("file")
            if f:
                cur_file = os.path.relpath(os.path.join(ROOT, f), ROOT) if not f.startswith("/") else os.path.relpath(f, ROOT)
            if top.get("kind") != "FunctionDecl":
                continue
            if not any(i.get("kind") == "CompoundStmt" for i in top.get("inner", []) or []):
                continue  # prototype only
            name = top.get("name")
            if not name:
                continue
            loc = top.get("loc") or {}
            line = loc.get("line") or ((top.get("range") or {}).get("begin") or {}).get("line") or 0
            rng = top.get("range") or {}
            beg = (rng.get("begin") or {}).get("line") or line
            end = (rng.get("end") or {}).get("line") or line
            fl = cur_file
            if fl.startswith(ROOT):
                fl = os.path.relpath(fl, ROOT)
            static = top.get("storageClass") == "static"
            defs.append((tu, name, fl, line, static, max(1, (end or 0) - (beg or 0) + 1), top))

    # Build node ids. Non-static functions are global: one node per name,
    # attributed to the file that defines them. Static functions are per-file.
    global_home = {}
    for tu, name, fl, line, static, loc, _ in defs:
        if not static and name not in global_home:
            global_home[name] = fl

    def nid(name, fl, static):
        if static:
            return f"c:{fl}#{name}"
        return f"c:{global_home.get(name, fl)}#{name}"

    seen_ids = set()
    for tu, name, fl, line, static, loc, top in defs:
        i = nid(name, fl, static)
        if i in seen_ids:
            continue
        seen_ids.add(i)
        nodes[i] = dict(id=i, name=name, file=global_home.get(name, fl) if not static else fl,
                        line=line, lang="c", kind="function", exported=not static, loc=loc)

    # Second pass: calls. Within a TU, resolve callee by (name, static-ness in
    # that TU). clang gives us the referenced decl inline.
    for tu, ast in results:
        # local static defs visible in this TU
        tu_static = {}
        tu_files = {}
        cur_file = tu
        for top in ast.get("inner", []):
            f = (top.get("loc") or {}).get("file")
            if f:
                cur_file = os.path.relpath(f, ROOT) if f.startswith("/") else f
            if top.get("kind") == "FunctionDecl" and top.get("name"):
                if top.get("storageClass") == "static":
                    tu_static[top["name"]] = cur_file
                tu_files[top["name"]] = cur_file

        cur_file = tu
        for top in ast.get("inner", []):
            f = (top.get("loc") or {}).get("file")
            if f:
                cur_file = os.path.relpath(f, ROOT) if f.startswith("/") else f
            if top.get("kind") != "FunctionDecl" or not top.get("name"):
                continue
            if not any(i.get("kind") == "CompoundStmt" for i in top.get("inner", []) or []):
                continue
            caller_static = top.get("storageClass") == "static"
            src = nid(top["name"], cur_file, caller_static)
            if src not in nodes:
                continue

            def cb(n, src=src, tu_static=tu_static):
                k = n.get("kind")
                if k not in ("DeclRefExpr", "MemberExpr"):
                    return
                d = n.get("referencedDecl") or {}
                if d.get("kind") != "FunctionDecl":
                    return
                cname = d.get("name")
                if not cname:
                    return
                if cname in tu_static:
                    tgt = f"c:{tu_static[cname]}#{cname}"
                elif cname in global_home:
                    tgt = f"c:{global_home[cname]}#{cname}"
                else:
                    tgt = f"ext:{cname}"
                    if tgt not in nodes:
                        nodes[tgt] = dict(id=tgt, name=cname, file="(unresolved)", line=0,
                                          lang="unresolved", kind="unresolved", exported=False, loc=0)
                if tgt not in nodes:
                    return
                if tgt == src:
                    return
                edges[(src, tgt)] = dict(s=src, t=tgt, conf="high")

            for i in top.get("inner", []) or []:
                walk(i, cb)

    out = dict(nodes=list(nodes.values()), edges=list(edges.values()))
    with open(OUT, "w") as fh:
        json.dump(out, fh)
    sys.stderr.write(f"c: {len(nodes)} nodes, {len(edges)} edges, {len(results)}/{len(jobs)} TU-configs parsed, {len(errors)} failed\n")
    for p, e in errors[:10]:
        sys.stderr.write(f"  FAIL {p}: {e}\n")


main()
