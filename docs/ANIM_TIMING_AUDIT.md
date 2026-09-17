# The web's animation timing and ordering, decision by decision

Phase 9 step 1 of `docs/C_GAME_SHAPE_MIGRATION.md` section 7.
Written before any code changes, so that what moves to C in step 2 is a list and not a guess.

Every row below names the code that decides something about WHEN a card moves, HOW LONG it moves for, or IN WHAT ORDER things happen, and then says one of three things:

- **C answers it** - a function or struct in `c/src/anim_plan.h` already returns this, and the web either ignores it or re-derives it.
- **C answers it partly** - the rule is there but an input, an output or a re-ask is missing.
- **C has no concept of it** - nothing in the kernel is about this question at all.

`anim_plan.h`'s own header sets the boundary this audit measures against: plan building, optimistic policy, timing policy and the conflict model are C; interpolation, view updates, screen coordinates and gesture previews are the platform's.
Its second paragraph sets the direction of travel: iMessage is the spec, so where an iMessage rule and a web rule disagree the iMessage one is the answer and the web is the client that re-derives.

## 0. The shape of the problem, in one paragraph

The web has no animation model.
It has a queue of events (`animationQueueRef`, `src/contexts/AnimationContext.tsx:168`), one `setTimeout` at a time (`src/contexts/AnimationContext.tsx:1196`), and a single constant (`ANIMATION_TIME = 500`, `src/constants/constants.ts:2`) standing in for every duration, every gap and every deadline in the product.
Each timer's callback commits that step's board, clears the flight, decrements a counter and calls itself for the next step (`src/contexts/AnimationContext.tsx:1196-1247`).
Nothing anywhere holds "what is the current step at time T" as a value, so a push that lands mid-flight cannot be answered by re-asking; it can only be answered by mutating the queue the timer chain is walking, which is what the four insertion branches at `src/contexts/AnimationContext.tsx:1074-1112` do.
The file's own TODO says this (`src/contexts/AnimationContext.tsx:1116-1119`): "this serial setTimeout-driven event queue is a React workaround ... It should be replaced with a proper animation model".

## 1. Step order and step durations

| Decision | Web code | C today |
| --- | --- | --- |
| A step lasts 500 ms | `src/constants/constants.ts:2`, read at `src/contexts/AnimationContext.tsx:1247` | **C answers it.** `ANIM_TIME_MS` (`c/src/anim_plan.h:74`) is the same 500, and `anim_step_duration_ms` (`:329`) is the seam a per-type rule would land in. The web imports the TS constant instead. |
| The CSS transition lasts 500 ms | `src/components/GameDisplay/AnimationOverlay.tsx:580` | **C answers it.** Same constant. This one is rendering (a CSS string), so it may keep reading a number, but the number must come from the kernel. |
| The gap between two steps is 0 ms | `src/contexts/AnimationContext.tsx:1242-1246`: the next flight starts in the same commit as the previous one lands | **C answers it, and differently.** `ANIM_GAP_MS` is 25 (`c/src/anim_plan.h:75`), and `AnimPlanStep.start_ms` is documented as `i*(ANIM_TIME_MS+ANIM_GAP_MS)` (`:271`). The web's effective gap is zero. This is a real, intentional divergence the gate must justify frame by frame. |
| Step i starts at i x (duration + gap) | nowhere: the web has no start time, only "when the previous timer fired" | **C answers it.** `AnimPlanStep.start_ms` (`c/src/anim_plan.h:271`). |
| The whole sequence's length | nowhere | **C answers it.** `AnimPlan.total_ms` (`c/src/anim_plan.h:288`). |
| One event per step, in wire order | `src/contexts/AnimationContext.tsx:169-173` (`enqueue` appends), `:1144` (shift) | **C answers it better.** `anim_build_beats` (`c/src/anim_plan.h:447`) merges consecutive covers by the same seat into one beat, because the kernel spends one COVER event per card and a defender who covered two attacks in one move must fly as one movement. The web animates them as two 500 ms steps. |
| The replay's initial deal waits 400 ms | `src/components/ReplayScreen.tsx:684` | **C has no concept of it.** Nothing in `anim_plan.h` is about a cold open's lead-in. |
| Autoplay's inter-step delay: 250 ms default, recorded gap clamped to 150..3000 in AUTO, gap/mult with a 30 ms floor otherwise | `src/components/ReplayScreen.tsx:693-712` | **C has no concept of it.** This is a transport dial over recorded wall-clock gaps, not choreography; it is the replay screen's own product feature and should stay in React. |
| The autoplay ticker polls at 250 ms | `src/components/ReplayScreen.tsx:725` | **C has no concept of it**, and does not need one: this is the frame loop, which the boundary leaves to the platform. |
| The speed stops (`AUTO`, powers of ten around the median gap) | `src/components/ReplayScreen.tsx:121-149` | **C has no concept of it.** Pure presentation of recorded times; stays in React (moves to `src/replay/speeds.ts` in step 4). |

## 2. The count-freeze

`anim_plan.h` is explicit that the freeze is the iMessage rule and not the web's (`c/src/anim_plan.h:34`).

| Decision | Web code | C today |
| --- | --- | --- |
| The deck/discard/hand badges hold at their pre-move values until each flight lands | **the web does not do this.** It commits each step's own board as that step's timer fires (`src/contexts/AnimationContext.tsx:1200-1209`), so the badges advance per step but never freeze for the sequence, and the FIRST step's board is on screen from frame zero. | **C answers it.** `AnimCounts` (`c/src/anim_plan.h:219-261`) is the whole freeze: deck, discard, per-seat hand, the battle row, `paired`, and the flipped trump. `AnimPlan.pre` carries it (`:286`). |
| The deck badge drops as cards leave the deck, not as they land | `src/contexts/AnimationContext.tsx:152-153` (`inFlightFromDeck`, `inFlightToFlipped`), set at `:1165-1171`, cleared at `:1214-1215`, consumed at `src/components/GameDisplay/DeckAndFlipped.tsx:10-18` through `clientTable().rules(view, fromDeck, toFlipped)` | **C answers it partly.** `AnimPlanStep.in_flight_from_deck` / `in_flight_to_flipped` (`c/src/anim_plan.h:279-280`) are exactly these two numbers, per step, and `ViewRules` already takes them as arguments. The web computes them itself from `from_location === 'deck'` and `to_location === 'flipped'` instead of reading the plan. |
| A card bound for the flipped slot does not reduce the badge | `src/contexts/AnimationContext.tsx:1167` | **C answers it.** `in_flight_to_flipped`, same lines, plus `AnimCounts.flipped` carrying the identity (`c/src/anim_plan.h:260`) for the "the trump does not show in the pile before the deal plays" rule the owner stated at 1.1(55). |
| The battle row holds at its pre-move arrangement | **the web does not do this**, and this is the exact defect `anim_plan.h:207-218` describes ("a card 36pt to the side of where it belonged on the very first painted frame"). The web has no pre-move row; `AnimationOverlay` reads `currentAnimation.game_state ?? game` (`src/components/GameDisplay/AnimationOverlay.tsx:270`). | **C answers it.** `AnimCounts.battles` / `n_battles` / `paired`, derived by `anim_pre_stream_table` (`c/src/anim_plan.h:575`) for additions and `anim_pre_bout_table` (`:544`) for sweeps. |
| The badge of the acting seat drops as cards LEAVE rather than as they land | **the web does not do this** for hands; only the deck gets the treatment. | **C answers it.** `ANIM_BEAT_DROPS` (`c/src/anim_plan.h:423`) and `anim_badge_drops_as_cards_leave` (`:452`). |

## 3. The veil

| Decision | Web code | C today |
| --- | --- | --- |
| A flying card is hidden at the place it left and the place it lands on | `flightPlaces` (`src/contexts/AnimationContext.tsx:46-54`), the `animatingCards` map (`:155-161`), written at `:1176-1193`, cleared at `:1218-1225`, read by `getCardAnimationState` (`:1263-1284`) and honoured at `src/components/GameDisplay/CardFace.tsx:62,113` | **C answers it partly.** The veil sets are all there (`anim_veil_veiled`, `anim_veil_flying`, `anim_veil_grid`, `anim_veil_fan`, `anim_veil_hand_slot_deferred`, `anim_veil_sweep_unplaced`, `anim_veil_teardown`, `anim_veil_handover`, `c/src/anim_plan.h:863-922`) and `AnimPlan.veil_ids` (`:293`) names every identity a sequence brings into being. What C does NOT have is the web's per-PLACE question: the web hides a card at a place key (`seat`, `'table'`, `'flipped'`), and the kernel's sets are per identity. `flightPlaces` is the only genuinely new rule here, and it is a coordinate rule, so it is rendering. |
| A masked back is not veiled | `src/state/pushSequence.ts` passes cards through; the web never checks `mask_cards` | **C answers it.** `AnimEvent.mask_cards` (`c/src/anim_plan.h:198`) and the veil's exclusion of masked cards. |
| Which cards a replay the board has not started yet must hide | **the web does not do this**; the replay screen publishes a sequence and the board draws whatever the feed committed. | **C answers it.** `anim_veil_unstarted_replay` (`c/src/anim_plan.h:928`). |

## 4. The per-move-type branches in the animation path

Every one of these is a `type === '...'` test taken by the timing or ordering code, not by a renderer.

| Site | What it decides | C today |
| --- | --- | --- |
| `src/contexts/AnimationContext.tsx:417-431` | which optimistic entries are attack/cover (hand to table) and which are pickups (table to hand) | **C answers it.** `anim_is_placement` (`c/src/anim_plan.h:947`), `anim_conflict_dest` (`:790`). |
| `src/contexts/AnimationContext.tsx:441` | `serverAttackPasses`, the gate for the whole optimistic-pass branch | **C answers it partly.** `AnimBeat.attack_pass_seats` (`c/src/anim_plan.h:432`) says which seats laid cards via ATTACK_PASS, and `anim_pass_hand_off` (`:482`) says whether that was a transfer. The web instead infers "a pass happened" from the defender changing between two boards (`:520-524`). |
| `src/contexts/AnimationContext.tsx:684`, `:928` | "does this message contain MY pass" | **C answers it.** `anim_pass_hand_off` with `attack_pass_seats` and the final defender. |
| `src/contexts/AnimationContext.tsx:974-975`, `:1021`, `:1062-1069` | finds the pickup / cards_to_trash / magic_transition events, to decide where revert events go in the queue | **C answers it.** `anim_conflict_sweep` (`c/src/anim_plan.h:747`) exists precisely so that "pickup or trash" is stated once; the kernel already refuses to let a caller restate it, and this file restates it three times. |
| `src/contexts/AnimationContext.tsx:1015`, `:1057-1059` | finds the first server attack from hand, to decide the revert insertion point | **C has no concept of it** as an ordering question. See section 9. |
| `src/contexts/AnimationContext.tsx:1167` | `to_location === 'flipped'` | **C answers it.** `in_flight_to_flipped`. |
| `src/contexts/AnimationContext.tsx:1228` | `type === 'revert'` clears the reverting/position tracking | **C answers it partly.** `ANIM_EVT_REVERT` (`c/src/anim_plan.h:91`) is defined as a client-only type with no evwire byte, so the kernel knows the type exists; nothing in C decides what tracking a revert's landing releases. |
| `src/components/GameDisplay/AnimationOverlay.tsx:117`, `:274`, `:354`, `:371`, `:420`, `:477`, `:490` | which screen rect a flight starts and ends at | **C has no concept of it, correctly.** These are coordinates. They stay. |

## 5. `optimisticPassState` and its four reconciliation sites

Declared at `src/contexts/AnimationContext.tsx:209` as `{ defender, first_attacker }`, set only in `pass` (`:1460-1466`) from the kernel's own optimistic board, cleared on refusal (`:1475`).
The four places that reconcile it:

1. **`src/contexts/AnimationContext.tsx:445-515`** - the early pass-conflict branch.
   It collects my still-pending pass cards by re-parsing the optimistic map's JSON keys, asks the kernel for a verdict against the NEXT defender's hand, and on any revert sets `passIsInvalid`, clears `optimisticPassState`, and splices those cards out of `myOptimisticAttackCovers`.
   **C answers the verdict** (`resolveConflictMotions` already delegates, `src/state/optimisticConflicts.ts:72-86`).
   **C has no concept of** the guess itself: the whole variable exists because the web wants to know the shield's next position before the server says so, and `client_optimistic_apply` already computes exactly that board (`sdk/ts/table/client_table.ts:230-236`, used at `:1460`).
   The pending state is a cached copy of a board the kernel will happily recompute.
2. **`src/contexts/AnimationContext.tsx:699-701`** - inside `keep`, the board editor applied to every event's board and the final board when cards are merged.
   It calls `turnedBoard(next, first_attacker, defender)` before `keepPending`.
   **C answers it.** `turnedBoard` is a `clientTable().edit` call; the sequence of edits is the only thing TS decides.
3. **`src/contexts/AnimationContext.tsx:933-949`** - the completion callback.
   If the message carries my pass and the server's final board agrees on both the defender and the first attacker, the guess is dropped; if it disagrees, the guess is imposed on the server's board; if the message is not my pass, the guess is dropped.
   **C answers it partly.** `anim_pass_hand_off` says whether a stream hands the shield over and to whom, which is the same comparison; nothing in C decides "trust my guess over the server's board".
4. **`src/contexts/AnimationContext.tsx:1205-1207`** - the per-step commit inside the timer.
   Every landing step's board gets the guess re-imposed, unconditionally, with no check that this message is mine.
   This is the site that makes the guess a timing decision rather than a state decision: which board a step commits depends on a ref that some other code path may have cleared between two timers.
   **C answers it partly**, as above.

The dedup key that would replace the JSON-string map is already in C: `anim_event_key` packs `(type, card, from, to, seat)` into a u64 (`c/src/anim_plan.h:587`), and its header says in as many words that the seat replaces the uuid because a plan is per viewer.
The web instead keys on `JSON.stringify` (`createCardEventString`, `src/utils/animationUtils.ts`) and parses that string back out in five places (`:414`, `:452`, `:826`, plus `src/state/optimisticAnimation.ts:41`).

## 6. Optimistic TIMING, as opposed to optimistic policy

This is the gap the plan's step 1 singles out, and the audit confirms it: the POLICY is in C and the TIMING is not.

| Decision | Web code | C today |
| --- | --- | --- |
| A predicted flight starts immediately on tap, before the request is even validated | `src/contexts/AnimationContext.tsx:1322-1323` (`queueAnimation` from `triggerOptimisticAnimation`), reached from `attack` `:1373`, `pass` `:1453`, `pickup` `:1544`, `cover` `:1654` | **C has no concept of it.** Nothing in `anim_plan.h` says when a predicted motion begins. |
| A predicted flight lasts 500 ms, like a confirmed one | the same single `setTimeout` at `src/contexts/AnimationContext.tsx:1196` serves both | **C answers it only by accident**: `anim_step_duration_ms` takes an `ANIM_EVT_*`, and a prediction has the same type as its confirmation, so the answer is the same. There is no way to ask "how long does a PREDICTED attack fly for". |
| The board the move leaves appears after 500 ms | `src/contexts/ServerContext.tsx:710-716`: `setTimeout(..., ANIMATION_TIME)` around `optimisticBoard` | **C has no concept of it.** This is a second, independent timer on the same constant, in a different file, with its own validity thunk (`applyOptimistic`, `src/contexts/ServerContext.tsx:703`). It is not coupled to the flight it is meant to follow; if the queue is busy the board lands while some other card is still flying. |
| A confirming push replaces the prediction mid-flight | `src/contexts/AnimationContext.tsx:862-891`: the per-event dedup partition drops any event all of whose cards are in the optimistic map, and deletes the tracking | **C answers the policy, not the timing.** `anim_event_key` is the dedup key and `anim_stale_optimistic_on_table` (`c/src/anim_plan.h:615`) is the release rule. What nothing answers is what the FLIGHT does: today the confirming event is simply dropped, so the prediction's timer runs to completion and the card lands on the predicted schedule. If the prediction had already landed, the confirmation is a no-op and the board jumps. |
| A push that arrives mid-flight is applied by mutating the queue | `src/contexts/AnimationContext.tsx:169-173` appends; `:1074-1112` splices reverts in | **C has no concept of it, and this is the hole step 2 must fill.** A re-askable per-frame entry (`client_anim_advance(now_ms)`) turns this from "edit the chain the timer is walking" into "the next call answers differently". |
| A refused move's cards fly home | see section 7 | |

## 7. The refusal return flight (part 4, commit `5606c65b`)

`refusedBoard` (`src/contexts/AnimationContext.tsx:1339-1343`) makes the board a refused move's cards fly home to, but only while no push has moved the game on, which it tests by comparing `held.version !== tap.version`.
The four catch blocks then queue one `revert` event per card: `attack` `:1385-1424`, `pass` `:1483-1513`, `pickup` `:1564-1586`, `cover` `:1665-1686`.

| Decision | Web code | C today |
| --- | --- | --- |
| Does a refused card fly home at all, or has a push already taken it? | `src/contexts/AnimationContext.tsx:1341` (a version comparison) | **C answers it.** `anim_should_drop_stale` (`c/src/anim_plan.h:599`) is the same version comparison, and it is already delegated for pushes (`src/state/clientReconcile.ts:22`); the refusal path re-implements it inline with `!==` instead of `<=`. |
| Which of my pending cards still need a flight home | `:1388-1398` (attack), `:1483-1486` (pass), `:1553-1557` (pickup), `:1666-1667` (cover) - each a different combination of `revertingCards`, `optimisticCardPositions` and `optimisticAnimations` | **C answers it.** `anim_conflict_verdict` with `ANIM_TRANSPORT_SERVER` (`c/src/anim_plan.h:783`) buckets exactly this into REVERT / KEEP / CLEAR, and `AnimServerHope` is the extra question a refusal answers conclusively. Four hand-written variants of one rule is the pattern `wasm_anim_conflict_verdicts`' own comment calls out ("two doors onto one rule is a new way for two hosts to disagree", `c/wasm/wasm_api.c:951`). |
| Where the card flies FROM | `:1403-1404`, `:1496-1497`, `:1569-1571`, `:1670-1671`: `optimisticCardPositions.get(key)?.location ?? 'table'` (`?? 'hand'` for pickup, with a from/to swap at `:1571`) | **C answers it partly.** `anim_conflict_dest` (`c/src/anim_plan.h:790`) names the KIND of place a motion put its card, which is the same information in the kernel's vocabulary. The `?? 'table'` fallback has no C counterpart because in C a motion always carries its dest. |
| WHEN it flies | `queueAnimation` appends to the tail of whatever is running | **C has no concept of it.** A refusal's return flight has no place in the plan or the beats. |
| Reverting in a REVERSE group order, last motion first | **the web does not do this.** | **C answers it.** `anim_conflict_reversal` (`c/src/anim_plan.h:827`) is the whole rule, including dropping a group no motion reverts rather than playing a beat of silence. It is chain-transport only today, which is the one thing step 2 must widen. |
| The refused card comes back UNSELECTED | `src/contexts/AnimationContext.tsx:1419-1423` deletes the tracking; the selection is elsewhere | **C answers it.** `anim_selection_after_tap` (`c/src/anim_plan.h:942`) keeps a selection from ever naming a card not in my hand, which is the same defect stated as an invariant. |

## 8. The resync path

| Decision | Web code | C today |
| --- | --- | --- |
| After a refused move whose answer names a newer version, wait 1000 ms for a late push, then load the game | `src/constants/constants.ts:5` (`RECONCILE_GRACE_MS = 2 * ANIMATION_TIME`), `src/contexts/ServerContext.tsx:1033-1045` | **C has no concept of it.** This is a network deadline, not choreography. It is DERIVED from the animation constant, though, so once the kernel owns `ANIM_TIME_MS` this number must be derived from the kernel's too, or the coupling the comment relies on ("two flights, time for a late push") silently breaks. |
| A resync re-applies my pending optimistic cards so a just-played card does not vanish and reappear | `src/contexts/ServerContext.tsx:608` (`applyOptimisticOverlay`), fed by `src/contexts/AnimationContext.tsx:215-224` | **C answers the board edit** (`clientBoards.keepPending`, a `clientTable().edit` call), not the decision to do it. |
| A resync does not reset the version gate downward | `src/contexts/AnimationContext.tsx:237-250` (`Math.max`) | **C answers it partly.** `anim_should_drop_stale` is the per-push gate; "never lower the watermark" is a separate monotonic rule with no C entry. |
| A stale-round notice clears after 4000 ms | `src/contexts/ServerContext.tsx:97` | **C has no concept of it.** A toast's dwell time; stays in React. |

## 9. The queue's insertion order, which is the part with no C counterpart at all

`src/contexts/AnimationContext.tsx:1074-1112` is four branches that decide where revert events go relative to the server's own events:

- a pickup revert plus a `magic_transition` in the message: reverts go immediately before the magic transition (`:1074-1085`);
- otherwise, if the message has an attack from hand: reverts go immediately before it, "for parallel visual effect" (`:1086-1097`);
- otherwise, if the message has a pickup or trash: reverts go before it (`:1098-1109`);
- otherwise: reverts go first (`:1110-1112`).

**C has no concept of any of it.**
The nearest thing is `anim_conflict_reversal` (`c/src/anim_plan.h:827`), whose rule is quite different and much simpler: the doomed motions fly back first, in reverse group order, and only then does the arriving stream animate forward.
`anim_plan.h:672-676` states it as a principle: "The board REVERSES those motions before it plays anything else ... and only when it stands at a state the newest chain vouches for does that chain animate forward. Never a cut, never a snap."

That is the iMessage rule, so by this file's own direction of travel it REPLACES the four branches rather than being reconciled with them.
The "parallel visual effect" comment at `:1087` is the web's workaround for not having a reversal step, and it is why the revert and the valid attack appear to overlap: they do not overlap, they are two 500 ms steps the author hoped would read as one.
This is the single largest behavioural diff the gate will have to justify.

## 10. The rematch reset

| Decision | Web code | C today |
| --- | --- | --- |
| The win screen swaps to the lobby instantly, locally, before the meta round-trip | `src/contexts/ServerContext.tsx:952-976` | **C answers the board** (`lobbyBoard`, a kernel edit) but **has no concept of the transition**. |
| The authoritative reset then arrives as a `MAGIC_TRANSITION` broadcast and must not snap | the comment at `src/contexts/ServerContext.tsx:957-961` asserts there is no visible snap because the kernel makes both boards | **C answers it, richly, and the web ignores it.** `AnimSurfacePlan` (`c/src/anim_plan.h:1235-1266`) is exactly this question for iMessage: `ANIM_SURFACE_LOBBY` (`:1175`) is the board giving way back to the lobby, its transition is `ANIM_TRANSITION_FADE`, and `settle_ms` (`:1264`) says how long the surface must be on screen before it may be put away. `anim_surface_plan` (`:1293`) composes it. The web cuts. |
| A failed continue rolls back to the finished game | `src/contexts/ServerContext.tsx:967-970` | **C has no concept of it**, and it is a network failure path rather than choreography. |

## 11. Hand ORDER, the divergence the iMessage work found

Three implementations of "what order is a hand drawn in" ship on the web today, and a fourth is in C.

| Implementation | Where | What it does |
| --- | --- | --- |
| `mergeReplayHandOrder` | `src/components/ReplayScreen.tsx:256-289`, with `handCardKey` at `:248` | Keeps the viewer's preferred order for cards still present, appends new ones, drops departed ones. Face-down slots collapse onto one `'hidden'` bucket and are reconciled by COUNT. Used for every seat's revealed hand (`:323-328`). |
| `mergeHandOrder` | `src/state/clientReconcile.ts:45-52` | The same idea over `Card[]`, by key set rather than by count. Its own comment calls it "legacy". |
| `reconcileHandMemory` + `displayedHand` | `src/state/clientReconcile.ts:83-101` | The sticky arrangement memory the live hand actually renders from: memory grows only with genuinely new cards, and the rendered hand is the authoritative hand ordered by that memory. |
| `anim_hand_laid_out` | `c/src/anim_plan.h:975` | **C answers it.** Deferred deals drop out first; ids the local `order` knows keep their relative order from it; ids it does not know append in kernel order; stale and repeated entries fall out by construction. `order` is explicitly a grow-only memory that names cards not in the hand at all, which is `reconcileHandMemory`'s contract. iOS reaches it through `fio_hand_laid_out` (`c/ios/ios_api.c:802`) and `HandLayout.laidOut`. |

The divergence recorded in the session memory as "Hand order divergence: the board renders a replay, not the live game" is visible right here: `mergeReplayHandOrder` reconciles hidden slots by COUNT and `displayedHand` reconciles by KEY, so the same rearrangement scrubbed through a replay and played live can produce two different arrays.
`anim_hand_laid_out` already answers both, and `anim_fan_cards` (`c/src/anim_plan.h:960`) and `anim_laid_count` (`:966`) answer the held-back and deferred cases the web has no concept of.

**Gap for step 2:** `anim_hand_laid_out` takes dense card ids (0..51), so it cannot express a face-down slot at all.
The replay's hands are `(Card | null)[]`.
Either the kernel grows a "this slot holds a card I cannot name" id, or the replay passes the hidden slots' positions separately.
The sentinels are already reserved for it: `ANIM_TABLE_UNKNOWN` (`c/src/anim_plan.h:986`) is exactly "a cell holding a card the caller cannot NAME", with a static assert keeping it off the deck.

## 12. The bout-end hold, the out-collapse and the role hand-off

All three are the beats model, all three are iMessage rules, and the web has none of them.

| Rule | C | Web |
| --- | --- | --- |
| A cover that ended its bout rests before the sweep takes the table away | `ANIM_BEAT_HOLDS` (`c/src/anim_plan.h:420`), `anim_build_beats` | **nothing.** The sweep follows at the next 500 ms tick like any other step. |
| An `out` is a notice with no cards and no time, so a beat that MOVED something adopts the out notices trailing it and collapses those badges with its own card motion | `ANIM_BEAT_MOVED`, `outs_mask` (`c/src/anim_plan.h:421,426`) | **nothing.** An `out` event goes through the queue as its own 500 ms step with no cards, which is 500 ms of stillness. `src/state/pushSequence.ts` does not even give it a card list. |
| The cards a beat, and the whole stream, puts DOWN, so a later sweep is drawn from these | `ANIM_BEAT_PLACED`, `AnimBeat.placed_ids`, `AnimBeats.placed_ids` (`c/src/anim_plan.h:422,434,441`) | **nothing.** |
| A good being SET leads the stream; a good being CLEARED runs parallel with the throw-in that cleared it; a PASS hands the shield over WITH the transfer card; everything else waits for the closing beat | `AnimRoles` + `anim_goods_opening` / `anim_goods_cleared` / `anim_pass_hand_off` (`c/src/anim_plan.h:471-483`) | **nothing.** The web's marks come off whatever board the current step committed, so all three timings are "whenever the step that carried the board lands". |
| Who may write the shown badges | `anim_shown_ledger_allows` (`c/src/anim_plan.h:1080`) | **nothing.** Any code path may call `updateGameState`. |
| Which table the grid paints, and whether it is a sweep | `anim_shown_table` (`c/src/anim_plan.h:1047`) | `src/components/GameDisplay/AnimationOverlay.tsx:270` picks `currentAnimation.game_state ?? game`, which is the "live outranks pending" bug `anim_plan.h:1022-1031` says was written down as the rule and is wrong. |

## 13. Summary: what step 2 must add to C

C already answers, and the web only has to start asking:

- durations, gaps, start offsets, total length (`ANIM_TIME_MS`, `ANIM_GAP_MS`, `AnimPlanStep`, `AnimPlan.total_ms`);
- the count-freeze including the row and the flipped trump (`AnimCounts`, `anim_pre_stream_table`);
- the veil identities (`AnimPlan.veil_ids` and the `anim_veil_*` family);
- beat grouping, the hold, the out-collapse, the placed set and the badge-drop rule (`anim_build_beats`, `AnimBeat`);
- the role hand-off timings (`anim_goods_opening`, `anim_goods_cleared`, `anim_pass_hand_off`);
- the dedup key (`anim_event_key`) in place of the JSON-string map;
- the conflict verdict for all four refusal shapes (`anim_conflict_verdict`, already reachable);
- the reversal's order (`anim_conflict_reversal`), once it is widened past the chain transport;
- the hand's laid-out order (`anim_hand_laid_out`), once it can express a face-down slot;
- the lobby-and-back surface plan (`anim_surface_plan`) for the rematch.

**WHAT CAME OF THIS (Phase 9 steps 2 and 3, 2026-09-17).**
Every row above that says "C answers it" now has the web asking, except `optimisticPassState`'s three surviving reconciliation sites and the JSON-string dedup map, which are written up as the remainder in `docs/C_GAME_SHAPE_MIGRATION.md` "Phase 9 as built, part 2".
Two rows came out differently from the way this file guessed.
Section 9's four insertion branches were called "the single largest behavioural diff the gate will have to justify"; replacing them with the kernel's reversal moved no frame of any recorded trace, because in every one of them the branch's anchor event was event 0 and all four already produced "reverts first".
Section 2's claim that "the FIRST step's board is on screen from frame zero" was overstated for the badges: the web already committed a step's board only as its flight landed, and what it really lacked was ONE landing - the board a predicted move leaves rode a second timer in another file, and for one frame the board had advanced while the card was still in the air.

C needs new entries for:

1. **A re-askable per-frame call.** Given `now_ms` and the live board plus whatever is pending, return the current step or beat and the next deadline, so a push landing mid-flight is answered by the next call and not by editing a timer chain. This is the one structural addition; everything else is plumbing.
2. **A re-exported plan and beats builder for wasm.** `wasm_anim_build_plan` was deleted as dead code in `0cda5e85`; iOS still reaches the same C through `fio_anim_plan` (`c/ios/ios_api.c:407`) and `fio_anim_beats` (`:507`).
3. **Optimistic TIMING.** When a predicted flight starts, how long it lasts, and what a confirming push does to one in progress.
4. **The refusal return flight as part of the plan**, so its duration and its position are the kernel's and not `queueAnimation`'s.
5. **A hand order that can name a face-down slot**, so the replay's `(Card | null)[]` and the live hand get one answer.
6. **A monotonic version watermark**, so "never lower the gate" is a kernel rule and not a `Math.max` in a React effect.

C must NOT grow:

- screen coordinates, rects, curves or the `flightPlaces` per-place hiding (rendering);
- the replay's speed dial, its recorded-gap clamps and its ticker interval (a transport over wall-clock times, not choreography);
- the 4000 ms stale-round toast and the 1000 ms reconcile grace (network and notification deadlines), except that the latter must be derived from the kernel's `ANIM_TIME_MS` rather than from a TS copy of it.
