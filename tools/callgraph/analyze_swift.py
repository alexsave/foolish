#!/usr/bin/env python3
"""Swift analyzer.

No Swift toolchain (no swiftc / SourceKit / IndexStore) is available in this
container, so this is a hand-written lexical parser, NOT a semantic one:

  * comments (nested /* */ and //) and string literals (including multiline
    \"\"\" and \\(interpolation)) are blanked out first, preserving offsets;
  * a brace-depth stack tracks type scopes (class/struct/enum/extension/
    protocol/actor) and function bodies, so callers are attributed to the
    innermost enclosing declaration;
  * declarations captured: func, init, deinit, subscript, and computed
    properties `var x: T { ... }` (SwiftUI `var body` included);
  * call sites: `ident(` and `.ident(` inside a body, plus trailing-closure
    style `Ident {` only for known declared type names.

Callee resolution is by NAME, so overloads across types collapse and edges to
same-named methods on different types are approximate. Confidence is recorded
per edge: high = unique name in repo or same-file, med = ambiguous name.
"""
import json, os, re, subprocess, sys
from collections import defaultdict

ROOT = os.path.abspath(sys.argv[1])
OUT = sys.argv[2]
C_JSON = sys.argv[3] if len(sys.argv) > 3 else None

files = [f for f in subprocess.check_output(["git", "ls-files"], cwd=ROOT).decode().split("\n")
         if f.endswith(".swift")]


def blank(src):
    """Replace comments and string literals with spaces, keeping offsets."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                out[i] = " "
                i += 1
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            depth = 1
            out[i] = out[i + 1] = " "
            i += 2
            while i < n and depth:
                if src[i] == "/" and i + 1 < n and src[i + 1] == "*":
                    depth += 1
                    out[i] = out[i + 1] = " "
                    i += 2
                    continue
                if src[i] == "*" and i + 1 < n and src[i + 1] == "/":
                    depth -= 1
                    out[i] = out[i + 1] = " "
                    i += 2
                    continue
                if src[i] != "\n":
                    out[i] = " "
                i += 1
        elif c == '"':
            if src.startswith('"""', i):
                out[i] = out[i + 1] = out[i + 2] = " "
                i += 3
                while i < n and not src.startswith('"""', i):
                    if src[i] != "\n":
                        out[i] = " "
                    i += 1
                for k in range(min(3, n - i)):
                    out[i + k] = " "
                i += 3
            else:
                out[i] = " "
                i += 1
                while i < n and src[i] != '"':
                    if src[i] == "\\" and i + 1 < n:
                        out[i] = out[i + 1] = " "
                        i += 2
                        continue
                    if src[i] != "\n":
                        out[i] = " "
                    i += 1
                if i < n:
                    out[i] = " "
                    i += 1
        else:
            i += 1
    return "".join(out)


DECL_RE = re.compile(
    r"\b(?P<kw>func|init|deinit|subscript)\b\s*(?P<gen><[^>\n]*>)?\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)?")
TYPE_RE = re.compile(r"\b(?P<kw>class|struct|enum|extension|protocol|actor)\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)")
VAR_RE = re.compile(r"\b(?:var|let)\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*:\s*[^={\n]+\{")
OPFUNC_RE = re.compile(r"\bfunc\s*(?P<op>[-+*/%<>=!&|^~]+)\s*(?:<[^>\n]*>)?\s*\(")
CALL_RE = re.compile(r"(?P<dot>\.)?\b(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\(")

SWIFT_BUILTIN = set("""
print debugPrint assert assertionFailure precondition preconditionFailure fatalError
String Int Int8 Int16 Int32 Int64 UInt UInt8 UInt16 UInt32 UInt64 Double Float Bool
Array Set Dictionary Data Date UUID URL URLRequest Optional Result Range ClosedRange
min max abs zip stride repeatElement type withUnsafeBytes withUnsafeMutableBytes
map filter reduce compactMap flatMap forEach sorted sort append remove removeAll insert
contains first last count joined split components prefix suffix dropFirst dropLast
enumerated reversed isEmpty hash encode decode init deinit require XCTAssert
XCTAssertEqual XCTAssertTrue XCTAssertFalse XCTAssertNil XCTAssertNotNil XCTAssertThrowsError
XCTAssertNoThrow XCTAssertGreaterThan XCTAssertLessThan XCTFail XCTAssertEqualWithAccuracy
DispatchQueue Task MainActor withCheckedContinuation withTaskGroup Timer NotificationCenter
Text VStack HStack ZStack Button Image Color Spacer Group ForEach List NavigationStack
ScrollView GeometryReader Rectangle Circle RoundedRectangle Path Animation withAnimation
State Binding Published ObservedObject StateObject EnvironmentObject Font Angle
CGPoint CGSize CGRect CGFloat UIColor UIImage UIView UIScreen UIApplication
JSONEncoder JSONDecoder Bundle FileManager UserDefaults Locale Calendar TimeInterval
""".split())


def module_of(path):
    p = path.split("/")
    if p[0] == "sdk":
        return "sdk/swift"
    if len(p) >= 2:
        return p[1] if p[0] == "ios" else p[0]
    return p[0]


nodes = {}
raw_calls = []  # (src_id, callee_name, is_member)
by_name = defaultdict(list)

for f in files:
    src = open(os.path.join(ROOT, f), encoding="utf-8", errors="replace").read()
    code = blank(src)
    lines_at = [0]
    for m in re.finditer("\n", code):
        lines_at.append(m.start() + 1)

    def lineno(pos):
        lo, hi = 0, len(lines_at) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if lines_at[mid] <= pos:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    # Collect declaration start positions with their kind/name
    decls = []
    for m in TYPE_RE.finditer(code):
        decls.append((m.start(), "type", m.group("name"), m.end()))
    for m in DECL_RE.finditer(code):
        kw = m.group("kw")
        nm = m.group("name")
        if kw in ("init", "deinit", "subscript"):
            nm = kw
        if not nm:
            continue
        decls.append((m.start(), "func", nm, m.end()))
    for m in OPFUNC_RE.finditer(code):
        decls.append((m.start(), "func", "operator" + m.group("op"), m.end()))
    for m in VAR_RE.finditer(code):
        decls.append((m.start(), "func", m.group("name"), m.end() - 1))
    decls.sort()
    dptr = 0

    # Walk braces, maintaining a scope stack.
    scope = []       # list of (kind, name, brace_depth_at_open)
    depth = 0
    pending = None   # decl awaiting its opening brace
    i, n = 0, len(code)
    cur_fn = None    # (id) innermost function node
    fn_stack = []

    while i < n:
        # attach any decl starting at/behind i
        while dptr < len(decls) and decls[dptr][0] <= i:
            pos, kind, name, end = decls[dptr]
            if pos >= i - 0:
                pending = (kind, name, pos)
            dptr += 1
        c = code[i]
        if c == "{":
            if pending:
                kind, name, pos = pending
                if kind == "type":
                    scope.append(("type", name, depth))
                else:
                    owner = ".".join(s[1] for s in scope if s[0] == "type")
                    qual = f"{owner}.{name}" if owner else name
                    nid = f"swift:{f}#{qual}"
                    if nid not in nodes:
                        nodes[nid] = dict(id=nid, name=qual, short=name, file=f, line=lineno(pos),
                                          lang="swift", kind="method" if owner else "function",
                                          exported=True, loc=0, module=module_of(f))
                        by_name[name].append(nid)
                        if owner:
                            by_name[qual].append(nid)
                    scope.append(("func", name, depth))
                    fn_stack.append((nid, depth, pos))
                    cur_fn = nid
                pending = None
            else:
                scope.append(("blk", None, depth))
            depth += 1
        elif c == "}":
            depth -= 1
            while scope and scope[-1][2] >= depth:
                s = scope.pop()
                if s[0] == "func" and fn_stack and fn_stack[-1][1] == depth:
                    nid, d, pos = fn_stack.pop()
                    nodes[nid]["loc"] = max(1, lineno(i) - nodes[nid]["line"] + 1)
                    cur_fn = fn_stack[-1][0] if fn_stack else None
                if s[2] < depth:
                    break
        elif c == ";" and pending:
            pending = None
        elif c == "\n" and pending and code[max(0, i - 200):i].count("(") == 0:
            pass
        i += 1

    # Calls: re-scan and attribute to the function whose [start,end] span contains it
    spans = sorted(((v["line"], v["line"] + max(0, v["loc"]) , k) for k, v in nodes.items() if v["file"] == f))
    for m in CALL_RE.finditer(code):
        name = m.group("name")
        if name in ("if", "for", "while", "switch", "guard", "return", "catch", "func",
                    "init", "throw", "await", "try", "in", "where", "case", "defer", "repeat"):
            continue
        ln = lineno(m.start())
        best = None
        for s, e, k in spans:
            if s <= ln <= e:
                if best is None or s >= best[0]:
                    best = (s, k)
        if not best:
            continue
        raw_calls.append((best[1], name, bool(m.group("dot")), f))

# Resolve
edges = {}
unresolved = {}
c_exports = set()
c_home = {}
if C_JSON and os.path.exists(C_JSON):
    cj = json.load(open(C_JSON))
    for nd in cj["nodes"]:
        if nd["lang"] == "c" and nd.get("exported"):
            c_exports.add(nd["name"])
            c_home[nd["name"]] = nd["id"]

for src, name, member, f in raw_calls:
    if name in SWIFT_BUILTIN:
        continue
    cands = by_name.get(name, [])
    same_file = [c for c in cands if nodes[c]["file"] == f]
    same_mod = [c for c in cands if nodes[c]["module"] == module_of(f)]
    if same_file:
        tgt, conf = same_file[0], "high"
    elif len(cands) == 1:
        tgt, conf = cands[0], "high"
    elif same_mod:
        tgt, conf = same_mod[0], "med"
    elif cands:
        tgt, conf = cands[0], "med"
    elif name in c_exports:
        tgt, conf = c_home[name], "cross"
    else:
        if not re.match(r"^[a-z]", name):
            continue  # Type(...) construction of an undeclared/stdlib type
        tgt = f"ext:{name}"
        unresolved[tgt] = dict(id=tgt, name=name, file="(unresolved)", line=0,
                               lang="unresolved", kind="unresolved", exported=False, loc=0)
        conf = "low"
    if tgt == src:
        continue
    key = (src, tgt)
    rank = {"high": 4, "cross": 4, "med": 2, "low": 1}
    if key not in edges or rank[conf] > rank[edges[key]["conf"]]:
        edges[key] = dict(s=src, t=tgt, conf=conf)

allnodes = list(nodes.values()) + list(unresolved.values())
json.dump(dict(nodes=allnodes, edges=list(edges.values())), open(OUT, "w"))
sys.stderr.write(f"swift: {len(nodes)} defs, {len(unresolved)} unresolved, {len(edges)} edges over {len(files)} files\n")
