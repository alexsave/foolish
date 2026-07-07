# Rules-duplication & marshal-hazard findings (post replay-kernel port)

Adversarially-verified sweep run after porting the replay codec's rules
projection into the C kernel (`cnitro/src/replay.c`). Every finding below was
independently confirmed against the code by a second reviewer instructed to
refute it; refuted claims are listed at the end. Items marked **fixed** were
addressed on the same branch as the port.

## The single-source-of-truth picture after the port

The server production path holds the invariant: `actions/*.ts`,
`bot_actions.ts`, `bot_strategy.ts`, `meta_actions.ts` and the replay codec
all delegate rules to the kernel. The sanctioned thin TS projections
(`canCover`, `game_done`, `get_next_player_index`, `shouldBotActCore`) are
policed by `e2e/wasm_engine.test.ts`. The remaining second-sources are in the
**client's UI-affordance / optimistic layer**, which is unpoliced except for
two `canPass` scenarios.

## Confirmed, still open — client second-sources (candidates for follow-up)

Ranked by user-visible consequence. The server always re-validates, so none
of these can corrupt state — they cause phantom buttons, rejected requests,
or optimistic flicker, and they can silently drift when kernel rules change.

1. **`src/utils/gameValidation.ts` `canAttack`/`validateAttack` omit the
   first-attacker restriction** — on an empty table every non-defender gets a
   live Attack button; the kernel rejects with NOT_FIRST_ATTACKER
   (`game.c` `handle_attack`). Phantom affordance, request always fails.
2. **`src/state/optimisticConflicts.ts:86` applies the attack
   DEFENDER_CAPACITY rule to pending COVER cards** — the pending array mixes
   attacks and covers, so legal in-flight covers can be false-reverted
   (flicker) when an unrelated broadcast lands. Covers have no capacity rule
   in the kernel.
3. **`src/contexts/ServerContext.tsx:716` optimistic pickup rotation** runs
   `get_next_player_index` on pre-refill statuses; the kernel rotates *after*
   `refill_player_hands`, which can eliminate seats — the optimistic
   defender/first_attacker can point at a seat the kernel skips.
4. **`src/contexts/AnimationContext.tsx:150` `canBotMove`** is an accidental
   second implementation of `should_bot_act` that ignores `good_players`
   while uncovered attacks remain (spurious poll bumps). Fix: call
   `shouldBotActCore` (already the policed mirror).
5. **`src/contexts/AnimationContext.tsx:474` pass-conflict check
   double-counts** server attack cards (they are already in the last event's
   `game_state`), so a legal in-flight pass can be reverted early.
6. **`src/utils/gameValidation.ts:168` `validatePass`** misses the kernel's
   PASS_CAPACITY and empty-table rejects; **`:188` `validateCover`** misses
   the exact-match-uncovered/duplicate checks (the defender double-tap gap
   the kernel fixed persists client-side within the optimistic window).
7. **`src/contexts/DragContext.tsx:76`** single-card cover drop onto a battle
   skips `canCover` — an illegal drop fires a kernel-rejected request.
8. **Cover-mapping logic exists in three copies** (verified behaviorally
   equivalent today): `src/utils/coverCombinations.ts`,
   `gameValidation.canCoverCards`, `KeyboardPlayMode.findUnambiguousCoverMapping`.
   Consolidate to one before they drift.
9. **Good affordance stricter than kernel** (`ActionButtons.tsx:147`): the
   client only offers Good when everything is covered; `handle_good` accepts
   it with uncovered attacks. Minor missing affordance, deliberate-looking.

Suggested policing: extend the `pass_parity` three-oracle fuzz pattern to
attack/cover (`canAttack`/`canCoverCards`/`validate*` vs `kernelLegalMoves`).

## Confirmed heuristic-layer duplication (deliberate, advisory-only)

- `strategies/move_stats.ts` forward model (gpt strategy) re-implements
  pass-chain legality and a simplified draw model; omits `legal.c`'s
  next-player capacity bound. Belief math only — never gates a move.
- `strategies/pass_prob.ts` is live (imported by move_stats) and correct on
  the rank-budget rule, but unpoliced.
- `offlinefun/localtest/frozen/cordite_core*.ts` are deliberate frozen
  research sims (headers say so); `cordite_core_old.ts` retains the
  pre-settlement `>4 players` deck rule, so 5-player offline baselines from
  it are knowingly off-spec.
- `e2e` drivers and the TS oracle (`e2e/replay_ts_oracle.ts`) inline deck
  rules deliberately as independent test oracles — do not "fix" them to use
  the helpers under test.

## Fixed on this branch

- **Client bundle pulled the wasm embed**: `common_utils.ts` statically
  imported `wasm/engine.ts` for `start_game`/`refillPlayerHandsWithEvents`.
  `start_game` moved to `_shared/game_lifecycle.ts` (server/tests only);
  `refillPlayerHandsWithEvents` (zero callers) and its `kernelRefill` wrapper
  deleted. The client no longer ships the ~77KB base64.
- **`ensureEngineAsync`** could clobber a bots.wasm `__adoptEngine` landing
  mid-flight, and a failed instantiation was unretryable (take-once embed).
  Now adoption-safe and retryable.
- **`kernelGameDone`/`kernelShouldAct`/`kernelNextPlayer`** (test-only
  readers) now clear the resident mark before marshaling, matching the
  documented "every state reader clears the mark" invariant from the
  resident-state bug fix.
- **`ReplayScreen`** reset on `code` change (no stale replay shown).
- **`encode.ts marshalInput`** rejects >65535 actions / >52 pairs explicitly
  instead of silently wrapping counts.
- **`wasm_replay_encode/decode`** clamp `in_len` to the replay buffer.
- **Makefile**: both wasm targets now use blanket header dependencies —
  previously a `replay.h` edit rebuilt rules.wasm but left bots.wasm stale
  (empirically confirmed), shipping divergent kernels.

## Refuted by verification (no action)

- Resident-mark hazard in the three kernel readers is unreachable today
  (test-only callers) — fixed anyway as cheap invariant hygiene.
- Session-log import truncating to the oldest 512 logs: unreachable in
  production (per-move logs start empty; DB session logs are only loaded for
  the replay snapshot, not for bot belief).
- The TS thin projections themselves: sanctioned, parity-policed pattern.
- Kernel replay hard caps (REPLAY_MAX_INT_BYTES, REC_CAP, MENU_CAP...):
  fail-clean hardening; conforming games cannot reach them (the only
  reachable divergence — a >8KB hostile integer — is rejected cleanly).
  Differential fuzz: 11,812 hostile/boundary integers + 121,536 hostile
  encode inputs + 5,320 weird-but-legal games, zero divergence from the
  frozen TS oracle, zero sanitizer reports.
