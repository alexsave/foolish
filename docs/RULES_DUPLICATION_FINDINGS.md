# Rules-duplication & marshal-hazard findings (post replay-kernel port)

Adversarially-verified sweep run after porting the replay codec's rules
projection into the C kernel (`sdk/c/src/replay.c`). Every finding below was
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

## Client second-sources — FIXED in the follow-up pass

All of these were fixed and are now policed by `e2e/attack_cover_parity.test.ts`
(the pass_parity pattern extended to attack/cover; ~140k attack + ~21k cover
checks per run against the kernel validators, mutation-tested to confirm it
catches the original first-attacker bug):

1. **`gameValidation.ts` `canAttack`/`validateAttack`** now enforce the
   first-attacker restriction on an empty table (was: every non-defender got
   a live Attack button the kernel would reject with NOT_FIRST_ATTACKER).
2. **`optimisticConflicts.ts`** no longer applies the attack
   DEFENDER_CAPACITY rule to pending COVER cards (covers are the defender's
   own play; counting them false-reverted legal in-flight covers).
3. **`ServerContext.tsx` optimistic pickup rotation** is now skipped when a
   refill could eliminate a seat (any other IN player with an empty hand);
   in that case the authoritative broadcast supplies the rotation. When no
   elimination is possible the pre-refill rotation is exact and kept.
4. **`AnimationContext.tsx` `canBotMove`** now delegates to
   `shouldBotActCore` (moved into client-safe `common_utils.ts`; re-exported
   from `pure_bot_actions.ts` whose heavy imports must never reach the
   client bundle).
5. **`AnimationContext.tsx` pass-conflict checks** no longer double-count
   the broadcast's attack/pass cards (the final `game_state` already
   contains them); both branches now mirror the kernel capacity rules.
6. **`validatePass`** now mirrors PASS_CAPACITY + the empty-table reject;
   **`validateCover`** now mirrors exact-match-uncovered + duplicate-target
   (the client side of the defender double-tap gap).
7. **`DragContext.tsx`** single-card cover drops are gated on `canCover`.
8. **Cover-mapping logic** consolidated to `coverCombinations.ts`
   (`canCoverCards` delegates; KeyboardPlayMode's local copy deleted).

Still open (deliberate): **Good affordance stricter than kernel**
(`ActionButtons.tsx`): the client only offers Good when everything is
covered; `handle_good` accepts it with uncovered attacks. Kept as-is — the
restrictive affordance reads as intended UX, not drift.

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
  `start_game` moved to `_shared/common/game_lifecycle.ts` (server/tests only);
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
