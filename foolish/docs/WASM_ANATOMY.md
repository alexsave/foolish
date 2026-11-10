# Where bots.wasm's 191,915 bytes go

Measured 2026-09-22 on `origin/main` at `507c9695`, in a throwaway worktree, with the pinned toolchain (Homebrew clang 22.1.8, LLD 22.1.8, binaryen 130, the same versions `scripts/ci_llvm.sh` pins).
Every number below was read off a binary built in that worktree; none is copied from a comment.

## 0. Provenance, and one number I could not reproduce

- `c/build/bots.wasm`: **191,915 B raw**, sha1 `325dcbab60f48bb8a1bce8b238739cfb1cde95ce`; `gzip -9 -n` (Apple gzip) **82,224 B**.
- `c/build/web.wasm`: **67,298 B raw**, sha1 `f4f46567...`; gz 31,050 B.
- Freshness proof: wiped `c/build/botobj` and both modules, touched every `.c`/`.h` under `c/src`, `c/wasm`, `shared/c`, rebuilt from scratch: identical sha1 on both modules.
  `c/tools/wasm_anatomy/generate.sh` then ran green, including its own guard that every function body of the name-preserving companion is byte-identical to the stripped module (388 functions).
- The same objects re-linked through my own variant harness (exact link line and exact `wasm-opt` line from `make -n`) reproduce the shipped sha1, so the variant sizes in section 6 are trustworthy.
- The pin in `e2e/mem/wasm_memory.test.ts` on `origin/main` is `BOTS_RAW_MAX = 192_000` with the comment "191,915 B today", which matches what I measured.
- The brief's **192,186 B** is not reproducible from this tree on this machine.
  The other checkout on this Mac (another checkout on this Mac, branch `claude/wasm-build-not-committed`) has a 191,485 B `bots.wasm` and `.gz` on disk, which is the pre-#221 number that `scripts/wasm_build.sh`, `scripts/ci_llvm.sh`, and the `c/Makefile` comments all still quote.
  Whoever raises the pin to 193,000 should measure the artifact the raising branch actually ships, on the lane that ships it; a 271 B gap between two "same clang" builds is exactly the size of an export-list or knob-string change, not compiler noise (the repo's own cross-OS measurement says raw bytes are identical under the pin).
- `c/tools/wasm_anatomy/README.md` quotes "191,490 B", another stale figure.

## 1. Sections

bots.wasm, 191,915 B:

| section    | bytes   | share  |
|------------|---------|--------|
| CODE       | 183,031 | 95.37% |
| DATA       | 5,244   | 2.73%  |
| EXPORT     | 2,897   | 1.51%  |
| FUNCTION   | 390     | 0.20%  |
| TYPE       | 262     | 0.14%  |
| ELEM       | 40      | 0.02%  |
| GLOBAL, TABLE, MEMORY, DATACOUNT | 17 | 0.01% |
| magic + version + 10 section headers | 34 | 0.02% |

Sum check: 191,881 B of section payload + 34 B of framing = 191,915 B, residual 0.
CODE decomposes exactly too: 182,444 B of function bodies + 585 B of per-body size LEBs + 2 B function-count LEB = 183,031.
The symbol sizes in this report are those body bytes, so they sum to CODE minus 587 B of LEB framing, and nothing is unaccounted for.
No custom sections ship (`--strip-all`); the name section on the companion build is 10,440 B, which is what stripping saves.
The indirect-call table has 24 funcref entries.
118 exports, 388 functions, 33 type signatures, no imports.

web.wasm for comparison: CODE 64,598 (95.99%), DATA 715, EXPORT 1,567, 180 functions, 63 exports.

## 2. CODE by subsystem

One trap governs this whole section.
The shipped module goes through `wasm-opt -O2 --inlining-optimizing`, which folds 160 functions (82,603 B of bodies) into their callers and shrinks CODE from 201,409 to 183,031 B (-18,378 B, -9.1%).
In the shipped bytes `wasm_table_bot_drive` is one **40,716 B** function, 21.2% of the module, because `octogen_strategy_choose` (16,445 B), `cordite_strategy_choose` (13,542 B), `blackpowder_strategy_choose` (6,718 B), `bot_drive` (1,444 B) and the roster dispatch were all inlined into it.
`bot_drive.c` shows 52 B and `firecracker_strategy.c` does not appear at all.
So per-file attribution of the shipped bytes is wrong for exactly the subsystems this report is about.

The honest ownership table is the same objects linked WITHOUT the wasm-opt pass (`WASM_BOTS_POSTOPT=`, so each brain is still its own function).
Pre-opt bodies total 200,608 B; shipped bodies total 182,444 B; scale the pre-opt column by ~0.91 for a shipped estimate.

| subsystem | pre-opt B | % of pre-opt CODE | shipped standalone remnant B | note |
|---|---|---|---|---|
| bot: cordite (`cordite_strategy.c`) | 23,513 | 11.7% | 9,092 | 13,542 B chooser inlined into `wasm_table_bot_drive` |
| bot: octogen (`octogen_strategy.c`) | 21,719 | 10.8% | 4,747 | 16,445 B chooser inlined |
| bot: espresso (`espresso_strategy.c`) | 9,321 | 4.6% | 9,017 | rollout policy for firecracker and blackpowder; not a rung |
| bot: blackpowder | 8,268 | 4.1% | 1,332 | 6,718 B chooser inlined |
| bot: robusta | 2,901 | 1.4% | 2,742 | `robusta_mc_choose`; firecracker IS this with an espresso rollout |
| bot: handwritten_prod (the "handwritten" rung) | 2,299 | 1.1% | 514 | |
| bot: handwritten (`handwritten_strategy.c`) | 1,622 | 0.8% | 1,515 | rollout policy for robusta and blackpowder |
| bot: simple_heuristic | 1,078 | 0.5% | 291 | linked and dispatchable but `seeded=0` in the roster |
| bot: random | 96 | 0.0% | 0 | |
| bot: firecracker | 33 | 0.0% | 0 | one call into robusta |
| **all bot families** | **70,850** | **35.3%** | | |
| solver + leaf book (`cordite_sim.c`) | 25,273 | 12.6% | 21,693 | plus 2,637 B of DATA (section 4) |
| engine (`game`, `legal`, `view`, `deal_rng`) | 21,482 | 10.7% | 17,107 | shared with web.wasm |
| replay codec (`replay.c` + `replay_extras.c`) | 20,475 | 10.2% | 15,310 | `run_replay_v6` alone 7,671 B; mostly shared with web |
| C Table (`table.c`) | 10,650 | 5.3% | 3,101 | 29 functions inlined into `wasm_table_*` |
| client_table (the web slot) | 10,060 | 5.0% | 3,061 | browser code, in the server link (section 5) |
| anim_plan | 7,999 | 4.0% | 1,685 | browser code, in the server link (section 5) |
| msg_wire + `wasm_msg_api` (FMSG) | 5,397 | 2.7% | 6,112 | |
| wasm bridge `wasm_api.c` | 4,870 | 2.4% | 11,911 | grows because anim/msg/replay bodies inline into it |
| wire (`awire`, `evwire`) | 4,777 | 2.4% | 4,178 | |
| roster seat codec (`roster.c`) | 4,773 | 2.4% | 2,788 | |
| replay steps (`replay_steps.c`) | 4,432 | 2.2% | 2,853 | |
| `wasm_table_api.c` (own bytes) | 3,658 | 1.8% | 59,469 | the 59 KB is everything inlined into the table exports |
| bot drive loop (`bot_drive.c`) | 1,957 | 1.0% | 52 | |
| `wasm_bots_api.c` | 1,807 | 0.9% | 2,497 | includes `probe_capture` 336 B, `malloc` 127 B, `atoi` 144 B |
| bot roster + knobs | 1,110 | 0.6% | 740 | |
| sha256 | 1,038 | 0.5% | 616 | |
| i18n | 0 | 0 | 0 | not in `WASM_BOT_SRC`; no translation symbol is in either module |
| **total** | **200,608** | **100%** | **182,444** | |

Grouped the way the brief asked, pre-opt: bot brains 35.3%, replay + wire codecs 14.8% (29,684 B), solver + leaf book 12.6%, C Table + client slot + table bridge 12.1% (24,368 B), engine 10.7%, anim 4.0%, bridge 3.3%, msg_wire 2.7%, roster/drive/knobs 3.9%, sha256 0.5%.

What only the server pays (bots minus web, pre-opt, no inlining skew): **125,961 B** of bodies.
The largest server-only items are the solver 25,273, cordite 23,513, octogen 21,719, C Table 10,573, espresso 9,321, blackpowder 8,268, then replay codec 3,193 and engine 2,931 (the parts only bot play reaches).
The shipped CODE delta is 183,031 - 64,598 = 118,433 B; the module delta is 124,617 B.

## 3. The biggest functions

Shipped (post-opt) bodies:

| B | function | owner |
|---|---|---|
| 40,716 | `wasm_table_bot_drive` | `wasm_table_api.c`, with octogen + cordite + blackpowder choosers, `bot_drive`, roster dispatch inlined |
| 8,385 | `espresso_strategy_choose` | espresso |
| 6,936 | `run_replay_v6` | replay |
| 6,372 | `cd_simulate` | cordite (struct-engine rollout, see section 5e) |
| 3,861 | `sim_solve_rec` | cordite_sim (2,218 pre-opt; grows by inlining) |
| 3,568 | `wasm_anim_build_plan` | wasm_api (anim_plan inlined) |
| 3,021 | `sim_handwritten_move` | cordite_sim (bitboard re-implementation of the handwritten policy) |
| 2,764 | `og_pick_candidates` | octogen |
| 2,742 | `robusta_mc_choose` | robusta |
| 2,438 | `sim_gen_moves` | cordite_sim |
| 2,292 | `cd_sim_from_game` | cordite_sim |
| 2,097 | `wasm_client_play` | wasm_table_api (client slot) |
| 1,999 | `evwire_walk` | evwire |
| 1,998 | `cd_pick_candidates` | cordite |
| 1,765 | `sim_loose_move` | cordite_sim |
| 1,629 | `replay_encode_v6_from_game` | replay |
| 1,515 | `handwritten_strategy_choose` | handwritten |
| 1,443 | `rs_play` | replay_steps |
| 1,402 | `wasm_client_board_edit` | client slot |
| 1,395 | `wasm_msg_seal` | msg |

Pre-opt (true ownership): `octogen_strategy_choose` 16,445; `cordite_strategy_choose` 13,542; `espresso_strategy_choose` 8,646; `run_replay_v6` 7,671; `cd_simulate` 6,931; `blackpowder_strategy_choose` 6,718; `sim_handwritten_move` 3,224; `robusta_mc_choose` 2,868; `og_pick_candidates` 2,864; `sim_gen_moves` 2,618; `cd_sim_from_game` 2,606; `sim_solve_rec` 2,218; `evwire_walk` 2,201; `cd_pick_candidates` 2,091; `cd_sim_playout_reply` 1,907; `sim_loose_move` 1,866; `anim_build_plan` 1,721; `leafbook_key` 1,667; `handwritten_prod_strategy_choose` 1,650; `handwritten_strategy_choose` 1,622.

## 4. DATA: 5,244 B, and the leaf book is half of it

Six segments, 5,196 B of payload plus 48 B of segment headers; the objects carry 5,232 B of initialized data before the linker merges duplicate strings.

| bytes | what | owner |
|---|---|---|
| 1,883 | `leafbook_vals` (one value byte per canonical K=4 endgame; the visible `!#%` run is `outcome<<4 \| distance`) | cordite_sim |
| 754 | `leafbook_disp` (the minimal-perfect-hash displacement table, M=377) | cordite_sim |
| 884 | 63 env-knob key strings (`OG_*`) and small `.data` knob defaults | octogen |
| 687 | 42 knob key strings (`CD_*`) | cordite |
| 319 | `ROSTER` table (160 B) + rung names | bot_roster |
| 270 | `BP_*` knob keys and three `BP_VERIFY: ...` `fprintf` format strings | blackpowder |
| 256 | SHA-256 round constants `K` | sha256 |
| 33 | `REPLAY_B32_ALPHA` | replay |
| 20 | `ELO_STEPS` | table |
| ~120 | initialized scalars (`g_seed`, `g_move_cap`, `msg_last_body_version`, ...) | various |

So the leaf book costs **2,637 B of DATA** plus `leafbook_key` (1,667 B pre-opt, inlined in the shipped module) and `cd_leafbook_value` (161 B).
The `c/Makefile` comment "The book data (~27 KiB read-only)" is stale: 27 KiB describes the K=5 book (19,715 entries); the shipped book is K=4 with 1,883 entries, as `c/LEAFBOOK.md` says.
Nothing else in DATA is a table worth naming; the rest is the knob vocabulary.

Not module bytes, but since the brief asked about static tables: zero-initialized BSS in the objects totals 2,395,463 B of linear memory.
The big ones are `g_io` 409,600, `solve_ws` 278,592, `rs_play.acts` 278,528, `g_scratch` (bot_drive) 237,572, `g_moves` 237,572, and four full `Game`-sized 135,944 B slots (`g_game`, `g_rs_game`, `wasm_msg_seal.scratch`, `msg_rebase_one.probe`).
`docs/WASM_L1_BUDGET.md` and `docs/BOTS_WASM_MEMORY_PLAN.md` own those; I did not re-derive them.

## 5. What is in the server link that the server never calls

I measured the reach set the way `e2e/wasm_web_link.test.ts` measures the browser's: esbuild, tree shaking on, over the five edge-function entries (`server/impls/supabase/functions/{action,bot-heartbeat,create,delete-account,meta}/index.ts`, with their `deno.json` aliases), collecting every `wasm_*` name that survives; and the repo's own `browserWasmReach()` for the browser.
Caveat: like the web test, this misses an export bound through a computed name; I looked for that pattern in `sdk/ts/wasm/bots.ts`, `wasm_asset.ts`, and `server_table.ts` and found none (the only `Record<string,...>` tables are constants).

Result: the server bundle reaches **44** of bots.wasm's 118 exports (36 `wasm_table_*`, the 3 `wasm_belief_probe_*`, `wasm_elo_deltas`, `wasm_init`, `wasm_io_ptr`, `wasm_io_cap`, `wasm_layout_hash`).
The other 74 split as:

**(a) 15 exports reached by neither shipped bundle**, only by e2e tests and by two `server/api/common/replay` call sites the edge functions tree-shake away:
`wasm_anim_beats_ptr`, `wasm_anim_build_beats`, `wasm_anim_conflict_verdicts`, `wasm_anim_transport`, `wasm_cards_a_ptr`, `wasm_cards_b_ptr`, `wasm_legal_moves`, `wasm_legal_moves_ptr`, `wasm_msg_pickup_hold`, `wasm_msg_rebase`, `wasm_msg_rule_p`, `wasm_msg_seal`, `wasm_replay_encode_v6_from_game`, `wasm_replay_extras_encode`, `wasm_replay_link_parse`.
`wasm_anim_transport` and `wasm_msg_pickup_hold` have a binder wrapper and no caller at all, not even a test.
The iMessage extension does not need the `wasm_msg_*` four here; it has its own `msg.wasm` link with its own export list.
Variant link without them: **-6,533 B raw, -2,817 B gz**.
This is the class `WASM_BOTS_UNSHIPPED_API` in `c/Makefile` exists for (it has been used four times already, most recently for `wasm_replay_error_detail`), and `e2e/validation/wasm_exports_validation.test.ts` cannot see it because that gate checks "every export is bound", not "every export is reached".

**(b) 58 exports reached only by the browser bundle** and carried in the server link because `WASM_BOTS_SHIPPED_EXPORTS` is the union: the whole client slot (28 `wasm_client_*`), the rest of anim (11 `wasm_anim_*`), the replay reader (16 `wasm_replay_*`), `wasm_msg_decode`, `wasm_msg_header_ptr`, `wasm_bot_roster_dump`, `wasm_can_cover`.
Variant links: minus all 58: **-31,668 B raw, -14,103 B gz**; minus (a) and (b) together, i.e. bots.wasm linked to the server's reach set: **148,783 B raw / 62,038 B gz, -43,132 B raw (-22.5%), -20,186 B gz**.
Isolated: all `wasm_anim_*` -10,009 B; all `wasm_client_*` -15,094 B; all `wasm_msg_*` -6,747 B; the replay reader -9,908 B (these overlap through shared code and do not sum to the union).
This is deliberate today, not an accident: `wasm_build.sh`, the anatomy README and the Makefile all frame bots.wasm as "everything web.wasm has plus the table and bots", because the Node e2e suites drive every binder entry through the one module off disk.

**(c) Test instruments in the shipped server link**: `wasm_belief_probe_{reset,dump,ptr}` are reached by `server_table.ts` (it binds `__beliefProbeReset/Dump`) but no non-test code calls those methods; `probe_capture` (336 B) runs on every `BOT_DRIVE_PHASE_CHOOSE`.
Bytes: -123 B raw.

**(d) A rung the server cannot seed**: `simple_heuristic` is linked and dispatchable but has `seeded=0` in the roster (the seven seeded rungs in `seed.sql` are random, handwritten, robusta, firecracker, blackpowder, cordite, octogen).
1,078 B pre-opt, 291 B standalone after inlining.

**(e) A code path the shipped configuration cannot reach**: the struct-engine rollout (`cd_simulate`, `og_simulate`, the `CD_DIFFTEST`/`OG_DIFFTEST` fast-vs-slow comparison).
`cd_rollout` takes it only under `CD_NO_FASTROLL`, `CD_LEAF`, `CD_DIFFTEST`, or when sampled seat weights deviate; octogen likewise.
In the shipped module the env table is filled only by the roster's compiled-in knob strings (`CD_BUDGET=prod,CD_RACE=1,CD_RACE_C=75` and `OG_TRUMP_KEEP=...`) because `wasm_setenv_from_io` is not exported (0 hits in the export table), and `cordite_set_seat_weights` is called only by `fulminate_strategy.c`, which is not linked.
I stubbed the four dispatch sites in my worktree, rebuilt the two objects, and relinked: **-8,588 B raw, -2,821 B gz**; the drop is exactly `cd_simulate` (6,931 B pre-opt), `og_simulate` (1,461 B) and 1,336 B of inlined dispatch.
Sources restored and the baseline sha1 verified afterwards.
`espresso` and `handwritten` do NOT go with it: they are the rollout policies of firecracker, robusta and blackpowder (`firecracker_strategy.c:24`, `robusta_strategy.c:261`, `blackpowder_strategy.c:358`), so they are live for three seeded rungs.

**(f) Debug strings in a module with no stdout**: three `BP_VERIFY` `fprintf` formats (~190 B of DATA), `og_difftest_report` + `atexit` under `OG_DIFFTEST`.
Trivial.

**Duplication**: none that is removable.
`sim_handwritten_move` (3,224 B) is the bitboard re-implementation of the handwritten policy inside the solver and `handwritten_strategy_choose` (1,622 B) is the struct one; both are live (the solver's playouts and robusta's rollouts respectively) and merging them changes play.
`handwritten` and `handwritten_prod` are two different rungs.
The roster's `#ifndef FOOLISH_SEEDED_BOTS_ONLY` already keeps `espresso_prod` and `gunpowder` out, and it works: neither symbol is in the module.

**Tooling drift worth fixing (not shipped code)**: `c/tools/wasm_anatomy/build_html.mjs`'s `FILE_SUB` taxonomy predates the split, so `table`, `client_table`, `msg_wire`, `anim_plan`, `roster`, `bot_drive`, `bot_roster`, `bot_knobs`, `robusta`, `firecracker`, `blackpowder`, `replay_steps`, `replay_extras`, `sha256`, `card`, `wasm_table_api`, `wasm_msg_api` all land in "other"; its subsystem chart is not usable for the current module.
Its README also still says 191,490 B.

## 6. Cuts, ranked by bytes per unit of risk

1. **Move the 15 unreached exports (5a) to `WASM_BOTS_UNSHIPPED_API`.** -6,533 B raw / -2,817 B gz (3.4% of the module).
   Risk: low; this is the existing mechanism and the test build (`bots_test.wasm`) keeps them.
   What breaks: the e2e suites that call `animBuildBeats`, `animConflictVerdicts`, `kernelMsg{Seal,Rebase,RuleP}`, `kernelResidentReplayCodeV6`, `kernelReplayExtrasEncode`, `kernelReplayLinkParse` and the raw `wasm_legal_moves`/`wasm_cards_*_ptr` helpers must load the test build the way `table_fixture.ts` already does; and the two dead call sites in `server/api/common/replay/{extras,codec}.ts` need deleting or an entry that reaches them.
   `wasm_web_link.test.ts` and the exports gate are unaffected (none of the 15 is in web.wasm).
   This is the one cheap cut.

2. **Link bots.wasm to the server's reach set (5a + 5b).** -43,132 B raw / -20,186 B gz (22.5%).
   Risk: medium, and it is an architecture decision, not a trim.
   What breaks: every Node e2e suite that drives a browser entry (client slot, anim, replay reader, FMSG decode) through bots.wasm has to load web.wasm or the test build instead; `wasm_exports_validation.test.ts`'s "every export is bound" becomes per-module; three documents that describe bots.wasm as the superset change meaning.
   Whether the edge function's cold start cares about 20 KB of gzip is the product question; the repo's own record says the download budget was the browser's and #221 already solved that.

3. **Compile the struct rollout out of the shipped objects (5e).** -8,588 B raw / -2,821 B gz.
   Risk: medium.
   What breaks: the shipped objects and the test build stop being the same objects (the invariant the test build rests on), unless the test build accepts losing `CD_NO_FASTROLL`/`CD_LEAF`/`CD_DIFFTEST` on wasm too; the fast-equals-slow difftest that validates the bitboard playout lives on this path; research knobs stop working on wasm while native is unchanged.
   I would not do this for 2.8 KB of gzip.

Not worth it, with the measured number so nobody re-litigates it:
- the belief probe exports (-123 B raw); if anything, the argument is runtime cost of `probe_capture` per bot decision, not bytes;
- `wasm_elo_deltas` (-323 B; and it is reached by the server);
- `wasm_bot_roster_dump` (-295 B; the browser reaches it);
- `simple_heuristic` (at most 1,078 B, needs roster surgery for a rung the ladder documents);
- the `BP_VERIFY` strings (~190 B);
- the leaf book (2,637 B DATA + ~1.8 KB code) - it buys octogen ~15% p50 per `c/LEAFBOOK.md`;
- sha256's K table (256 B, needed for seals);
- `wasm-opt -Oz` instead of `-O2 --inlining-optimizing` (the Makefile measured ~150 B, unverified for warm latency);
- any per-family cut: cordite + octogen + the solver are 70,505 B pre-opt (35% of CODE) and they are the top three rungs of the product's ladder; espresso and handwritten (10,943 B) are the rollout policies of the middle three rungs; blackpowder (8,268 B) is a seeded rung.

The honest conclusion: after #1 there is nothing cheap left.
The module is 35% bot brains, 13% solver, 12% table, 11% engine and 15% codecs, and all of those are the product.
The only large lever is #2, and it is a decision about what one module is for, not a trim.

## Appendix: how the numbers were produced

- Build: `make -s -C c WASM_CC=/opt/homebrew/opt/llvm/bin/clang wasm-bots wasm-web` in the worktree.
- Sections: `llvm-objdump -h` cross-checked against the JSON from `c/tools/wasm_anatomy/analyze.mjs` (identical per section).
- Per-function sizes and names: `analyze.mjs` over `build/bots.wasm` with the name-preserving companion `build/named/bots.named.wasm` that `generate.sh` builds and verifies; the same over a link with `WASM_BOTS_POSTOPT=` for pre-opt ownership.
- Symbol to file: `llvm-nm --defined-only` over `build/botobj/*.o`, the map `generate.sh` builds.
- DATA/BSS: `llvm-readobj -S` segment lists over the objects (macOS `llvm-nm` reports every wasm data symbol as `d`, so it cannot tell rodata from bss).
- Reach sets: esbuild over the five edge-function entries with tree shaking, and the repo's `browserWasmReach()`.
- Variants: the exact `clang ... -Wl,--export=...` and `wasm-opt` lines from `make -n build/bots.wasm`, with names removed, linked into scratch and gzipped with `gzip -9 -n`; the empty-drop baseline reproduces the shipped sha1.
- The stub: four one-line edits in `cordite_strategy.c` and `octogen_strategy.c` in the worktree, objects removed and rebuilt, variant linked, sources restored from a copy, objects rebuilt, baseline sha1 re-verified, `git status` clean.
- Scratch artifacts (JSON, TSVs, variant modules, the regenerated `wasm-anatomy.html`) are in a session scratchpad that is gone.
