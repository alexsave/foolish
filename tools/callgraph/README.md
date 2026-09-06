# Call Atlas

Every function in the repo, every function it calls, on one map.

The rendered page lives at [`docs/call-atlas.html`](../../docs/call-atlas.html) —
open it in any current browser. It is self-contained (~0.9 MB, no network) and
covers C, TypeScript, Swift, Rust and the Postgres schema in a single graph,
including the edges that cross between them.

## Regenerating

```sh
tools/callgraph/generate.sh                 # -> docs/call-atlas.html
tools/callgraph/generate.sh /tmp/out.html   # somewhere else
```

Needs `clang`, `node` (the repo's `node_modules` supplies `typescript`),
`cargo`, and `python3` with `numpy`. Swift needs no toolchain — see below.
The layout step is the slow part, about two minutes; the whole run is under
four. Set `CALLGRAPH_WORK=some/dir` to keep the intermediate JSON around.

## How each language is read

| language | method | resolution |
| --- | --- | --- |
| C | `clang -ast-dump=json`, every `.c` parsed under **three** build configurations and the readings unioned | clang's own, exact |
| TypeScript | the TypeScript compiler API with its checker | exact, except body-less declarations (below) |
| Swift | a hand-written lexical parser | **by name** — approximate |
| Rust | the `syn` crate's full AST | by name, plus named closures |
| SQL | the migrations replayed in filename order | by name, live definitions only |

Three of those want spelling out.

**C parses three times on purpose.** `c/wasm/wasm_oracle_mt.c` is wrapped in
`#ifdef FOOLISH_ORACLE_MT` from its first line to its last, and
`octogen_strategy.c` and `wasm_bots_api.c` each gate a block on the same define.
A third configuration, `OG_EXPLAIN_BUILD` + `CD_WASM_OVERLAY`, is the only one
in which `octogen_strategy.c` defines `wasm_og_explain_ptr` and its siblings —
the accessors the oracle UI reads across the wasm boundary. A
single-configuration parse silently drops all of it. Defines that *remove*
code (`GUARDS_VALIDATE_ONLY`, `DEAL_RNG_DISABLED`, `FOOLISH_SEEDED_BOTS_ONLY`)
are deliberately not set: the union should be the largest honest view of the
tree.

**Swift is lexical, not semantic.** No `swiftc`, SourceKit or IndexStore is
assumed to be present, so `analyze_swift.py` blanks comments and string
literals, tracks a brace-depth scope stack to attribute calls to the innermost
declaration, and resolves callees **by name**. Two methods called `apply` on
different types collapse into one candidate. Those edges are recorded as
*name-ambiguous* and the page can filter them out. If a Swift toolchain is ever
available in CI, an IndexStore-backed analyzer would replace this file wholesale
and nothing downstream would change — the JSON shape is the contract.

**SQL is a replay, not a snapshot.** Migrations are a timeline: `commit_game` is
dropped and recreated six times as its signature changes. `analyze_sql.py`
replays every `.sql` file in order — the baseline dumps (`e2e/schema.sql`,
`supabase/seed.sql`) first, then the migrations by timestamp — and keeps only
the definition of each routine still in force at the end, which is what the
database actually holds. Triggers and the `pg_cron` job are nodes too: they are
how a routine runs with no call site anywhere in the app. Views, RLS policies
and dynamic SQL (`EXECUTE format(...)`) are not covered.

## The cross-language edges

Three seams, wired in `merge.py` from the language graphs, not guessed:

- **TypeScript → C**, 213 edges. The web client calls `ex.wasm_apply_action()`
  on a `WebAssembly.Instance`; the checker resolves that to a member of the
  `EngineExports` interface, which has no body. The merge step matches those
  `wasm_*` names to the C functions of the same name in `c/wasm/`.
- **Swift → C**, 106 edges. `analyze_swift.py` knows the exported C symbol names
  from `c.json`, so `EngineC.apply` calling `fio_apply_awire` lands on the real
  definition in `c/ios/ios_api.c`.
- **TypeScript → SQL**, 7 edges. `supabase.rpc('commit_game', …)` names its
  callee in a string literal; the checker only ever sees postgrest-js, so
  `analyze_ts.mjs` records the literal and the merge step resolves it against
  the live routine of that name.
- **SQL → TypeScript**, 1 edge. The `bot-heartbeat` cron job runs
  `net.http_post` against `/functions/v1/bot-heartbeat`, which is the Deno edge
  function in `server/impls/supabase/functions/bot-heartbeat/`. That URL is the
  only trace of the call anywhere.
- **Rust → nothing.** `rustpoc/` is a standalone port with no FFI. That zero is
  a finding, not a gap.

## The tree view

The force map shows shape; it is bad at "what is actually in here". The **Tree**
view answers that instead, with two hierarchies:

- **Directories** — the repo drilled down through directory, file, function.
  A directory that holds exactly one child directory and nothing of its own is
  a corridor rather than a level, so `server/impls/supabase` collapses into one
  row instead of three clicks. Every directory takes the colour of whatever
  dominates it, so the tree reads the same way the map does.
- **Call tree** — callees (or callers, with the direction toggle) unrolled from
  one root, with the confidence of each edge on the row and recursion marked
  where a function appears above itself in the same branch.

The call tree's root and the selection are deliberately separate: clicking a row
reads that function in the inspector without yanking the tree out from under
you. `↻ start from …` in the bar re-roots when you actually want that.

## Colour

Language sets the hue; the group sets the tint. So `c/src` (rules kernel) and
`c/wasm` (bridge) are two shades of the same red rather than two unrelated
colours, and the same group reads the same way across all four languages.
Platform and standard-library symbols keep their caller's hue at a much lower
saturation, so the repo's own code is what carries colour.

Groups are assigned by path in `merge.py`'s `RULES` table — first match wins.
Adding a directory means adding a pattern there.

## Toggles, and why each is its own layout

Tests are a quarter of the tree and the platform buckets another sixth. Hiding
either by filtering one shared layout leaves craters where the nodes used to be,
so `layout.py` runs the whole force layout once per combination — `full`,
`noTest`, `noPlat`, `noBoth` — and the page swaps coordinate sets. That is what
the two-minute layout step is spending its time on.

## What it does not see

Dynamic dispatch, functions passed as values, SwiftUI's implicit `body`
re-entry, anything reached through reflection or a string name. The map is what
the source says, not what runs.

## Files

| file | role |
| --- | --- |
| `generate.sh` | the entry point; runs everything below in order |
| `analyze_c.py` · `analyze_ts.mjs` · `analyze_swift.py` · `analyze_rust/` · `analyze_sql.py` | one per language, each emitting the same `{nodes, edges}` JSON |
| `merge.py` | merges the four, wires cross-language edges, assigns groups |
| `layout.py` | force layout over the file graph, once per toggle combination |
| `pack.py` | columnar payload |
| `shell.html` · `app.js` · `build_html.py` | the page, its canvas map and tree views, and the inliner |
