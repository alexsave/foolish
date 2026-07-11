# rules.wasm / guards.wasm memory shrink — execution handoff

**Audience.** This doc is written for an executor who has NOT read the bots
round. Everything you need is either in this file, in the two companion docs
(`docs/BOTS_WASM_MEMORY_PLAN.md`, `docs/WASM_L1_BUDGET.md`), or at an exact
`file:symbol` reference below. Follow the method; do not improvise caps.

**Mission.** Transfer the wins from the bots.wasm shrink round (PRs #55, #58,
#61) to the OTHER two shipped modules. bots.wasm is **done for now**
(18 → 14 pages, code −29%; do not touch it). The remaining candidates:

| module | today (measured 2026-07-11) | realistic target | verdict |
|---|---|---|---|
| `rules.wasm` | **5 pages** (327,680 B), 35,489 B code, unpinned | **4 pages** solid, **3 pages** stretch | ~90 KiB of dead aliasable scratch — the main prize |
| `guards.wasm` | **1 page**, pinned (`max=1`), 10,717 B code | — | **DONE. Do not touch.** 1 page is the hard floor (wasm page granularity); the linker pin makes any regression a build failure |
| `bots.wasm` | 14 pages + runtime TT | — | done this round; leave alone |

**Non-goal (same as always): zero behavior change.** Every byte the game
logic reads/writes must be identical before/after. When in doubt, measure;
when still in doubt, skip.

---

## 0. Method (identical to the bots round — read §0 of BOTS_WASM_MEMORY_PLAN.md)

1. **Measure the real requirement** (`cnitro/tests/l1_measure.c`, the stack
   canary, or analytic worst cases stated inline).
2. **Cap or alias at a stated margin**, documented in a Makefile comment next
   to the flag.
3. **Overflow must be a CLEAN error** — trap, clamp, or link failure. Never
   corruption.
4. **Ship only behind green gates** (§6).

### How to measure module pages (no external tools needed)

```sh
cd cnitro && make build/rules.wasm build/guards.wasm
node -e "
const fs=require('fs');
const buf=fs.readFileSync('build/rules.wasm');
let p=8; function leb(){let r=0,s=0,b;do{b=buf[p++];r|=(b&0x7f)<<s;s+=7;}while(b&0x80);return r>>>0;}
while(p<buf.length){const id=buf[p++],len=leb(),end=p+len;
 if(id===5){leb();const f=leb(),min=leb();console.log('min pages',min, f&1?('max '+leb()):'unpinned');break;} p=end;}"
```

### How to get the exact per-symbol map (this is how the table below was made)

The rules module builds in one clang invocation (no .o files), so compile the
same sources to objects with the same flags and read the BSS/data symbols:

```sh
cd cnitro && mkdir -p /tmp/rulesobj
for f in src/game.c src/deal_rng.c src/legal.c src/replay.c src/view.c \
         src/awire.c src/evwire.c wasm/wasm_api.c; do
  clang --target=wasm32 -Oz -nostdlib -ffreestanding -mbulk-memory \
    -isystem wasm/include -Isrc -D_Thread_local= -DMAX_LOG_PAIRS=64 \
    -DMAX_MOVE_CARDS=28 -DMAX_BATTLES=64 -DWORLD_LOG_CAP=40 \
    -DREPLAY_REC_CAP=4096 -DWASM_REPLAY_IO_CAP=32768 -DMAX_SNAPS=16 \
    -DMAX_LEGAL_MOVES=1024 -DMAX_LOGS=128 -DWASM_IO_CAP=24576 \
    -c $f -o /tmp/rulesobj/$(basename $f .c).o
done
llvm-nm --print-size --defined-only /tmp/rulesobj/*.o | grep -iE ' [bd] ' \
  | awk '{print $2, $4}' | sort -r | head -20
```

(If the Makefile's rules flags change, re-derive them from
`WASM_RULES_FLAGS` at `cnitro/Makefile:310` instead of copying the above.)

Regenerate the visual layout any time: `bash cnitro/tools/wasm_anatomy/generate.sh`
→ `docs/wasm-anatomy.html`, "Memory layout" tab (covers all three modules).

---

## 1. The measured map today — rules.wasm (post-#61, MAX_SNAPS=16)

| symbol | bytes | KiB | lives in | role |
|---|---:|---:|---|---|
| shadow stack | 65,536 | 64.0 | `Makefile` rules block (`-Wl,-z,stack-size=65536`, comment at `Makefile:309-314`) | worst measured path ~45 KiB (cover enumeration), 1.4× |
| `g_moves` | 59,396 | 58.0 | `wasm/wasm_api.c` (`LegalMoves`, `MAX_LEGAL_MOVES=1024` × 58 B) | legal-move menu |
| `g_rec` | 49,152 | 48.0 | `src/replay.c` (`REPLAY_REC_CAP=4096` × 12 B) | replay rANS choice log |
| `g_replay_io` | 32,768 | 32.0 | `wasm/wasm_api.c` (`WASM_REPLAY_IO_CAP`) | replay blob in/out |
| `g_io` | 24,576 | 24.0 | `wasm/wasm_api.c` (`WASM_IO_CAP=24576`) | marshal/export buffer |
| `g_comb` | 21,632 | 21.1 | `src/replay.c` | 52×52 binomial table — **wire-frozen, do not touch** |
| `g_snaps` | 18,560 | 18.1 | `wasm/wasm_api.c` (`MAX_SNAPS=16` × SnapSlot) | animation snapshots |
| `g_game` | 18,056 | 17.6 | `wasm/wasm_api.c` | **the resident Game — NEVER overlay** |
| `g_bn` | 10,756 | 10.5 | `src/replay.c` | replay bignum |
| `g_weights` + `g_opts` | 14,336 | 14.0 | `src/replay.c` | codec tables — **wire-frozen** |
| misc (<1 KiB each) | ~1,100 | ~1.1 | — | rng/seeds/snap tags/in-card buffers |
| **total** | **~315,900** | **~308.5** | | → **5 pages** (327,680 B), headroom 11,780 B |

The rules module **never calls `memory.grow`** (no allocator; all-static —
`docs/WASM_L1_BUDGET.md` "Result" table). This is what makes R0 (pinning) safe.

---

## 2. Transfer matrix — every candidate from the bots round, applied here

| bots-round result | verdict for rules.wasm | verdict for guards.wasm |
|---|---|---|
| **M8/M9 overlay** (alias non-concurrent scratch; the big win, −162 KiB) | ✅ **TRANSFERS — this is R1 below.** The replay family (g_rec+g_bn+g_replay_io = 92,676 B) is provably never live at the same time as the action family (g_moves+g_snaps+g_io = 102,532 B) | ❌ N/A — guards links neither `replay.c` nor `wasm_api.c` (`Makefile:392`: game/view/awire/wasm_guards_api only) |
| **M1/M7a stack shrink + BSS hoist** (256→64 KiB, and it was *faster*) | ⚠️ **PARTIAL — R4 stretch.** Rules stack is already 64 KiB at 1.4× its measured ~45 KiB worst. Going to 32 KiB requires first shrinking the cover-enumeration frame (the M7a trick), and the accounting must net out (§R4) | ❌ done — 16 KiB at >5× margin |
| **M4 MAX_SNAPS 24→16** | ✅ already landed in #61 (shared flag) — the map above includes it | ✅ already at MAX_SNAPS=1 |
| **M2 cap truncation** (MAX_LEGAL_MOVES) — **NEGATIVE for bots** (real menus >16k) | ⚠️ different situation: rules' menu is *test-surface only* (see R3 caution before believing this) | N/A (guards has no menu buffer) |
| **M2-stream** (streaming enumeration instead of materialized index arrays; −112 KiB frame, +11% speed) | ✅ the *technique* is the enabler for R4's cover-enum frame reduction | N/A |
| **M3 MAX_LOGS cut — NEGATIVE for bots** (session log peaks ~1,030) | ✅ already optimal: rules ships MAX_LOGS=128 by design (it never imports session logs — `Makefile:304-308`) | N/A (no log import) |
| **M5 packed move slots — NEGATIVE** (union-capped, latency risk) | ❌ do not retry. Packing `LegalMove` (58 B) would change the exported per-move wire layout the TS bridge parses (`engine.ts` MOVE_WIRE_MAX) → wire break for ~30 KiB. Rejected | N/A |
| **M6 module split — BLOCKED for bots** (W1) | ❌ equivalent block: rules.wasm must keep the replay codec — it IS the engine that serves replay for human-only games. No split | N/A |
| **Ship-set trim** (−29% code) | ❌ nothing to trim — rules carries no strategies; the export allow-list (`WASM_API_EXPORTS`) already did the dead-code pass | ❌ same |
| **TT 2WAY/PACK8** (#55/#58 — the actual octogen speedup) | ❌ N/A — no solver in rules/guards. Do not go looking for one | ❌ N/A |
| **Linker pin** (guards precedent: `--initial-memory=--max-memory`, link fails on regression) | ✅ **TRANSFERS — R0 below.** Rules never grows, so it can be pinned exactly like guards (`Makefile:358`) | ✅ already the precedent |

**Losses to NOT retry, with reasons** (so you don't burn a day rediscovering):
- Do not shrink `REPLAY_REC_CAP`/`REPLAY_BN_CAP`/`g_comb`/`g_weights`/`g_opts` —
  wire-frozen (W4) and already at measured 6–52× margins.
- Do not overlay anything with `g_game` — it is resident state that persists
  across calls (the whole point of the resident fast path).
- Do not reorder `Game` fields (W5) — snap slots and serializers use
  `offsetof(Game, ...)`.
- Do not touch guards.wasm. 1 page is the floor; it is pinned; any "improvement"
  is pure risk.

---

## 3. R0 — pin rules.wasm linear memory (zero-risk tripwire; do this FIRST)

**What.** Add to the rules link flags (next to the existing stack override in
the rules block, see `WASM_RULES_FLAGS` / `Makefile:310-321`):

```
-Wl,--initial-memory=327680 -Wl,--max-memory=327680
```

(5 pages today; re-pin lower after each landed candidate. Copy the comment
style from the guards pin at `Makefile:342-358`.)

**Why.** Converts every future silent regrowth into a loud failure: if a
buffer grows past the pin, **wasm-ld refuses to link**; if anything ever calls
`memory.grow` at runtime, it traps. This is exactly the guards.wasm precedent
and it is what makes the tight headrooms in R2/R3 acceptable — tightness
becomes a build error, not a production surprise.

**Verification.** Build + run the entire e2e suite (fuzz drives the module
hardest). Any hidden growth path traps loudly. Also assert in
`e2e/mem/wasm_memory.test.ts` that the rules instance's
`memory.buffer.byteLength === 327680` (mirror the bots assertions there).

**Cost/risk.** None at runtime (same declared memory). ~30 minutes.

---

## 4. R1 — the arena overlay: alias the replay family into the action family
### (the M8/M9 transfer; rules.wasm 5 → 4 pages; the main prize)

### 4.1 The two families and why they never coexist

**Action family** (live during action/marshal/menu exports):
`g_moves` (59,396) + `g_snaps` (18,560) + `g_io` (24,576) = **102,532 B**.

**Replay family** (live only inside one `wasm_replay_encode`/`decode` call):
`g_rec` (49,152) + `g_bn` (10,756) + `g_replay_io` (32,768) = **92,676 B**.

Alias the replay family into the same bytes as the action family →
**saves 92,676 B**. New total ≈ 315,900 − 92,676 = **223,224 B = 3.41 pages →
4 pages** (262,144 B), with a comfortable 38,920 B of headroom.

### 4.2 Why this is SAFER than the bots version (read the TS evidence)

The bots overlay had to prove solver-vs-replay non-concurrency. The rules
module's TS bridge is simpler; three facts close the argument:

1. **Replay calls are hermetic.** `kernelReplayRun`
   (`supabase/functions/_shared/wasm/engine.ts:1375-1383`) is one synchronous
   function: write input at `wasm_replay_io_ptr()` → call encode/decode →
   `slice()` the output. Nothing else can touch the instance mid-call
   (single-threaded wasm), and the output is **copied out** before return.
2. **Menu consumption is hermetic.** `kernelLegalMoves` (`engine.ts:1402+`)
   marshals, enumerates (`wasm_legal_moves`), and drains every
   `wasm_export_moves` chunk **inside the same synchronous function**, into JS
   objects. `g_moves` is dead the moment it returns.
3. **There is NO index-into-resident-menu contract on this module.**
   `wasm_apply_action(seat, wireLen)` (`engine.ts:687`) takes the action as
   **wire bytes**, not a menu index. (The W2 index contract binds the *bots*
   choose path only.) So no later call ever reads a previously-built `g_moves`.

The one genuinely open proof obligation:

- **P1 — snapshots are drained before any replay call can occur.** Snap slots
  are written by the hook during an action and read back by the marshal
  (`wasm_snap_count`/`wasm_snap_tag`/`wasm_snap_aux`/`wasm_export_snapshot`)
  in the same handler invocation. Verify by reading the call site of
  `wasm_snap_count` in `engine.ts` and confirming the reads complete inside
  the same synchronous marshal that ran the action. If any code path stashes
  a snapshot index for later, the overlay is off for `g_snaps` (drop it from
  the arena; you lose 18,560 B of the win, still 4 pages — redo the math).

### 4.3 Implementation blueprint

Follow the **exact** pattern already shipped in the bots build — read these
three references first:
- `cnitro/src/wasm_overlay.h` — offset macros + `_Static_assert` style.
- `cnitro/src/replay.c` (search `CD_WASM_OVERLAY`) — how `g_rec`/`g_bn`
  become `#define`s over an anchor without touching native builds.
- `cnitro/wasm/wasm_api.c` (search `CD_WASM_OVERLAY`) — same for
  `g_replay_io`/`g_io`.

Differences for the rules build:

1. **New flag, new anchor.** The bots anchor is
   `cd_overlay = (unsigned char *)&solve_ws` in `cordite_sim.c` — rules does
   NOT link `cordite_sim.c`, so it needs its own arena + anchor. Add to
   `wasm/wasm_api.c`:

   ```c
   #ifdef CD_RULES_OVERLAY
   // Arena hosting BOTH families at distinct offsets per family; the two
   // families alias each other. Sized by the larger (action) family.
   _Alignas(16) static unsigned char g_rules_arena[RULES_ARENA_SIZE];
   unsigned char *const rules_overlay = g_rules_arena;
   #endif
   ```

   Put the offset macros in a new `src/rules_overlay.h` (mirror
   `wasm_overlay.h`'s comment discipline). Compute offsets with `sizeof`,
   16-align each, and `_Static_assert` every member fits.
   Layouts (both start at offset 0 — they alias):
   - action family: `moves @0`, `snaps @align16(sizeof(LegalMoves))`,
     `io @snaps_off + align16(sizeof g_snaps)`
   - replay family: `rec @0`, `bn @align16(REC bytes)`, `replay_io @bn_off + align16(sizeof(Bn))`
   - `RULES_ARENA_SIZE = max(action_end, replay_end)` — assert both.

2. **Flag exclusivity.** `wasm_api.c` and `replay.c` are ALSO compiled into
   bots.wasm with `CD_WASM_OVERLAY` defined. The ifdef chains must be
   mutually exclusive and defensive:

   ```c
   #if defined(CD_WASM_OVERLAY) && defined(CD_RULES_OVERLAY)
   #error "pick one overlay flavor"
   #endif
   ```

   Add `-DCD_RULES_OVERLAY` to `WASM_RULES_FLAGS` ONLY (`Makefile:310`) —
   never to the shared `WASM_FLAGS` (guards + native must see plain statics;
   guards doesn't compile these files anyway, but native does).

3. **Alignment invariants.** `SnapSlot` is `_Alignas(8)`; `Bn`/`RecChoice`
   need 4. The 16-aligned arena + 16-aligned offsets satisfy all. Assert:
   `_Static_assert(_Alignof(SnapSlot) <= 16, ...)` etc.

4. **TS pointer caching is safe.** `wasm_io_ptr()`/`wasm_replay_io_ptr()`
   return fixed static addresses (now arena+offset — still link-time
   constants). The chunk math in `kernelLegalMoves` derives from
   `wasm_io_cap()` which is unchanged.

5. **Re-pin.** Drop the R0 pin to `-Wl,--initial-memory=262144
   --max-memory=262144` (4 pages) in the same commit. The link will fail if
   your arithmetic is off — that's the design.

### 4.4 Gates for R1 (all must be green; run in this order)

1. Native: `cd cnitro && make tests && ./build/cnitro_tests` (161/161) —
   native must be BYTE-UNCHANGED (flag absent ⇒ plain statics; verify with a
   before/after `sha256sum build/cnitro_tests` if paranoid).
2. Difftests: `./build/sim_difftest 4 1000`, `./build/apply_difftest 4 200`,
   `./build/replay_difftest 40`. (Ignore `solver_difftest` "mismatches" —
   that tool compares two different solvers and exits 1 by design; it is not
   a regression signal. Compare its count to main if worried: 428 today.)
3. Rebuild + embed: `make wasm` (regenerates
   `supabase/functions/_shared/wasm/rules_wasm.ts` — commit it).
4. `npm run test:e2e` core: `replay_codec`, `replay_codec_edges` (byte-exact
   vs the frozen TS oracle), `client_rules_parity`, `fuzz`, `cover`,
   `cover_combinations`, `state_codec`, `view_codec`, `awire_codec`,
   `packed_wire_parity`, `marshal_resident`.
   (DB-backed tests need Postgres; in an environment without it, expect
   `ECONNREFUSED 127.0.0.1:5432` failures on meta/fuzz-DB tests — those are
   environmental, CI covers them. The wasm/codec tests run without DB.)
5. **Write the new interleave gate** (model: `e2e/replay_solver_overlay.test.ts`):
   on ONE rules instance — (a) enumerate a large menu and capture the decoded
   move list; (b) run a full replay encode AND decode of a real game;
   (c) enumerate the same state again → the move lists must be identical;
   (d) `wasm_export_state` before vs after the replay calls → byte-identical
   (proves `g_game` untouched); (e) action → replay encode → next action's
   snaps still correct (covers P1).
6. `npm run test:mem` + extend `e2e/mem/wasm_memory.test.ts` with the rules
   page assertion (4 pages, flat).
7. CI: push and let `c-engine` / `validate` / `mem` / `edge-serve` /
   `metrics` confirm. Expect the metrics comment to show rules.wasm size
   unchanged (code doesn't move) and NO latency deltas (the bench only
   exercises bots; engine-speed rows must stay ⚪ neutral).

**Expected result to report:** rules.wasm 5 → 4 pages (327,680 → 262,144 B),
zero behavior change, native builds byte-identical.

---

## 5. The road to 3 pages (only after R1 is merged; pick ONE route)

3 pages = 196,608 B. After R1 you're at ~223,224 B — need another ~26.6 KiB.

### R3 — `MAX_LEGAL_MOVES` 1024 → 512 for the RULES module only (−29,698 B)

**The case for it:** the rules menu is *test-surface only*. Production menus
and bot enumeration run on the adopted bots instance (engine-slot adoption,
`engine.ts:181 __adoptEngine`); production validation is direct rule-checking,
not menu membership; the client's move gating is guards.wasm. The Makefile
already documents 1024 as "covers every menu the parity/fuzz suites produce"
(`Makefile:299-303`).

**The case against (why the bots M2 was NEGATIVE):** real 8-player large-hand
defender states can enumerate >16k moves. If ANY suite (or any future
non-adopted deployment) drives such a state through the rules module at 512,
the clamp changes what the suite sees. Before shipping: grep the suites for
menu-size assertions, run `fuzz` + `cover_combinations` at 512 repeatedly, and
confirm the enumerate path's overflow behavior at the cap is a clean clamp
with the count still reported (read `legal.c`'s cap handling first).

**Arithmetic:** 223,224 − 29,698 = 193,526 B → 3 pages with only **3,082 B
headroom**. That is brittle — acceptable ONLY because the R0 pin turns a
future overflow into a link failure, but any later feature that adds a static
buffer will bounce off the pin and force a revisit. Say so in the Makefile
comment.

### R4 — stack 64 → 32 KiB after shrinking the cover-enumeration frame (−32,768 B)

The M7a transfer. Preconditions and accounting:

1. Rules' measured stack worst is **~45 KiB, in the cover enumeration**
   (`Makefile:309-314`) — so a 32 KiB stack is UNSAFE today. You must first
   shrink that frame.
2. Adapt the canary: `e2e/stack_canary.mts` (written for the bots kernel)
   — point it at the rules embed and drive the cover/fuzz corpora to get the
   true high-water. `--stack-first` means any miss is a loud trap, and the
   fuzz + cover e2e suites exercise the enumeration harder than production.
3. Find the fat frames: the recursion in `legal.c`'s cover-combination
   generation (start where `LEGAL_STATS` hooks were added in #61). Apply the
   M7a pattern: hoist per-depth locals to `static` depth-indexed arrays
   (single-threaded wasm; keep `_Thread_local` in the declaration so native
   arena builds stay safe — copy the `sim_rec_moves` pattern from
   `cordite_sim.c:1277-1281` including the comment about why indexing is
   bounded).
4. **The accounting rule that makes or breaks this:** hoisted BSS is new
   static memory. Net = (stack cut) − (hoisted bytes). You need net ≥ 26,616 B
   to cross into 3 pages, i.e. hoisted scratch must stay under ~6 KiB — OR
   alias the hoisted scratch into the arena's replay-family bytes (they are
   dead during enumeration — enumeration happens in action time). If you do
   that, extend the interleave gate with an
   enumerate→replay→enumerate-byte-identical case (already in R1's gate) plus
   a cover-heavy corpus.
5. Ship stack 32 KiB only at ≥1.4× the NEW measured high-water, with the
   margin stated in the Makefile comment. If the hoist only gets the
   high-water to ~30 KiB, ship 48 KiB (−16,384 B) and take 4 pages with fat
   headroom instead — do not shave the margin to force 3 pages.

**Recommendation:** land R1 (+re-pin at 4 pages) and STOP unless there's a
concrete reason to chase 3. R3 is a one-line change with an audit burden and
3 KiB headroom; R4 is a day of careful work that also (per the bots round)
tends to make enumeration slightly *faster* — the M7a hoist measured +8.6%
on octogen's solve path. If you go, prefer R4; use R3 only with the pin.

---

## 6. Full gate checklist (copy into every PR description)

- native `cnitro_tests` 161/161; native binaries byte-identical when the new
  flag is absent
- `sim`/`apply`/`replay` difftests: 0 real divergences
  (`solver_difftest`'s nonzero exit is expected — different-solver study tool)
- `make wasm` embed regenerated + committed (`rules_wasm.ts`); bots.wasm.gz
  UNTOUCHED (if it changed, you leaked a flag into shared `WASM_FLAGS` — stop)
- guards.wasm byte-identical (it shares `game.c`/`view.c`/`awire.c` — verify
  `sha256sum build/guards.wasm` before/after)
- e2e: `replay_codec` + `replay_codec_edges` byte-exact, `client_rules_parity`,
  `client_guards`, `fuzz`, `cover`, `cover_combinations`, codecs
  (`state`/`view`/`awire`), `packed_wire_parity`, `marshal_resident`
- new/extended interleave gate green (§4.4.5)
- `test:mem` extended with the rules page + flatness assertion
- pin holds: link succeeds at the target `--initial-memory`, full suite runs
  with zero `memory.grow` traps
- CI metrics comment: engine-speed rows ⚪ neutral, wasm-size table unchanged
  for rules code, memory table reflects the new page count
- docs updated: this file's ledger (§7), `WASM_L1_BUDGET.md` result table,
  anatomy page regenerated

## 7. Execution ledger (append results here, bots-plan style)

| candidate | status | measured result |
|---|---|---|
| R0 pin @5 pages | — | |
| R1 arena overlay → 4 pages + re-pin | — | |
| R2/R3/R4 → 3 pages | — | |

---

## 8. Context you might be missing (why things are the way they are)

- **Why rules.wasm exists at all when bots.wasm serves everything:** bot games
  adopt the bots instance as THE engine (resident fast path). Human-only edge
  paths and the parity/fuzz oracles run the rules module. It must keep the
  full rules API including the replay codec (the M6-style split is blocked
  for the same reason it was blocked for bots).
- **Why guards is special:** it runs on the player's browser main thread; the
  1-page pin IS the shipped feature (L1-resident on Graviton/M-series). Its
  snapshot ring is `MAX_SNAPS=1` because it exports no snapshot readers.
- **Why the replay tables are untouchable:** the codec is byte-compared
  against a frozen TS oracle (`e2e/replay_ts_oracle.ts`); `g_comb`/`g_opts`/
  `g_weights` and the REC/BN caps are wire format (W4).
- **Latency reality check:** none of this round buys player-visible speed —
  the production hot path is bots.wasm (already done; octogen p50 ~66 ms,
  neutral through the whole shrink). Rules-module wins are edge memory
  footprint and regression-proofing. Do not spend latency-optimization effort
  here; if you have speed budget, the known lever is the bots solver, not
  these modules.
- **Benching note:** the CI metrics bench (`BENCH_BOTS`) only measures bots
  strategies — a rules change shows up ONLY in the memory/size tables. Any
  bot named there must be one the wasm dispatch actually ships
  (`wasm_bots_api.c`), or the bench silently measures a random-fallback move
  (this bit us in #61: phantom −80% "speedups" for de-shipped bots).

---

*Prepared 2026-07-11 from the merged state of #61 (`main`). Measurements in
§1 taken from a fresh build at that commit; re-measure with §0's commands if
main has moved.*
