# Handoff: stale-intent actions across round boundaries (web) + race regression suite

*Investigation handoff for a suspected live bug on foolish.cards, plus the spec
for a permanent concurrency regression suite. Self-contained: assumes no prior
context beyond this repo. Verified anchors as of 2026-07-13 (branch
`claude/monetization-roadmap-algsai`, rebased on main @ `4db9531`).*

---

## 1. The reported symptom

User report (owner, from live play): when a defender's **pickup** and another
player's **attack (throw-in)** race each other, the attacker's client plays a
**revert animation** (card slides back to hand) — and then the same attack
**plays anyway** right after.

## 2. Why this is almost certainly real (mechanism, with anchors)

The server serializes all mutations through an optimistic-concurrency commit
(`commit_game`, CAS on `games.version` — see `supabase/migrations/`,
README "How the game runs"). But **serialization is not intent-preservation**:

1. Attacker's client POSTs `attack(9♣)` to the `action` edge function
   (binary packed body — `src/contexts/ServerContext.tsx:1180`). The request
   is in flight.
2. Defender's `pickup` commits first. In this engine **pickup closes the round
   immediately** — takes the whole table, refills hands, rotates
   `first_attacker`/`defender` (`sdk/c/src/game.c:776-808`, `handle_pickup`).
3. The attacker's client receives the pickup broadcast and correctly reverts
   the optimistic 9♣ (revert machinery: `src/state/optimisticConflicts.ts` —
   table-clear sweep and DEFENDER_CAPACITY paths at lines 60-105).
4. The in-flight `attack` request now executes against the **post-pickup
   state**. Validation is performed against the *current* state only —
   `validateAttack` → `kernelValidateAttack(game, …)`
   (`supabase/functions/_shared/common/actions/attack.ts:13-19`); **nothing in the
   request says which state the player composed the move against** (the
   `version` in `packed_action.ts` is the response/cache version, not a
   client-intent field).
5. If the attacker happens to be the new round's eligible attacker and 9♣ is a
   legal *opening attack of round N+1*, the kernel accepts it. It IS legal —
   it's just **not what the player chose to do**: they threw a card into last
   round's battle, and it became a brand-new attack.
6. The accepted attack broadcasts back → the client animates it → "revert,
   then it plays anyway."

The same class of bug exists for other intents that survive a phase change by
coincidence (e.g., a `pass` composed against a table that has since been
swept, re-validating as a different pass; a `good` racing a round transition).

This is exactly the failure mode the iMessage design guards against with its
**round-boundary rule** (`docs/IMESSAGE_GAME_DESIGN.md` §7.4-§7.5): *an action
must never survive re-validation across a round closure*. The web/server needs
the same guard.

## 3. What is NOT the bug

- The server is not corrupting state; every applied action is kernel-legal.
- The optimistic revert machinery appears to behave correctly (it reverts).
  The ghost replay comes from the **server accepting the stale-intent
  request**, not from the client re-merging reverted cards. (Verify this —
  see §4 — but do not start by "fixing" `optimisticConflicts.ts`.)
- Full CAS-on-client-version would NOT be a fix — it would reject *legitimate*
  concurrency (throw-ins while the defender thinks are legal cross-version
  applies and must keep working). The guard must be **round-scoped, not
  version-scoped**.

## 4. Investigation plan (do this before writing the fix)

1. **Reproduce in e2e with injected latency.** The harness already exists:
   commit `43753c2` added `E2E_BCAST_LATENCY_MS` / per-POST latency injection
   and `e2e/bench_broadcast.ts` drives the real broadcast path. Write a test
   that: creates a 2p game, brings it to "defender deciding" state, dispatches
   `pickup` and `attack` with the attack's POST delayed until after pickup
   commits, then asserts on the resulting action log.
   **Expected today (bug): the attack is accepted as round N+1's opening
   attack.**
2. Instrument which state version the attack validated against (the action
   handler logs / `packed_action.ts` cache read at `:53-65`).
3. Confirm the client-side story on top: with the same latency injection in a
   browser (or the reconcile unit tests in `src/state/clientReconcile.ts` —
   deliberately pure and unit-testable), verify the revert fires from the
   pickup broadcast and the subsequent attack broadcast is the server echo,
   not a client resend. If a client resend/retry path exists after all, that
   is a second bug — find it before fixing either.

## 5. Fix design (proposed — validate against findings first)

**Wire a client-intent round counter through the action path:**

1. Add `intent_round` (u8/u16) to the packed action request
   (`supabase/functions/_shared/packed_action.ts` encode + decode; bump the
   packed format version — there is a `fmt` byte in the response already,
   `supabase/functions/action/index.ts:17`; mirror versioning on the request).
   The client sets it from its current authoritative state's completed-round
   count. Add the same counter to the game state the client tracks (derivable:
   count of round closures — discard/pickup events — in the animation feed;
   the kernel knows it authoritatively, consider exporting it in views via
   `player_views.ts` so the client doesn't hand-count).
2. In the `action` edge function, before kernel validation: if
   `game.completed_rounds > intent_round` → **reject** with a new reject code
   `REJECT_STALE_ROUND` (wire it into the `[fmt | status | reject_code |
   u32 version]` response, `action/index.ts:17`). Same-round concurrency
   (throw-ins, covers, goods) still validates purely by kernel legality —
   unchanged behavior.
3. Client on `REJECT_STALE_ROUND`: keep the revert (already happened), show
   the small toast the iMessage doc words as: "«Sveta» picked up before your
   9♣ landed." No retry, no resend. Strings: en/ru/ko in
   `src/localization/strings.ts`.
4. **Compatibility:** old clients that don't send `intent_round` (missing
   field / old fmt) must keep working during rollout — treat absent as
   "no guard" (today's behavior), then tighten once clients are deployed.
   Bots (`bot_actions.ts`) compose against fresh server state under the lease
   loop and can send the real counter trivially.
5. Do NOT change kernel legality (`sdk/c/src/game.c` / `legal.c`) — the
   kernel is shared with replay/iMessage and its semantics are correct. The
   guard is a server-edge policy on *intent*, and belongs in the action
   handler layer.

## 6. The regression suite: seven races as web e2e tests

Port of `docs/IMESSAGE_GAME_DESIGN.md` §14 to the server-authoritative world.
New file `e2e/race_conditions.test.ts`, using the real edge-function modules +
real Postgres like the rest of `e2e/` (see `e2e/README.md`), with per-POST
latency injection to force each interleaving deterministically. For each case,
run BOTH delivery orders.

| # | Race | Expected after fix |
| --- | --- | --- |
| 1 | defender `pickup` ∥ attacker throw-in (the reported bug) | pickup commits; delayed attack **rejected `REJECT_STALE_ROUND`**; action log contains no round-N+1 attack; client-side (reconcile unit layer) shows one revert and no ghost replay |
| 2 | two attackers throw in simultaneously (3p) | both accepted if defender capacity allows (kernel `legal.c` capacity rule); if the first consumes the last slot, second rejected with capacity reject code — **not** stale-round; client reverts cleanly (`optimisticConflicts.ts` DEFENDER_CAPACITY path already covers this — keep its unit tests green) |
| 3 | defender covers battle 1 ∥ attacker adds battle 2 | both accepted in either order; no reverts, no rejects |
| 4 | two attackers send `good` ∥ each other (3p) | first accepted; second is a same-round no-op/duplicate (kernel `good_players_mask`) — assert no error surfaced to user and no double round-transition |
| 5 | two players join the last open seat ∥ each other (lobby) | one seat granted; loser gets clean reject + lobby refresh (this is `create`/`meta` path, not `action`) |
| 6 | action ∥ game-ending move | delayed action rejected (game over guard already exists — `executeAttack` GAME_OVER check, `_shared/common/actions/attack.ts:24-26`, plus stale-round); assert no post-terminal mutation and broadcasts stop cleanly |
| 7 | same player, two tabs, both act on the same state | second tab's cross-round action rejected `REJECT_STALE_ROUND`; same-round duplicate follows kernel legality (e.g., attacking the same card twice → normal reject); both tabs converge via the version gate (`shouldDropStaleSequence`, `src/state/clientReconcile.ts:44-52`) |

Suite invariants (assert in every case): final state is kernel-legal and
identical across both delivery orders **except** where the table explicitly
says order decides (cases 1, 2); `games.version` strictly increases; every
reject maps to a named reject code; no action appears in logs that no client
intended in that round.

## 7. Acceptance criteria

- [ ] Repro test demonstrates today's behavior (red) before the fix lands.
- [ ] `REJECT_STALE_ROUND` implemented per §5 with backward compatibility.
- [ ] All seven regression cases green in both delivery orders.
- [ ] Existing suites untouched and green: `npm run test:e2e`,
      `npm run test:validate` (validation suite mirrors hand-rolled-logic
      guards — see commits `4f33a4a`, `c04ffa1`).
- [ ] Reconcile/optimistic unit tests extended, still pure (no React), per the
      "no second copy" rule at the top of `src/state/clientReconcile.ts`.
- [ ] Localized toast strings (en/ru/ko) for stale-round rejects.
- [ ] `docs/IMESSAGE_GAME_DESIGN.md` §14 cross-referenced: the iMessage rebase
      guard and this server guard are the same rule in two enforcement points —
      note that in both docs so future rules changes update them together.

## 8. Out of scope

- No kernel/rules changes; no replay codec changes.
- No optimistic-overlay redesign — only additions needed for the toast.
- No client retry/backoff work.
- The iMessage implementation itself (separate doc/branch).
