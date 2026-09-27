# Shared consolidation pass - summary

Branch `shared-consolidation`, worktree `.claude/worktrees/shared-consolidation`.
The starting point was the three-way merge of `pickemup`, `chuiniu` and `tallybones` (`8863f0e4`), re-verified at the start of this pass: all three tips are ancestors, and every product suite was green on that tree.
Every document in this folder is an investigation or a review; this one is the ledger of what was done with them.

## What was investigated

| Doc | Topic | Verdict in one line |
| --- | --- | --- |
| `IOS_MESSAGES_LAYER.md` | the iOS Messages layer across five products | pickemup, chuiniu and tallybones adopted MessagesKit's InsertStaging and DevFlags; chuiniu and tallybones opted out of SendHint and CollapseSlide by recorded decision |
| `C_PRIMITIVES.md` | mixrad, b32, sha256, deal_rng, the wasm libc | nothing was reimplemented; one duplicated wire checksum; the guard-rail tests had not kept pace with what shipped |
| `MESSAGES_C_LAYER.md` | msg_stage in C, envelope, seat, lobby, two-phone tests | msg_stage has one entry point (the Swift face); seat and lobby are byte-identical across the three new products; the envelope is two incompatible philosophies (delta with ancestry vs whole game) |
| `POLICY_INTERFACE_AND_ARENA.md` | the struct-of-function-pointers question | no, at both levels; lift the statistics and the seed hash only |
| `KERNEL_SKELETON.md` | beats, plan, say, view, code, check, fuzz, i18n | a naming convention, not shared code, except the say string utilities and the test harness; the beat clocks are three different clocks |
| `BUILD_TEST_SCAFFOLDING.md` | Makefiles, CI, codegen, mutation ledgers | one Makefile shape already; the gaps are guard rails and a missing wasm toolchain check |
| `SECURITY_REVIEW_SHARED.md` | what was in shared/ before this pass | no decoder is memory-unsafe under millions of fuzzed inputs; three Medium findings, none reachable from shipped code, all fixed |
| `SECURITY_REVIEW_LIFTED.md` | what this pass added to shared/ | no memory-safety bug reachable from a bubble after 1 to 5 million fuzzed iterations per module, native and wasm32 digests equal; two Low contract-hardening fixes (a sender index past the roster, a group cap past `who[]`), neither reachable from shipped code |

## The struct-of-function-pointers question

No, at the kernel level and at the arena level, and this pass agrees with the investigation.
The architecture is one kernel per product with flat named C exports that structgen turns into Swift and TS; the five "new game" signatures already diverge where an interface would have to erase real differences.
None of the five bot arenas dispatches a policy through a function pointer today, and the games differ on player count, win condition and information hiding.
What was shared instead is the math under the arenas and one start callback in the lobby rules, which is the limit this pass set: one callback plus parameters, never a table of them.

## The CollapseSlide finding

Recorded, not acted on.
pickemup's own CollapseSlide adoption is compiled in but DEBUG-only and off unless the rig's `dev.slide` file exists, so in Release all three new products ship the same plain compact request that chuiniu and tallybones chose on purpose.
Switching either product is an on-device judgement that pickemup itself has not made yet, not a consolidation lift.

## What was lifted (phase 2, one worker per step, serialized)

| Step | Into shared/ | Out of | Proof |
| --- | --- | --- | --- |
| M1 | the guard-rail tests and README match the tree; tallybones CI gains the generated job | tests only | validate lane 150/150, four mutations red |
| M2 | `tools/wasm_cc.mk`, one wasm toolchain guard | pickemup, chuiniu, tallybones Makefiles | every wasm object and native test binary byte-identical; Apple clang now refused |
| M3 | `c/test/check.h`, `c/test/twophone.h` | pk/cn/tb `*_check.h` and the STEP/OK macros | identical counts; the harness was the only thing catching a broken kernel |
| M4 | `c/text_util/` (UTF-8 stepping, columns, itoa, put, fill) | pk/cn/tb say layers; uttt's stepping, put and fill | identical counts; uttt wasm-web driven over 23,797 positions, captions hash the same |
| M5 | `c/wire_check/` (2-byte truncated SHA-256) | uttt, pk, cn, tb message coders | 100,000-tuple differential harness per product, bubbles byte-identical; a tallybones test gap closed |
| M6 | `c/stats/` (Bessel mean/stderr, Wilson, splitmix seed hash) | foolish main_eval, tallybones solve, chuiniu Wilson, pk/cn seed hashes | arena outputs byte-identical at fixed seeds; the two deliberate statistic changes in their own commit and DECISIONS entries |
| M7 | `c/msg_seat_tag/` (roster row, name verdict, same_name, resolve) | pk, cn, tb | 70,218-line bubble dumps byte-identical; structgen hashes unchanged |
| M8 | `c/msg_lobby_roster/` (cap, new, join, leave, start, offered, plan) | pk, cn, tb (297 lines to 81) | identical counts; lobby dumps byte-identical; three test gaps found and closed in the next commit |
| M9 | `c/collapse/collapse.h` (the host's drawer spring and its two numbers, not the collapse design) | uttt `uttt_collapse_push`, pickemup's byte-identical copy | push dumps at every ms byte-identical; uttt wasm-web byte-identical; both mac test scripts green |

Alongside the stream: the security review of shared/ and its eight fixes (snprintf hang, the Release string gate that could not see short Swift literals, an unpinned LLVM install, mixrad entry guards, ship.sh argv, PackedBytes cursor, header notes, known-answer tests for the four primitives), `pickemup/c/tools/pk_play` (a terminal game against five bots on the kernel), and the "an 8" article fix in pickemup's say layer.

## Deliberately not lifted

- The beat clocks (S14): pickemup's seven bezier curves, chuiniu's one ease, tallybones' overshoot springs and foolish's index-derived plan share vocabulary, not code.
- The message envelope (S15): delta-with-ancestry (foolish, werewolf) and whole-game-per-bubble (the other four) are two designs, not one with a vtable.
- The cross-family seat and lobby unification (S8, S9): the tag-and-record family and the name-and-cached-seat family have different data models; this pass lifted the first family only.
- The Swift design families (Tokens, Buttons, Materials, BubbleSnapshot, LobbyScreen): blocked on foolish splitting its own Suit dependency out of Tokens first.
- A MessagesSurfaceController base class: would be a path change for four products and a behavior change for foolish (its D4 gating gap).
- foolish's own base32 in replay.c: a foolish behavior change needing its own step and proof.
- The Elo estimators: foolish and uttt invented two different ones; picking one is a design decision, not a dedup.
- uttt's seed hash in its arena: not splitmix64, so switching it would change every uttt game.
- Folding foolish's wasm-cc-check into the shared one, and uttt's WEB_WASM_CC guard: follow-ups, foolish is live.

## What changed in behavior, on purpose

- chuiniu's arena dice-lost standard error gains the Bessel correction (DECISIONS B5).
- pickemup's arena win-share interval is Wilson's instead of a clamped normal approximation (D66).
- uttt's arena prints a Wilson interval it did not have.
- Everything else is byte-identical where output exists, and identical in every assertion count where it does not.

## Open items for a maintainer

- The tallybones bridge turns a live leave into a game move, so the shared `can_exit` rule is unreachable through it; only the shared test covers it there.
- uttt's `uttt_anim` test reads the collapse response constant through the alias, so it moves with the constant; the shared test pins the literal.
- Werewolf keeps its own copy of `languages.h` (excluded by name in the shadow test); deleting it is werewolf's job.
- The `chuiniu.yml` tests job comment still says it compiles only deal_rng and sha256.
- Whether chuiniu and tallybones adopt pickemup's stricter warning flags (D58) is a product decision.

## Final verification of the tip

Run on the last commit, after every merge, from this worktree.

| Lane | Result |
| --- | --- |
| `make -C foolish/c tests`, `difftests`, `ios-smoke` | 7389 passed, 0 failed; difftests and smoke green |
| `cd foolish && npm run test:validate` | 150 of 150 |
| `make -C uttt/c run` | green, uttt_msg 3,062,830, uttt_anim 133, plus every shared test |
| `make -C pickemup/c run`, `asan`, `cross` | green, pk_msg 298,141, twophone 2,446, bridge 869; 100 games alike native and wasm32 |
| `make -C chuiniu/c run`, `asan`, `bot bot-test` | green, cn_msg 89,178, twophone 9,939, bot 210,779 |
| `make -C tallybones/c run`, `asan`, `bot run` | green, tb_msg 43,899, twophone 491 |
| `make -C werewolf/c tests` | green |
| uttt and pickemup `ios/scripts/mac_tests.sh` | green at M9 (the last change to a file their Swift reads); foolish `mac_tests.sh --no-lib unit` green at the PackedBytes fix |

Not re-run on the very last commit: the three iOS mac test scripts (last run at the commits named above; nothing Swift-visible changed after them) and foolish's `test:mem` (run once, 6 of 6, at the wasm libc fix).
Nothing was pushed; GitHub was not touched.
