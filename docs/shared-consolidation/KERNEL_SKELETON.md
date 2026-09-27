# The kernel skeleton - consolidation pass over five products

Investigated in the `shared-consolidation` worktree, read-only.
Products: pickemup/c (`pk_*`), chuiniu/c (`cn_*`), tallybones/c (`tb_*`), and, by role rather than by naming, foolish/c (`anim_plan.{c,h}`, the domain layer generally) and uttt/c (`uttt_anim.{c,h}`, `uttt_say.{c,h}`, `uttt_lang.{c,h}`, `uttt_draw.{c,h}`, `uttt_analyse.{c,h}`).
Scope is the rest of the kernel skeleton beyond the wire primitives (mixrad, b32, sha256, deal_rng, the wasm libc shims) and beyond the message envelope, seat identity, lobby and msg_stage layer, both of which two other investigators are covering in this same pass; this document cross-references but does not re-derive their findings.
Every claim below is a diff, a grep result, or a command's real output, with a file path and, where useful, a line number.

## Verdict

The three new products converged hard on a **naming and shape convention** for this whole skeleton, and that convention is real, deliberate and worth keeping: every file in `chuiniu/c/src/cn_beats.h` and `tallybones/c/src/tb_beats.h` opens with a comment that names `pickemup/c/src/pk_beats.h` (or, one level further back, `uttt`) as the shape its own file follows, and the same citation habit repeats in `cn_plan.h`, `tb_plan.h`, `cn_say.h`, `tb_say.h`, `cn_view.h`, `tb_view.h`, `cn_code.h`, `tb_code.h`, `cn_internal.h` and `tb_internal.h`.
But a shape convention is not shared code, and this pass found only two places where the actual bytes converged rather than the prose: the low-level string utilities inside `pk_say.c` / `cn_say.c` / `tb_say.c` (and, one level further back, `uttt/c/src/uttt_lang.c`), and the test-harness macros inside `pk_check.h` / `cn_check.h` / `tb_check.h`.
Both are genuinely product-neutral, genuinely byte-identical modulo the function-name prefix, and both should move to `shared/c/` now.

The beat clock (S14, pickemup's own "high risk" marking) is the opposite finding.
`pk_beats.c` (1,237 lines), `cn_beats.c` (138 lines) and `tb_beats.c` (269 lines) agree on a **struct shape** - a `Beat` with `start_ms`/`dur_ms`/`ev_i`/`kind`/`ease`/`seat`, a `BeatFrame` with `now_ms`/`next_ms`/`done`, and a `Beats` container of `n`/`total_ms`/`serial`/`start`/`beat[]` - and on a **build strategy**: walk the event list once, accumulate a running clock `t`, and store each beat's absolute `start_ms` into the array at build time.
That build strategy is itself a real, if quiet, convergence: it is not foolish's strategy.
Foolish's `anim_plan.h` (`AnimBeat`, line 565) stores no timing in the beat at all; `anim_plan_at` (`foolish/c/src/anim_plan.c:451`) derives a step's start from its **index alone** (`i * (duration + gap)`), by design, so that a step appended mid-flight cannot disturb an earlier step's timing (`docs/ARCHITECTURE_AS_A_PATTERN.md` lines 199-211).
pk/cn/tb do not do this; all three precompute and store an absolute millisecond per beat, closer to a conventional keyframe list than to foolish's pure function of index.
Past the struct shape and the accumulator strategy, nothing is shared: the three easing systems are unrelated code (pk: a 7-curve cubic-bezier solved by Newton-Raphson plus bisection, `pk_ease` at `pickemup/c/src/pk_beats.c:31`; cn: one hardcoded cubic ease-in-out, `cn_ease` at `chuiniu/c/src/cn_beats.c:5`; tb: two back-out overshoot springs with different constants per beat kind plus a cubic ease-out, `tb_ease` at `tallybones/c/src/tb_beats.c:14`), the beat-kind enums share zero members, and foolish's veil-bitset apparatus (`anim_veil_*`, `foolish/c/src/anim_plan.h:1022-1087`, 64-bit bitsets over dense card ids) and its separate "surface plan" sub-API (`ANIM_SURFACE_*`, `anim_plan.h:1359-1387`, a lobby-to-board fade) have no counterpart anywhere in pk/cn/tb: `grep -rn "veil\|VEIL"` and `grep -rn "surface\|SURFACE"` across all three kernels return nothing except one incidental hit (`pk_beats.h`'s `PK_ANC_BOARD` anchor comment "the lobby fades to it", which pickemup folds into its ordinary beat array rather than giving it a separate module).
So: the three-way evidence answers the brief's question directly - **there is no stable neutral core across pk_beats/cn_beats/tb_beats/anim_plan**, only a converged shape and a converged build strategy, each product's clock is shaped by its own events exactly as much as pickemup's own risk note predicted, and `shared/c/anim_clock.{c,h}` (S14) stays a lift-later item pending a fourth product or an actual second byte-identical function, not a lift-now.

S7 (the drawer-collapse numbers) turned up a genuinely new instance of the defect REUSE_AUDIT already tracks as D2.
Where D2 counted three copies (foolish's Swift, the shared `CollapseSlide.swift` port, and uttt's C port `uttt_collapse_push`), this pass found a **fourth**: `pickemup/c/ios/pk_lay.c:361-372` defines `pk_lay_collapse_push`, byte-identical in formula (including the literal `3.14159265358979` and the "under 0.3%... faded out linearly" comment) to `uttt/c/src/uttt_anim.c:204-215`, with its own constants `PK_LAY_DRAWER_RESPONSE_MS 338` and `PK_LAY_COLLAPSE_MS 600` in `pickemup/c/ios/include/pk_api.h:385-386`, its own comment "uttt's numbers, so the two games collapse alike" and its own pinned test in `pickemup/c/ios/pk_api_smoke.c:210-226`.
Grepping chuiniu and tallybones for the same numbers or function names (`grep -rn "338\|COLLAPSE_MS\|collapse_push\|spring_left\|spring_past"`) finds nothing in either product, which matches - from the C side - what the sister investigation into the iOS Messages layer already found from the Swift side: chuiniu and tallybones never adopted `CollapseSlide` and never ride the collapse animation at all, so they have no reason to hold these numbers.

## Evidence: the beat clock family (S14)

| File | Lines | Beat struct fields | Easing | Build strategy |
| --- | --- | --- | --- | --- |
| `foolish/c/src/anim_plan.h` / `.c` | 1,509 / 1,642 | `AnimBeat` (`:565`): `first`, `n_events`, `type`, `seat`, `flags`, `outs_mask`, `attack_pass_seats`, `placed_ids` (a 64-bit card bitset), `good_mask` - no stored time | veil bitsets (`anim_veil_*`, `:1022-1087`), a separate surface-fade sub-API (`ANIM_SURFACE_*`, `:1359-1387`) | timing derived from index: `anim_plan_at` (`c:451`) |
| `uttt/c/src/uttt_anim.h` / `.c` | 315 / 442 | no beat array at all: `uttt_motion(g, ch) -> UtttMotion`, then `uttt_motion_at(m, now_ms, &frame)` (`:126,129`) samples one motion | none of pk/cn/tb's per-beat curve selection; the collapse spring (`uttt_collapse_push`, `:182`) is a separate concern | one motion at a time, not a list |
| `pickemup/c/src/pk_beats.h` / `.c` | 345 / 1,237 | `PkBeat` (32 bytes, `:170-193`): `start_ms`, `dur_ms`, `part_ms`, `ev_i`, `kind`, `ease`, `from`/`from_i`, `to`/`to_i`, `card`, `flags`, `parts`, `stagger_ms`, `deck_n`, `bulge`, `rot0`/`rot1`, `amp`, `seat`, `sub`, `suit`, `ev_kind` | 7-curve cubic-bezier, Newton-Raphson + bisection fallback, `pk_ease` (`c:31-55`) | cumulative build-time clock; `step_start`/`wait_for`/`fx_start` (`c:290-298`), no `soonest()` helper - the sampler (`pk_beats_frame`, `c:1088`) walks start/mid/end edges inline |
| `chuiniu/c/src/cn_beats.h` / `.c` | 107 / 138 | `CnBeat` (`:43-54`): `start_ms`, `dur_ms` (no `part_ms` split), `ev_i`, `kind`, `seat`, `other`, `q`, `f`, `count`, `round` - no `ease` field at all | one hardcoded cubic ease-in-out applied to every beat, `cn_ease` (`c:5-10`) | cumulative build-time clock via a `static int beat(...)` helper (`c:25-43`) plus a `soonest()` helper (`c:73-76`) |
| `tallybones/c/src/tb_beats.h` / `.c` | 134 / 269 | `TbBeat` (32 bytes, `:44-63`): `start_ms`, `dur_ms`, `part_ms`, `ev_i`, `kind`, `ease`, `seat`, `other`, `cat`, `mask`, `parts`, `stagger_ms`, `sub`, `ev_kind`, `value`, `total`, `dice[]` | two back-out overshoot springs (different constants for SETTLE vs STAMP) plus a cubic ease-out, `tb_ease` (`c:14-24`) | cumulative build-time clock, same shape as cn's, plus a `soonest()` helper (`c:170-173`) byte-identical to cn's |

`soonest()` is the one place two of the three (cn and tb) share literal code:

```
static void soonest(uint32_t *next, uint32_t at, uint32_t now)
{
    if (at > now && at < *next) *next = at;
}
```

`chuiniu/c/src/cn_beats.c:73-76` and `tallybones/c/src/tb_beats.c:170-173` are byte-for-byte this function; `diff` on the two four-line bodies is empty.
Pickemup does not have this function; `pk_beats_frame` (`pickemup/c/src/pk_beats.c:1088-1107`) computes the same "next deadline" quantity inline, comparing `start`/`mid`/`end` against three variables rather than looping a `soonest()` call, so even this one candidate is two-out-of-three rather than three-out-of-three.

`PkBeat`'s field list (`start_ms`, `dur_ms`, `part_ms`, `ev_i`, `kind`, `ease`, ..., `parts`, `stagger_ms`, ..., `sub`, `ev_kind`) and `TbBeat`'s are close enough in field order and naming that tallybones' header comment credits pickemup by name (`tb_beats.h:3-4`); `cn_beats.h`'s comment does too (`:3`) even though `CnBeat` itself dropped `part_ms`, `stagger_ms` and `ease` outright, because Chui Niu's dice-cup animation never needs to sub-divide one beat into staggered parts or choose a curve per beat.
That is the honest shape of the convergence: a **vocabulary of field names that later products borrow selectively**, not a struct any two of the three share unmodified, and not one either shares with foolish's `AnimBeat` or uttt's beat-free `UtttMotion`.

**Proof commands** (current baseline, all read-only, run in this pass):

```
make -C pickemup/c run   # pk_beats_test: 233 assertions, 0 failed (of 5,439,861 total in the suite)
make -C chuiniu/c run    # cn_beats has no standalone test binary; covered inside cn_plan_test (1,737,483 assertions) and cn_fuzz (15,759,233)
make -C tallybones/c run # tb_beats_test: 459,317 assertions, 0 failed
```

All three passed cleanly at the start of this investigation (full counts in the "Proof commands" section at the end); nothing in this pass edited a source file, so these numbers are also the baseline a future S14 lift would have to reproduce unchanged.

## Evidence: x_plan (the event walker)

`pk_plan.h`/`.c`, `cn_plan.h`/`.c`, `tb_plan.h`/`.c` all implement the same documented rule - "the events are the kernel's own apply, observed" - and all three headers cite it as either "pickemup's rule" (cn, `cn_plan.h:3`) or "pk_plan's rule" (tb, `tb_plan.h:3`), one level further back citing "foolish's evwire rule" (pk, `pk_plan.h:7`).
The event struct itself is entirely product content: `PkEvent` is 14 bytes over a 33-member `PK_EV_*` enum (`pk_plan.h:29-81`), `CnEvent` is a different 20-ish byte layout over a 9-member enum with `dice_n[]`/`dice[]` payloads (`cn_plan.h:31-42`), `TbEvent` is 16 bytes over an 11-member enum (`tb_plan.h:36-47`); no two share a field beyond `kind`/`seat`, which is too little to be a struct.

The `.c` bodies are all short (`cn_plan.c` 27 lines, `tb_plan.c` 42 lines, `pk_plan.c` 71 lines) and all follow the same idea - replay through the one apply path with a "sink" attached that records events into a caller's buffer - but the sink-adapter code itself differs: cn passes a `CnSink` struct straight into `cn__apply` (`cn_plan.c:11`), tb wraps a callback through a local `Keep` adapter and `keep_one` (`tb_plan.c:6-11`), and pk (not shown in full here, `pk_plan.c`) follows tb's callback shape more than cn's direct-struct shape.
Nothing here is byte-identical; the functions are too small and too tightly coupled to each product's own `pk__apply`/`cn__apply`/`tb__replay` signatures (see x_internal.h below) to lift as code.
The pattern - "replay once, with an optional sink, so the event list cannot disagree with the state" - is exactly what `docs/ARCHITECTURE_AS_A_PATTERN.md`'s Part 1 piece 3 and the fuzzer of Part 2's Phase 1 already document in prose; there is nothing further to extract into `shared/c/`.

## Evidence: x_say (the caption/string layer) - the strongest lift in this pass

`pk_say.h`/`.c`, `cn_say.h`/`.c`, `tb_say.h`/`.c` are each "which sentence a position says" over that product's own `i18n/keys.h` table, and the high-level captioning (`pk_say_caption`, `cn_say_caption`, `tb_say_caption`, the headline/subline/spoken functions) is entirely product content, correctly so.
But underneath that layer, six low-level string primitives are **byte-identical** (modulo the function-name prefix and one array-size choice) across pk, cn and tb, and five of the six trace back to `uttt/c/src/uttt_lang.c`:

| Function | pickemup | chuiniu | tallybones | uttt (origin) | Identical? |
| --- | --- | --- | --- | --- | --- |
| `next_cp` (UTF-8 decode one codepoint) | `pk_say.c:30-40` | `cn_say.c:29-39` | `tb_say.c:29-39` | `uttt_lang.c:152-162` | yes, byte-for-byte in all four |
| `cp_cols` (column width of a codepoint) | `pk_say.c:43-53` | `cn_say.c:42-52` | `tb_say.c:42-52` | `uttt_lang.c:165-175` (approx.) | yes, byte-for-byte in all four |
| `pk_text_cols`/`cn_text_cols`/`tb_text_cols`/`uttt_text_cols` | `pk_say.c:56-65` | `cn_say.c:55-64` | `tb_say.c:55-64` | `uttt_lang.c:180` | yes, identical body, only the function name differs |
| `pk_itoa`/`cn_itoa`/`tb_itoa` | `pk_say.c:69-80` | `cn_say.c:68-79` | `tb_say.c:68-79` | none (uttt uses `snprintf(num, sizeof num, "%d", n)`, `uttt_say.c:28,40`) | yes across pk/cn/tb; uttt diverges by using libc instead |
| `static int put(...)` (bounded string copy) | `pk_say.c:82-88` | `cn_say.c:117-123` | `tb_say.c:81-87` | not present under this name | yes across pk/cn/tb |
| `pk_fill`/`cn_fill`/`tb_fill`/`uttt_fill` ({placeholder} template filler) | `pk_say.c:90-124` | `cn_say.c:81-115` | `tb_say.c:89-123` | `uttt_lang.c:191` | yes, identical body (34-35 lines) across all four, including the literal `{game}`-is-always-`GAME_NAME` special case |

`diff`-ing the `pk_fill`/`cn_fill`/`tb_fill` bodies directly (with the prefix substituted) produces no output; same for `next_cp`, `cp_cols`, `text_cols` and `put`.
The one real divergence sits one layer up: `append()`, the clause-joiner that composes a caption from its parts, is **not** identical - pickemup's version special-cases a `!`/`?` ending punctuation mark before choosing the join word (`pk_say.c:320-330`), chuiniu's does not (`cn_say.c:167-176`), and tallybones has no `append()` at all (its captioning goes through a different path, `tb_say_of`).
That is the correct boundary: the six primitives above never look at game state or even at the caption grammar, `append()` and everything above it does.

`pk_itoa`'s absence from uttt is a genuine, reasonable divergence rather than a gap: uttt's wasm build links the freestanding libc shim (`shared/c/wasm/`, per `shared/README.md`), so `snprintf` is available and cheap there, while pk/cn/tb's own header comments state a stricter rule - "NO snprintf ANYWHERE ... so the same code runs in a -nostdlib wasm build" (`pk_say.h:12-14`, `cn_say.h:11-13`, `tb_say.h:16-18`) - a rule stricter than the product they credit for the rest of the shape.

**Proof commands:**

```
make -C pickemup/c run   # pk_say_test: 55,609 assertions, 0 failed
make -C chuiniu/c run    # cn_say_test: 7,168 assertions, 0 failed
make -C tallybones/c run # tb_say_test: 344,338 assertions, 0 failed
```

## Evidence: x_view (the masked view)

`pk_view.h` (`PkView`, 62 fields including `PkRevealRow reveal[PK_MAX_SEATS]`), `cn_view.h` (`CnView`, dice counts and a menu of legal raises), `tb_view.h` (`TbView`/`TbCard`, category scorecards - no masking at all, since T9's rule is "everyone sees everything") share zero struct fields and zero function bodies.
`cn_view.c` contains one small helper not present in pk or tb, a five-line ascending insertion sort (`sorted()`, `cn_view.c:5-12`) used to keep a seat's own dice list ordered; grepping pk and tb for an equivalent (`static.*void.*sort`) finds nothing, so this is a single instance, not a duplicate, and not worth lifting on its own.
This matches, from the C side, exactly what `werewolf/COMMON.md` item 4 already wrote about `view.c`/`ww_view.c`: "No line could be shared - the payloads have nothing in common - but the SHAPE should be: one walker used both to write the blob and to measure it... a `viewer` argument with reserved negatives for unmasked and spectator."
pk and cn both follow that reserved-negative convention (`PK_VIEW_SPECTATOR`/`PK_VIEW_ALL` implied by `pk_view.h:64`; `CN_VIEW_SPECTATOR (-1)`/`CN_VIEW_ALL (-2)`, `cn_view.h:19-20`); tb has no viewer parameter at all, because tb has no masking.
Do-not-lift, and this pass found nothing that changes werewolf's own conclusion.

## Evidence: x_code (the history-as-code layer above mixrad)

The mixrad calls themselves (`mixrad_div_mod`, `mixrad_mul_add`) are the other investigator's territory; this pass confirms only that all three `x_code.c` files call directly into `shared/c/mixrad.h` (`pk_code.c:4`, `cn_code.c:3`, `tb_code.c:4`) and nothing else - the shared arithmetic boundary is already correct.
Above that boundary, each `x_code.c` is a per-product menu walker (`pk_legal_turn`'s count and order for pk, `cn_legal`'s call-then-bids order for cn, `tb_menu`'s KEEP/SCORE/LEAVE order for tb) with no shared code: the menus are different sizes, different orders, and the walker in each file reads that product's own legality function directly.
All three headers state the same design rule in near-identical words - "ONE WALKER DOES BOTH DIRECTIONS: the encoder and the decoder run the same function over the same menus" - crediting `uttt/c/src/uttt_code.h` (`pk_code.h:4`) or pickemup (`cn_code.h:3`, `tb_code.h:3`) in turn.
That rule is a documented pattern, not an extractable function; do-not-lift.

## Evidence: x_internal.h (the sink)

All three explicitly name "THE SINK" and credit `pk_internal.h`'s shape (`cn_internal.h:1-5`, `tb_internal.h:1-5`), but the sink struct itself differs every time: `PkSink` carries a callback, a viewer, a from/to range, a running step counter and a per-turn draw counter (`pk_internal.h:13-22`); `CnSink` is a plain `{out, cap, n, on}` struct with no callback (`cn_internal.h:12-17`); `TbSink` carries a callback plus a `bubble` cursor and, uniquely, ties into `tb__roll`'s body-derivation argument (`tb_internal.h:18-37`) because tallybones' dice are derived from the mixed-radix body rather than replayed from stored values.
No two of the three structs share a field beyond `n`/`count`, and the functions that take them (`pk__new`/`pk__apply`/`pk__seal`/`pk__replay` vs `cn__new`/`cn__apply` vs `tb__new`/`tb__step`/`tb__roll`/`tb__replay`) have different arities.
Do-not-lift; the pattern is the reusable part and it is already written down, three times, in near-identical prose.

## Evidence: x_check.h (the test-assertion macros) - the second lift in this pass

The `TEST`/`CHECK`/`report`/`first_fails_of`/`g_checks`/`g_fails` block is close to byte-identical across all three:

```
diff <(sed -n '13,48p' pickemup/c/tests/pk_check.h) <(sed -n '13,48p' chuiniu/c/tests/cn_check.h)
```

produces only: chuiniu drops the `#include <string.h>` pk keeps unused elsewhere in the block, reworks one comment sentence, and sizes `g_named[]`/`g_named_fails[]` at 64 where pickemup also uses 64; tallybones sizes the same arrays at 128 (`tallybones/c/tests/tb_check.h:10-11`) "so a test that goes red after a noisier one is still named" at a slightly higher assertion-failure fan-out.
The `CHECK` macro body (`do { g_checks++; if (!(cond)) { g_fails++; if (first_fails_of(g_test)) { fprintf(stderr, ...); } } } while (0)`), `first_fails_of`, and `report` are otherwise character-for-character the same in all three files.
Everything below that block - `seed_of`/`seed_wide` (deterministic test-only seeding, `pk_check.h:53-70` vs tb's xorshift64* `rnd()`/splitmix64 `seed_wide()`, `tb_check.h:51-70`), `act()`/`mv()` (product move-constructor helpers), and `table()` (a hand-built starting position) - is per-product and correctly so, since it pokes each game's own struct fields directly.

One incidental duplicate surfaced inside this comparison: the splitmix64/xorshift64* multiplier constant `2685821657736338717ull` appears identically in `pickemup/c/src/pk_bot.c:15` (the bot's own PRNG, unrelated to the test harness) and in `chuiniu/c/tests/cn_check.h:56` / `tallybones/c/tests/tb_check.h:56` (the test harness's `rnd()`).
This is the standard published splitmix64 constant, not a coincidence worth a lift on its own, but worth naming so nobody mistakes it for one file's local invention later.

Foolish and uttt have no equivalent: `grep -rln "g_checks\|first_fails_of" foolish/c/tests uttt/c/tests` returns nothing, so this harness was invented (or copied) starting with pickemup and is not a foolish/uttt-rooted convention the way x_say's string helpers are.

**Proof commands** (current baseline):

```
make -C pickemup/c run   # every pk_*_test binary above passed at 0 failed
make -C chuiniu/c run    # every cn_*_test binary above passed at 0 failed
make -C tallybones/c run # every tb_*_test binary above passed at 0 failed
```

## Evidence: x_fuzz.c

`pk_fuzz.c` (110 lines), `cn_fuzz.c` (94 lines), `tb_fuzz.c` (111 lines) share the same driver shape - `argc`/`argv` games count, a `TEST("fuzz")`/`CHECK(...)` loop over random legal actions, a `report(...)` call at the end - and nothing else: every invariant checked (`pk_fuzz.c`'s card conservation over 104 ids, `:19-24` in its header comment; cn's and tb's own game-specific invariants) is unique to that product's rules.
This is exactly the "fuzzer... is your executable spec" pattern `docs/ARCHITECTURE_AS_A_PATTERN.md` names in Phase 1 (Part 2); it is a documented convention, not extractable code.
Do-not-lift.
Baseline: `pk_fuzz` 3,921,125 assertions (2,800 games by default), `cn_fuzz` 15,759,233 assertions (3,000 games), `tb_fuzz` 16,424,832 assertions (700 games by default, invoked with 1,400 in the `make run` target), all 0 failed.

## Evidence: x_beats_dump / x_link_dump - not one family

The task brief groups these by naming, but they are two unrelated tools, and tallybones has neither:

| Tool | Product | Purpose | Line count |
| --- | --- | --- | --- |
| `pk_beats_dump.c` | pickemup | generates Markdown timeline tables for `docs/MOTION_REPORT.md` from synthetic games run through `pk_beats` (`pickemup/c/tests/pk_beats_dump.c:1-6`) | not a test; `make -C pickemup/c beats-dump` |
| `cn_link_dump.c` | chuiniu | decodes a real device's `dev.staged`/`dev.sent` App Group link through the compiled-in iOS bridge, for debugging a live simulator run (`chuiniu/c/tests/cn_link_dump.c:1-10`) | not a test; `make -C chuiniu/c build/cn_link_dump` then run against a captured link |
| (none) | tallybones | - | - |

Confirmed by running `pk_beats_dump`: it printed Markdown tables with real timing (`| 0 | DRAW | flight | 16 | 320 | flight | deck | fan.2 | back | - |`, etc.) exactly as advertised.
Do-not-lift as a family; if anything, `pk_beats_dump`'s "dump real games as Markdown for docs" idea and `cn_link_dump`'s "decode a captured live link" idea are each individually reusable ideas for the other two products, but they are not the same tool and nothing here is shared code today.

## Evidence: i18n/ and the datagen wiring

| Product | Languages | Compiled into the kernel? | i18n README |
| --- | --- | --- | --- |
| foolish | 25 (`c/i18n/strings_*.c`) | no - source of truth for a generator only, outside `c/Makefile`'s `*_SRC` lists (`foolish/c/i18n/keys.h:9-13`) | `foolish/c/i18n/README.md` |
| uttt | 24 + English (`uttt/c/i18n/strings_*.c`), gated by `UTTT_LANG_ENGLISH_ONLY` for the replay-page wasm build | **yes** - "UNLIKE THE SISTER PRODUCT'S TABLE, THIS ONE IS COMPILED INTO THE KERNEL" (`uttt/c/i18n/keys.h:9-10`) | none found |
| pickemup | English only, `pk_i18n/strings_en.c` | yes, following uttt's model | none found |
| chuiniu | English only | yes, "English only for the proof of concept" (`chuiniu/c/i18n/keys.h:7-8`) | none found |
| tallybones | English only | yes, "English only for the proof of concept" (`tallybones/c/i18n/keys.h:15-16`) | none found |

All three new products' `c/Makefile` `datagen:` targets (`pickemup/c/Makefile:186-193`, `chuiniu/c/Makefile:133-140`, `tallybones/c/Makefile:143-150`) are the same recipe with only the prefix changed: two `$(DATAGEN) --cwd i18n --header ... --table ..._KEY_NAME/..._STRINGS_EN --require-complete --swift ...` invocations, each synced into `../ios/Generated/i18n/` with the same `cmp`-then-copy idiom (`sync = mkdir -p $(dir $(2)) && { cmp -s $(1) $(2) || ... }`, identical text in all three Makefiles).
This is the correct shape per `shared/README.md`'s own rule: a product drives the shared `shared/tools/datagen` binary through its own recipe (config), and the recipe itself is boilerplate that REUSE_AUDIT already classifies as "(d) copy and adapt" for `project.yml`-style files.
Nothing here should move into `shared/`: the keys and strings are product content by construction, and the Makefile wiring is the established per-product-config-over-shared-tool pattern already used for `ios_xcframework.mk`, `ship.sh`, and the rig.
The one gap worth naming: none of the three new products has an `i18n/README.md` the way foolish does, and none has grown past one language, so nothing here has yet exercised datagen's `--require-complete` multi-language gate the way foolish's 25-language table does; that is a readiness note, not a defect.

## Evidence: S7, the drawer-collapse numbers, extended

REUSE_AUDIT's defect D2 (section 8) counted three copies of the collapse spring: `foolish/ios/FoolishKit/Messages/CollapseTween.swift` (`hostResponse` 0.338, `slideDuration` 0.6), `shared/swift/MessagesKit/CollapseSlide.swift` (a Swift port), and `uttt/c/src/uttt_anim.c` (`UTTT_COLLAPSE_MS 600`, `UTTT_DRAWER_RESPONSE_MS 338`, `uttt_collapse_push`, `uttt_spring_left`, `uttt_spring_past`).
This pass found a fourth, added after that audit was written:

```
pickemup/c/ios/include/pk_api.h:385:#define PK_LAY_DRAWER_RESPONSE_MS 338
pickemup/c/ios/include/pk_api.h:386:#define PK_LAY_COLLAPSE_MS        600
pickemup/c/ios/pk_lay.c:359: /* ---- the auto-collapse's push (A14): uttt_collapse_push, on this kernel's numbers ---- */
pickemup/c/ios/pk_lay.c:361: float pk_lay_collapse_push(float travel, int t_ms)
```

`pk_lay_collapse_push` (`pk_lay.c:361-372`) is line-for-line the same formula as `uttt_collapse_push` (`uttt/c/src/uttt_anim.c:204-215`): same `w = 2*pi/response`, same `left = (1 + w*t) * exp(-w*t)`, same tail-fade-out comment.
It has its own pinned test in `pickemup/c/ios/pk_api_smoke.c:210-226`, which asserts `PK_LAY_COLLAPSE_MS == 600 && PK_LAY_COLLAPSE_STEPS == 120 && PK_LAY_COLLAPSE_FLIP == 60.0f && PK_LAY_DRAWER_RESPONSE_MS == 338, "uttt's numbers, so the two games collapse alike"` - the pickemup author clearly knew this was uttt's number, and chose to re-derive the C rather than reference uttt's `uttt_anim.h`/`.c` by relative include.
`grep -rn "338\|COLLAPSE_MS\|collapse_push\|spring_left\|spring_past"` across `chuiniu/` and `tallybones/` (C and Swift) returns nothing in either product, confirming - from the C/kernel side - the same absence the iOS-Messages-layer investigation found from the Swift side: neither product rides the collapse animation, so neither needs these numbers.
Doing S7 now (moving `uttt_collapse_push`/`uttt_spring_left`/`uttt_spring_past` and the two constants into `shared/c/collapse/` with a module map, per REUSE_AUDIT's own plan) would retire both the uttt copy and pickemup's new one in the same step, which strengthens the case for doing it now rather than waiting.

## Evidence: general-purpose helper duplication, summary

| Helper | Kind | Copies found | Identical? | Recommendation |
| --- | --- | --- | --- | --- |
| `next_cp`/`cp_cols`/`*_text_cols` | UTF-8 decode + column-width table | uttt, pk, cn, tb (4) | yes, byte-identical | lift now |
| `*_itoa` | integer formatter | pk, cn, tb (3); uttt uses `snprintf` instead | yes among the three | lift now |
| `static int put(...)` | bounded string copy | pk, cn, tb (3) | yes | lift now |
| `*_fill` | bounded {placeholder} template filler | uttt, pk, cn, tb (4) | yes, byte-identical | lift now |
| `soonest(uint32_t*, uint32_t, uint32_t)` | next-deadline-across-N-intervals | cn, tb (2); pk inlines the same idea differently | yes between cn and tb | lift later (needs a third real caller, or pk's own function rewritten to match, before it is worth a header) |
| `TEST`/`CHECK`/`report`/`first_fails_of` | test-assertion macro block | pk, cn, tb (3) | yes, modulo one array-capacity constant | lift now |
| collapse spring (`*_collapse_push`, `*_spring_left`, `*_spring_past`) | critically-damped spring physics | uttt (C), pickemup (C, new), foolish (Swift), shared `CollapseSlide.swift` (Swift) | yes, same formula, across languages | lift now (already S7 in REUSE_AUDIT; now with a fourth copy to retire) |
| splitmix64 multiplier constant `2685821657736338717ull` | PRNG constant | pk_bot.c, cn_check.h, tb_check.h (3) | yes (it is the published splitmix64 constant) | no action - not a local invention, not worth a header for one constant |
| ascending insertion sort (`sorted()`) | small-array sort | cn_view.c only (1) | n/a, single instance | no action |
| `qsort` int comparator (`cmp_i`/`cmp_int`) | trivial comparator | pk_msg_test.c, cn_msg_test.c (2, test-only) | yes, one line | no action - too trivial to be worth a shared header |

No general-purpose bitset, ring buffer or fixed-capacity stack was found duplicated across two or more of these products' skeleton files; the closest candidates (foolish's `anim_veil_*` 64-bit card bitsets, pk_beats.h's `PK_HOLD_*` byte of flag bits) are single-instance and shaped by their own product's data, not a reusable abstraction two products both wrote.
No shuffle other than `deal_rng`'s was found; `pk_shuffle` (`pickemup/c/src/pk_deck.c:33`) is deal_rng-keyed Fisher-Yates and is the wire investigator's territory, not a second implementation.

## Ranked list

**Lift now:**

- `shared/c/say_util.{c,h}` (or a name in that vein, beside `shared/c/i18n/languages.h`): `next_cp`, `cp_cols`, `text_cols`, `itoa`, `put`, `fill`.
  Six functions, all pure string manipulation with zero game or product state, proven byte-identical by `diff` across `pickemup/c/src/pk_say.c`, `chuiniu/c/src/cn_say.c`, `tallybones/c/src/tb_say.c`, and (all but `itoa`) `uttt/c/src/uttt_lang.c`.
  Each product's `#include` stays relative, exactly as `sha256.h`/`deal_rng.h` already work (`shared_headers_reachable_validation.test.ts`'s `SHARED_HEADERS` list needs the new header added); no `module.modulemap` is needed since nothing here is reached from Swift directly (each product's own `pk_say.h`/`cn_say.h`/`tb_say.h` stays the Swift-visible surface, via structgen's bridge, unchanged).
  Proof: `make -C pickemup/c run`, `make -C chuiniu/c run`, `make -C tallybones/c run`, `make -C uttt/c run` all unchanged in assertion counts (baseline above: `pk_say_test` 55,609; `cn_say_test` 7,168; `tb_say_test` 344,338, all 0 failed); a new `shared/c/say_util_test.c` mutation-checked the way `pk_check.h`'s own tests already are.

- `shared/c/test_check.h` (or similar, under `shared/c/` since it is test-only and header-only): the `TEST`/`CHECK`/`report`/`first_fails_of`/`g_checks`/`g_fails`/`g_named[]` block, with the failure-fan-out array capacity taken as a `#define` the includer can override (default 128, tallybones' larger value, since raising it only grows a static array and cannot regress a passing test).
  Proof: same three `make run` invocations; the three products' own `act()`/`table()`/`seed_of()`/`seed_wide()` helpers stay exactly where they are, since they poke each game's own struct.
  Mutation check: break one `CHECK` call in each product's test suite after the move, confirm the same test fails by name, restore.

- Finish REUSE_AUDIT's S7 (`uttt_collapse_push`/`uttt_spring_left`/`uttt_spring_past`, `UTTT_COLLAPSE_MS`, `UTTT_DRAWER_RESPONSE_MS` into `shared/c/collapse/` with a module map on `SWIFT_INCLUDE_PATHS`, the way `shared/c/motion_ruler` already works), and in the same step delete pickemup's `pk_lay_collapse_push`/`PK_LAY_COLLAPSE_MS`/`PK_LAY_DRAWER_RESPONSE_MS` in favor of the shared header.
  This closes REUSE_AUDIT's D2 for real (four copies to one, not three to two) and removes a defect this pass found that the original audit could not have seen, since pickemup wrote its copy after that audit was recorded.
  Proof: `pickemup/c/ios/pk_api_smoke.c:210-226`'s existing pinned assertions must still pass unchanged against the shared function; `foolish/ios/scripts/mac_tests.sh`'s `CollapseTween`/`Driver`/`Curve`/`Layer`/`RulerTests` and `CompactRestHeightTests` (named in REUSE_AUDIT S7) must stay green if foolish's own `CollapseTween.swift` is switched to read the shared header, which is optional in this step and can be deferred; `make -C uttt/c run` and `make -C pickemup/c run` must show identical assertion counts before and after.

**Lift later:**

- `soonest()` as a tiny shared header, once a third real (non-inlined) caller exists, or once pickemup's own `pk_beats_frame` is touched for an unrelated reason and can be rewritten to call it instead of its inline three-way comparison.
  Two copies (cn, tb) plus one inline reimplementation (pk) is thin enough to defer; forcing pk onto the shared shape today is a pk_beats.c behavior-preserving refactor that needs its own P1-P4-equivalent proof pass on a file already marked high risk (S14), and the payoff (four lines) does not carry that cost today.

- A **prose** writeup (not code) of the beat-clock convention - the `Beat`/`BeatFrame`/`Beats` field-naming vocabulary and the cumulative-build-time-clock strategy pk/cn/tb converged on independently of foolish - belongs somewhere durable (this document, or a short addendum to `shared/README.md`'s C table under a note that no file exists yet), so the next product that builds an `xx_beats.h` starts from the vocabulary instead of re-deriving it, without anyone being tempted to extract a `shared/c/anim_clock.h` that the evidence does not yet support.

- Revisit `shared/c/anim_clock.{c,h}` (S14 itself) only when a fourth product's beat clock either (a) reuses one of pk/cn/tb's actual easing functions rather than inventing a new one, or (b) needs the veil-bitset or surface-plan machinery foolish built and none of pk/cn/tb have needed yet.
  Until then this is a real "do not lift" backed by the diffs above, not a deferral for its own sake.

**Do not lift:**

- `x_beats.{c,h}` end to end - no code survives a diff across all three; see the S14 evidence above.
- `x_plan.{c,h}` - the sink-adapter bodies are too small and too coupled to each product's own `apply`/`replay` signatures; the pattern is already documented in `docs/ARCHITECTURE_AS_A_PATTERN.md`.
- `x_view.{c,h}` - zero shared struct fields, zero shared functions; matches `werewolf/COMMON.md` item 4's own conclusion about `view.c`/`ww_view.c` exactly.
- `x_code.{c,h}` beyond the mixrad boundary already shared - the menu walker is per-product by construction; the "one walker, both directions" rule is a documented pattern, not extractable code.
- `x_internal.h` (the sink struct) - three different struct shapes behind one shared name and one shared design sentence; nothing to extract.
- `x_fuzz.c` - the driver loop shape is shared in spirit and documented in `docs/ARCHITECTURE_AS_A_PATTERN.md` Phase 1; every invariant it checks is game rules.
- `x_beats_dump` / `x_link_dump` - not one family; two tools with different purposes, one product (tallybones) has neither, nothing to lift.
- i18n content (`keys.h`, `strings_en.c`) and the per-product `datagen:` Makefile targets - content is product-specific by construction, and the Makefile wiring is already the correct shared-tool-plus-per-product-config shape (REUSE_AUDIT class "(d)").
- The splitmix64 constant, the single-instance `cn_view.c` insertion sort, and the test-only one-line `qsort` comparators - real but too small to be worth a shared header on their own.
