# A5 web — handoff (July 2026)

Branch `a5-web-consumer`, cut from `main` @ `538124e`. Three commits:

| | |
|---|---|
| `cc6bf6a` | the wasm loader's ESM door (a prod break the whole suite was blind to) |
| `74c44e5` | ReplayScreen + Oracle render the kernel's frames |
| `61ab830` | the tutorial too; `view.ts` + `animate.ts` deleted; a real engine bug fixed |

**A5 is done, kernel and web.** Nothing re-derives a game to show it any more.
`docs/C_CORE_CONSOLIDATION.md`'s A5 row is the canonical record; this doc is the
part that does not belong in a table.

---

## What is true now

A replay is the kernel rebuilding the real `Game` from a v6 code and replaying it
through the real engine, serialized as the SAME packed evwire frames live play
broadcasts, decoded by `decodeEventWire` — the client's LIVE decoder.

```
v6 code ──► replay_steps_frames_v6 ──► evwire frames ──► decodeEventWire ──► the screen
              (real engine, real            (the same bytes        (the same decoder
               evwire_walk hooks)            live play sends)       live play uses)
```

- `src/replay/frames.ts` — the web's consumer. Read its header; it explains the
  three things the frames genuinely cannot answer and where each comes from.
- `src/replay/view.ts`, `src/replay/animate.ts` — **deleted** (794 lines).
- `ReplayScreen.tsx` spectates (`viewer: -1`); `Tutorial.tsx` sits in **seat 0**.

### A step is one ACTION, not one log

49 steps where there were 92, on a 3p game. That is what live play broadcasts: a
cover that ends a bout brings its discard and refills with it, in one sequence.
Consequences worth knowing:

- The scrubber has fewer, more meaningful stops.
- INFO steps still map **1:1** to INFO logs, so `moveGaps`/timestamps survive.
- There is no synthetic "end" step — the last step is a real move, so the fool
  line rides *alongside* it.

### The kernel says what each step is

`replay_steps_index_v6` → `replayStepIndex()` → `{ kind, seat }` per step. This
exists because on the wire **an attack and a pass are one event type**, separated
only by a reconstructed English sentence. Matching that prose would be a
projection by the back door.

It deliberately does **not** report per-step log counts. That was the first cut
and it was a lie:

| stream | 3p game |
|---|---|
| the game as **played** (TS logs) | 92 |
| `decodeReplay`'s reconstruction | 79 |
| the engine **replaying** it | 76 |

They disagree on goods (v6 trims all but a trailing one, by design) and on how
draws group. A count against a fourth private stream would only *look* like a
mapping. The Oracle instead pairs the two streams it holds on the moves both call
moves, and **checks every pair** (`moveLogIndices`) — a drift refuses to analyse
rather than analysing the wrong position.

### The reveal eye stopped guessing

It used to retrodict: bind each revealed card back to the oldest face-down slot
that could have held it. Now: replay the code **once per seat** and read that
seat's own hand out of its own frames. Exact by construction, and free — 4
replays land in ~1ms, ~7KB of frames each. The arithmetic decode is the cost and
it is tiny. Do not optimise this into a cache without measuring first.

### Stepping back

No engine un-plays a move. `buildReverseFrames` inverts the *flight* for
presentation only (hand→table becomes table→hand) and commits **the previous
frame's board** — the kernel's, never a rewind computed from the animation.

---

## The engine bug this turned up (read this one)

**When nobody is dealt a trump, the engine ROLLS for the first attacker.**
`determine_lowest_power_index` falls through to `deal_index(num_players)`. That
roll is not in a replay code — the code records the deal, not the RNG state it
was rolled against. So the replay rebuilt the right hands and then picked a
different opening seat, at random, and refused the game with `-REPLAY_EHEADER`.

- ~**1.4%** of 2p deals (12 cards from 36, 9 of them trumps). Rare, not rare
  enough.
- **Flaky**: whether it failed depended on the RNG state at the time.
- The game **encoded and decoded perfectly**. Only its replay was unrenderable.

Fixed with `game_force_first_attacker(seat)` (`game.c`/`game.h`), set by
`rs_play` from the header and consulted **only** on the no-trump branch. Where a
trump *was* dealt the seat is still derived and still checked — that check is
what proves the hands came back, and it keeps its teeth.

Pinned by `test_replay_steps_replays_a_deal_with_no_trump`, which searches for
such a deal and replays it 8× against different RNG states.

**And the error message that hid it:** `REPLAY_EHEADER` (20) was mapped to
`"trump not in alphabet"`, but `replay_steps` raises it for a deal/header
disagreement too. It cost an hour of hunting for a trump bug in the codec that
was not there. Now: `"invalid replay header (trump not in alphabet, or the
rebuilt deal contradicts it)"`. **If you add a use of an existing error code,
fix its message.**

---

## The tutorial

Its code was **v5**, and v5 cannot replay — it hides the deal, so there is no
deck to rebuild. So the game had to change. It is now a different game (seed 37):
new deal, new moves. The narration **beats are derived**, so it re-narrates
itself; nothing was hand-keyed.

`tests/gen_tutorial_game.ts` is **restored** — the old one had been lost, which
is how the tutorial came to be a base32 constant nobody could regenerate and had
to be re-cut by hand when the menu last moved. To re-cut:

```
npx tsx tests/gen_tutorial_game.ts          # prints a code; paste into tutorialGame.ts
```

Nothing else needs editing. It searches seeds for a game where the learner
personally performs every taught move; 172 of 1500 qualified.

**The generator scores the REPLAY'S STEPS, not the played game's logs.** Scoring
the logs is what the first cut did and it lied about exactly the elements that
matter — it reported "the learner says good" about a game where the learner is
never once *asked* to, and reported "the learner leads" by reading
`game.first_attacker` on a **finished** game (a mutated field; the header said
seat 1).

### The seat-less good — the tutorial's one subtlety

A good that **closes** a bout is not attributed to anyone. `apply_round_end`
emits seat `-1`, because the transition belongs to every attacker who had not yet
spoken, and v6 records the round *ending*, not who ended it. Measured: **all** of
the learner's goods are of that kind — a finished game has no pending-good steps
at all. So without handling it, the tutorial would never once ask the learner to
say good.

`learnerOwesGood(prev)` reads it off the board instead: seat 0 was in, was not
defending, and had not spoken, so the good the table waits on is theirs. If you
touch the tutorial's step handling, this is the thing to not break — and
`e2e/tutorial_game.test.ts` asserts the GOOD-step count is **zero**, so the path
cannot quietly become dead code.

---

## Tests, and a warning about them

- **1695** C invariants; **42/42** validate; the replay/tutorial/wasm e2e suites.
- The tutorial has a guard in the **fast** validate runner
  (`e2e/validation/tutorial_validation.test.ts`, same pattern as
  `registerReplayValidation`) so a stranded code fails in seconds.
- Fixtures are **played, not frozen** (`e2e/helpers/seeded_game.ts`). A replay
  code is only readable by the kernel that cut it, so every frozen code rots.
  `oracle_replay.test.ts` was **already red at HEAD, 2 of 5**, because its
  octogen-4v4 fixture had been orphaned ("leftover data after game end") and
  nobody noticed. It now plays its own games: 5/5, 204 decisions across 3 shapes.

**I wrote three toothless tests in this session and only caught them by
mutating.** This is the failure mode here — write the mutation, run it, watch it
go red:

1. A test claiming to catch attack/pass collapsing that never asserted passes
   existed. Collapsing them passed it.
2. The no-trump invariant, which **searched with a different seed derivation than
   it played with** — so it tested a different game and passed against the very
   bug it was written for.
3. (Earlier, same class) `rules_wasm.ts` was a stale embed; validate passed 39/39
   against a kernel that did not have the change under test.

The through-line: **green against the wrong artifact/stream/runtime.** Ask what a
test is actually holding its claim against.

---

## What is left

| | |
|---|---|
| **A8** | now actionable — its gate (guards.wasm's 1-page budget) dissolved when the browser got bots.wasm. Fold the ~960 lines of TS wire mirrors (`@shared/wire/view.ts` 358 + `evwire.ts` 253 + `awire.ts` 170 + `logwire.ts` 181) into the client wasm. |
| **A9** | remaining twins: `replay_v6_parity`'s frozen choreography, `packed_wire_parity` (dies with A8 — its mirrors are production today), `bot_drive_parity`'s TS cycle. |
| **A10** | documented, **not started** — owner said not to start it as a drive-by. |
| **full e2e** | not run this session. Needs the `foolish-e2e-pg` container and no concurrent suites — see `project_e2e_local_postgres`. |

### Traps that cost time here

- **`npm run build` fails at HEAD** on `/_not-found` with `supabaseUrl is
  required` — a missing env var in `.env.production`, **pre-existing and
  unrelated**. "✓ Compiled successfully" + "Finished TypeScript" is the signal
  that matters. I verified this by stashing.
- **`npm run test:e2e` exits 0 with failures.** Trust the `ℹ fail` line.
- **`WASM_CC=/opt/homebrew/opt/llvm/bin/clang`**, not `CC=`. Plain `clang` is
  Apple clang and cannot target wasm32. Rebuild **both**: `make -C cnitro
  wasm-bots` *and* `make -C cnitro wasm` (`game.c` is in both).
- The Bash tool's cwd persists between calls, including after a failed `cd`.
  Absolute paths.
