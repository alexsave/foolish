# Retrospective belief for fusion: what was tried, and why none of it shipped

This is the record of the retrospective-belief work on fusion (2026-10-31 to 2026-11-01).
Seven belief knobs were built as research settings for fusion and measured in paired runs against plain fusion.
None of them gave a proven gain, so the code was not merged: PR #456 was closed, and this document is what remains.
Octogen was never touched: it is strategy-locked, and every knob was invisible to it.

All inference stayed inside the current game: the knobs read only the current game's public log and board, with no memory across games or of a person (Alex's hard rule).

## Where the code is

The code lives only in PR #456 (branch `claude/loving-hamilton-kb7ylc`, deleted after the close; GitHub keeps the commits reachable through the PR).
Its last tip was `1bd9f9461`, on main `64b927c2f`:

- `e9cec86b4`: the knobs and the `fusion_x` research brain (`src/octogen_strategy.c`, `src/octogen_strategy.h`, `src/strategy.h`, `src/bots.c`).
- `1970a4114`: the tests (`test_fusion_told_mod`, `test_fusion_pinned_tell`, `test_fusion_indep_and_keep`, `test_fusion_d_rules`, `test_fusion_d_sound_vs_handwritten`) and two fixes they found.
- `1bd9f9461`: a "Belief research knobs" section in `FUSION.md`.

Before the rebase onto main these were `2dc01adc`, `83b34606` and `7bcd4468`, on the old strong-fusion tip `a413fa12`.

The heart of it is in `og_build_belief`'s chronological log scan (`src/octogen_strategy.c` at `1bd9f9461`, from line 300).
It records the per-seat hand counts, which seats are out, the A tells and the D constraints, with helpers `og_xfer_legal`, `og_pinned_value`, `og_fu_d_tell` and `og_fu_defended` just above it.
The per-world choice is `og_keep_at` and `og_fu_world` (around line 2320), and the knobs are read by `og_fusion_params` (around line 2808).

**That log scan no longer exists on main.**
#453 (BotMemory) replaced it with a public block folded per record, which the belief brains read instead of the log.
Reviving any of this means porting the A, B and D tracking onto BotMemory, not reapplying the diff.
The rebase onto main after #453 was tried and aborted for that reason.

## The knobs

Each knob was a field of `OgFusion`, read only while `og_fu` is set, so octogen could not see any of them.
`FUSION_*` set them for fusion, and `FUSIONX_*` set them for `fusion_x` (`STRAT_FUSION_X` 25, alias `fux`).
`fusion_x` is fusion reading the other prefix: a native-only research row, not seeded, not on the roster or the ladder, and not in wasm.
It existed so a variant could be paired against plain fusion (`--strategy=fusion_x --control=fusion`), which ignores `FUSIONX_*`.
At every default fusion played exactly as without the knobs (see "Byte-identity" below).
Pinned cards are facts: no knob ever dropped one.

The background is octogen's belief.
Octogen infers voids (a seat that picked up holds nothing that beats those cards) and floors (a seat that leads lowest-first holds nothing below its lead).
It applies voids in 3 of 4 sampled worlds and floors in 1 of 2.
When a seat proves itself strategic (`mc_tell`, a pickup while holding a cover), octogen's `OG_ADAPT` drops all of that seat's voids and floors for the rest of the game.

### VOID_KEEP

What it does: the share of sampled worlds, per mille, in which an untold seat's voids apply.
Unset it is 750, octogen's 3 of 4.
How: one schedule for any rate num/den, in which world w keeps the constraint when ceil((w+1) num/den) > ceil(w num/den), so the first n worlds keep exactly ceil(n num/den) (`og_keep_at`).
At 750 it is octogen's void rule world for world.
Why: #451 measured untold seats' voids as only 52-69% true against octogen and fusion tables, so 3 of 4 looked too strong.

### FLOOR_KEEP

What it does: the same for floors.
Unset it is 500, octogen's 1 of 2, and at 500 it is octogen's floor rule world for world.
Why: #451 measured floors as 37-53% wrong against MC tables.

### INDEP

What it does: at 1, each void and each floor of an untold seat is kept or dropped on its own draw per world, at its keep rate, instead of all of a seat's together.
How: the draw is a hash of the world's seed, read from its high bits.
It does not touch the game's or the strategy's random streams.
One fix came out of the tests: a single `bot_mix` of a seed and a small index had made two voids of one seat exactly complementary, so the draw hashes twice.

### TOLD_MOD (B) with A

What TOLD_MOD does: 0 is octogen's behaviour, where a told seat's voids and floors are dropped.
N > 0 keeps them, including those inferred after the tell, and applies them all together in 1 of N worlds.
That draw is one per world, independent of the untold seats' schedule.
Why: #451 found that 53-83% of what `OG_ADAPT` throws away is true.

What A does: at 1, a seat is also marked told (the same `mc_tell` flag, so it goes through B) on either of two public facts:

- it picks up while exactly one attack is uncovered, and one of its pinned cards covers it;
- it covers or picks up while the table holds only uncovered attacks of one value, a one-card transfer was publicly legal, and it holds a pinned card of that value.

"Publicly legal" (`og_xfer_legal`) is the engine's transfer rule on public facts only: the rules allow transfers, no cover is on the table, the next seat still in holds at least attacks + 1 cards by the log's count, and the table has room.
The transfer half rests on the log's hand counts, so it is used only when those counts end on the board's.
A handwritten seat can never trigger either pattern, because it covers whatever it can and transfers whenever it can.
Why A only with B: #451 found A certain and earlier than the tell, but the voids it would drop are mostly true, so a hard drop would cost.

### D1, D2, D4 (D) with D_MOD

D is a set of soft constraints that read the handwritten policy's habits.
Each is decided from public table state, hand counts and the defender's room only.

- D1: a defender covers or picks up where a one-card transfer was publicly legal (A's test), so it holds no card of that value.
  It is not recorded against a pinned card of that value.
- D2: an attacker says good while the defender had room for one more card, so it holds no non-trump of the table's values (a value it holds pinned is left out).
- D4: an attacker plays a lone non-trump card where the defender had room for two, so it holds no second non-trump of that value.

`FUSION_D` is a bitmask: 1 is D1, 2 is D2, 4 is D4, and 7 is all three.
`FUSION_D_MOD` (default 4) makes them apply in (m-1)/m of worlds.
Each constraint lives until that seat's next draw or pickup.
It is dropped when the seat's constraints together leave too few pool cards for its unknown slots, when the seat is told, or when the log's hand counts disagree with the board.
D has its own tell: a seat that plays an unpinned card one of its D constraints forbids is not a handwritten seat, so all its D constraints are dropped for the rest of the game (`d_distrust`).
In 80 random-vs-handwritten_prod games the rules fired 1,425 (D1), 1,529 (D2) and 1,158 (D4) times, and no handwritten hand ever broke one.
Against random seats the D tell fired 3,114 times and A 714 times.
D3 from #451 (cover with the cheapest card) was left out, because it measured only 57-83% true.

## What #451 measured first

#451 (belief sizing, measurement only) scored octogen's and fusion's inferences against the real hands over 24,000 games.
The numbers that motivated each knob:

- Against handwritten tables the belief is exact: 0 wrong voids, 0 wrong floors, 0 tells, 0 A signals, and every D inference 100% true, at every table size.
- Against cordite tables 32-45% of voids are wrong, and against octogen and fusion tables 51-64%.
- On seats the tell never catches (the voids applied in 3 of 4 worlds), voids are still wrong 12-25% at cordite tables and 31-48% at octogen and fusion tables.
- Floors are 37-53% wrong against MC tables, and they are applied in 1 of 2 worlds.
- At least one wrong void or floor is applied at 7.6-12% of decisions at 2 players, 18-28% at 3-4 players, and 36-43% at 6 players.
- Tells are sound but late: 0 false fires in 26,215 tells, with the first tell about 40% of the way through a game, catching 44-70% of MC seats by the end.
- Of the voids `OG_ADAPT` drops after a tell, 53-83% are true.
- A fires 0.8-2.1 (A1) and 0.4-0.9 (A2) times per game at MC tables and never at handwritten ones; it catches 27-59% of MC seats, but the voids it would drop are not mostly wrong.
- D is 100% true against handwritten and 57-94% true against MC: D2 84-94% at 10-77 fires a game, D1 71-91%, D4 80-91%, D3 57-83%.

So the prize looked like this: the void and floor weights are miscalibrated against MC tables, and the tell's remedy throws away mostly-true constraints.

## Byte-identity

Octogen and fusion `cnitro_eval --inspect` transcripts at 2, 4 and 6 players, seeds 200001-200003, against octogen tables were compared with a build of main `64b927c2f`: 36 of 36 were identical.
That covers octogen 9/9, fusion 9/9, fusion with `FUSION_VOID_KEEP=750 FUSION_FLOOR_KEEP=500` 9/9, and `fusion_x` with no `FUSIONX_*` 9/9 against main's fusion.
Fusion with `FUSIONX_*` set was unchanged.
`cnitro_tests` reported 38,868 passed and 0 failed on the rebased branch.
Mutation checks M1-M6 went red on their intended assertions; M7-M23 were never run, since no knob survived.

## Measurements

All runs are `fusion_x` (the knob on) against plain `fusion` on the same deals, `nice -n 5`, 8 processes on disjoint seed ranges.
Diff is fusion_x's mean finishing place minus fusion's, so negative means the knob helped.
The interval is 1.96 times the standard error pooled from the per-worker errors.
Better/worse/same counts the deals where fusion_x finished better, worse or the same.
Win% is the first-place rate, fusion_x then fusion.

### 1. Screening sweep, 4 players (2026-10-31)

3,000 pairs per arm, seeds 12000001-12003000, the same deals for every arm.
Octogen tables unless noted.

| Arm | Env | Tables | Pairs | Diff +/- 95% | Better/worse/same | Win% fusion_x / fusion | Wall clock |
|---|---|---|---|---|---|---|---|
| Void keep 1/2 | `FUSIONX_VOID_KEEP=500` | octogen | 3,000 | +0.004 +/- 0.023 | 220 / 219 / 2,561 | 25.7% / 26.3% | 383 s |
| Void keep 2/3 | `FUSIONX_VOID_KEEP=667` | octogen | 3,000 | +0.010 +/- 0.025 | 236 / 251 / 2,513 | 26.4% / 26.3% | 388 s |
| Floor keep 1/4 | `FUSIONX_FLOOR_KEEP=250` | octogen | 3,000 | +0.007 +/- 0.020 | 163 / 178 / 2,659 | 26.2% / 26.3% | 384 s |
| Floor keep 1/3 | `FUSIONX_FLOOR_KEEP=333` | octogen | 3,000 | +0.007 +/- 0.021 | 198 / 201 / 2,601 | 26.1% / 26.3% | 388 s |
| Independent drop | `FUSIONX_INDEP=1` | octogen | 3,000 | -0.002 +/- 0.030 | 368 / 358 / 2,274 | 26.2% / 26.3% | 392 s |
| B+A, weight 1/4 | `FUSIONX_TOLD_MOD=4 FUSIONX_A=1` | octogen | 3,000 | -0.001 +/- 0.016 | 108 / 96 / 2,796 | 26.6% / 26.3% | 386 s |
| B+A, weight 1/2 | `FUSIONX_TOLD_MOD=2 FUSIONX_A=1` | octogen | 3,000 | +0.016 +/- 0.017 | 116 / 131 / 2,753 | 26.2% / 26.3% | 390 s |
| D1+D2+D4 | `FUSIONX_D=7` | handwritten | 3,000 | -0.016 +/- 0.024 | 326 / 308 / 2,366 | 41.1% / 40.9% | 121 s |

Every interval contains 0.
The keep rates, INDEP and B+A at 1/4 were flat, B+A at 1/2 leaned worse, and D was the only arm worth a closer look.

### 2. Rule D at every table size (2026-11-01)

`FUSIONX_D=7` against fusion, seeds 13000001 + chunk x 100000 + worker x 12500, cheapest cells first, inside a 90-minute measuring budget.
A cell whose interval cleared zero, or came within about one standard error of clearing it, got one extra chunk (marked 1+1).
Per-pair costs were higher than estimated (0.79 s per pair per process at handwritten 6p, 4.2 s at octogen 2p), so most cells stayed well short of +/-0.01.

| Tables | Players | Pairs | Diff +/- 95% | Better/worse/same | Win% fusion_x / fusion | Wall clock | Chunks |
|---|---|---|---|---|---|---|---|
| handwritten | 6 | 7,994 | -0.011 +/- 0.025 | 1,202 / 1,181 / 5,611 | 23.0% / 23.2% | 792 s | 1 |
| handwritten | 5 | 11,039 | +0.007 +/- 0.009 | 433 / 482 / 10,124 | 25.8% / 26.0% | 464 s | 1 |
| handwritten | 4 | 17,599 | -0.013 +/- 0.010 | 1,953 / 1,792 / 13,854 | 41.3% / 40.8% | 713 s | 1+1 |
| handwritten | 3 | 12,000 | -0.008 +/- 0.011 | 1,531 / 1,442 / 9,027 | 70.2% / 69.7% | 731 s | 1+1 |
| handwritten | 2 | 2,000 | -0.006 +/- 0.013 | 90 / 77 / 1,833 | 93.0% / 92.4% | 835 s | 1+1 |
| octogen | 6 | 1,040 | -0.025 +/- 0.101 | 247 / 238 / 555 | 16.6% / 14.8% | 447 s | 1 |
| octogen | 5 | 3,999 | -0.029 +/- 0.023 | 298 / 243 / 3,458 | 19.0% / 18.6% | 684 s | 1+1 |
| octogen | 4 | 2,240 | -0.025 +/- 0.041 | 412 / 374 / 1,454 | 26.2% / 25.2% | 317 s | 1 (qualified for +1, skipped for the budget) |
| octogen | 3 | 1,440 | +0.001 +/- 0.047 | 335 / 340 / 765 | 37.5% / 38.5% | 325 s | 1 |
| octogen | 2 | 320 | +0.041 +/- 0.058 | 39 / 52 / 229 | 59.7% / 63.8% | 347 s | 1 |

Two cells cleared zero on D's side: handwritten 4p (-0.013, interval -0.023 to -0.003) and octogen 5p (-0.029, interval -0.051 to -0.006).
That evidence is weak.
They are 2 of 10 cells, and both were picked for an extra chunk because they already looked good.
Handwritten 5p, the tightest interval of all, sits on the other side (+0.007 +/- 0.009).
Octogen 2p and 3p point the wrong way too, though their intervals are wide.

### 3. Pre-registered confirmation (2026-11-01)

To settle it, a confirmation run was written up on #456 before it was played.
It used fresh seeds 14000001 + chunk x 100000 + worker x 12500 (no overlap with any earlier run), exactly the two clearing cells, fixed sizes, no extra chunks and no early stop.
The decision rule: D is confirmed only if both cells' 95% intervals are clear of zero on D's side; then the BotMemory port becomes a task for Alex's go; otherwise D is documented and deleted.

| Tables | Players | Pairs | Diff +/- 95% | 95% interval | Better/worse/same | Win% fusion_x / fusion | Wall clock |
|---|---|---|---|---|---|---|---|
| handwritten | 4 | 17,599 | -0.0104 +/- 0.0100 | -0.0204 to -0.0004 | 1,924 / 1,822 / 13,853 | 41.9% / 41.4% | 711 s |
| octogen | 5 | 3,999 | -0.0182 +/- 0.0218 | -0.0400 to +0.0036 | 276 / 227 / 3,496 | 19.4% / 19.5% | 644 s |

Handwritten 4p cleared zero by a hair, and octogen 5p did not.
By the rule, D is not confirmed.

## Conclusion

No knob gave a proven gain.
The keep rates, the independent draws and the soft tell with A were flat at 4 players, and B+A at weight 1/2 leaned worse.
D is the only one with a consistent sign: its point estimates favoured it in both confirmation cells and in 7 of the 10 rule-D cells.
But the effect, if real, is around 0.01 places, it did not survive the pre-registered confirmation, and it would cost a port onto BotMemory.
So none of it was ported, #456 was closed unmerged, and fusion's defaults are unchanged.

A likely reason the gap between #451's accuracy numbers and the paired results is so small: a wrong void is applied in only some worlds, it changes the pick only at close decisions, and fusion's rollout noise and shadow race already hedge against a wrong belief.
This is an inference, not a measurement.

D1, D2 and D4 may still be useful elsewhere.
They state, from public facts only, what a naive player's move says about their hand (no card of that value, no non-trump of those values, no second card of that value).
Alex is considering them as human-facing "why" text for move grades, not as a bot feature.
Against handwritten-style play, #451 found them 100% true.

## Reproducing

The research brain and knobs are in #456's commits, which predate BotMemory, so a rerun means building `1bd9f9461` in a detached worktree:

    make -C foolish/c build/cnitro_eval
    FUSIONX_D=7 nice -n 5 foolish/c/build/cnitro_eval --strategy=fusion_x --control=fusion --opp=handwritten --players=4 --games=1100 --seed-start=14000001

Each run prints one paired line per table size: `pc mean_hero mean_ctrl diff+-SE win_hero win_ctrl h<c/h>c/eq`.
