# What happens if the web calls the same high-level kernel entries the phone does

An inventory, a measurement and a recommendation, written against `origin/main` on 2026-09-06.

The owner's question was "see what happens if web calls same high level entries. I'd prefer less TS code and more C code".
The standing instruction for this repo is when in doubt, move code to C, and `docs/KERNEL_LIFT_BRIEF.md` sets the direction of travel: iMessage is the spec, the web is the client that re-derives.

This document measures what is actually still duplicated rather than assuming.
The short version is that the web is much further along than the two adapter surfaces suggest, the remaining true duplication is around 560 lines of TypeScript, and roughly half of that is deletable for zero wasm growth because the kernel entry it duplicates is already exported.

## The headline numbers

| bucket | lines of TypeScript |
|---|---|
| true duplicate of a kernel answer the web could call today (no new wasm export) | 304 |
| true duplicate needing one or more NEW wasm exports | 260 |
| **total true duplicate** | **564** |
| TS shim the new exports would ADD back to `sdk/ts/wasm/bots.ts` | about 225 |
| **net TS deletion if every candidate is taken** | **about 340** |
| would need a NEW kernel rule written first (not a duplicate of an existing answer) | 561 |
| genuinely web-only (React, DOM, Next, Supabase, textures, routing) | the large majority of the 32,319 TS lines in `src/` + `server/impls/` |
| low-level wasm plumbing that shrinks but never vanishes | 3,073 (`sdk/ts/wasm/bots.ts` 1,449, `engine.ts` 1,624) plus `src/wasm/clientGuards.ts` 285 |

Measured wasm cost, from a real build of `bots.wasm` on this machine (baseline 158,551 raw / 67,129 gzip):

| what is exported | raw delta | gzip delta |
|---|---|---|
| all 36 remaining semantic kernel rules, symbols only | +7,572 | +3,525 (+5.3%) |
| the 25 board-set rules (`anim_veil_*`, fan, table, finish rows, roles) | +2,592 | +1,220 |
| the 6 beats / pre-bout / conflict rules | +2,981 | +1,494 |
| the 5 gesture rules (`legal.c` `play_*`) | +1,983 | +887 |
| `anim_finish_rows` alone | +245 | +129 |
| the fan trio (`anim_fan_cards`, `anim_laid_count`, `anim_hand_laid_out`) | +818 | +410 |
| `play_resolve` + `play_can_say_good` | +1,036 | +477 |
| 8 representative rules reached through real marshalling shims | +5,622 | +2,546 |
| the same 8 rules exported as bare symbols, no shim | +4,236 | +2,049 |

The last two rows isolate the JS-boundary cost: **about 173 raw / 62 gzip bytes per entry** for the `wasm_*` wrapper, on top of the rule itself.

## Method, and what makes this measurable

`c/src/anim_plan.c` is **already linked into `bots.wasm`** (`WASM_BOT_SRC`, `c/Makefile:808`), and so is `legal.c`.
The board-set and gesture rules the phone calls are therefore already compiled; `wasm-ld`'s dead-code elimination drops them only because nothing roots them.
That makes the marginal cost directly measurable: relink the already-built objects with extra `-Wl,--export=` flags and run the same `wasm-opt -O2 --inlining-optimizing` pass the Makefile runs.
That is what the table above is.

`bots.wasm` is loaded by the browser as well as the edge (`sdk/ts/wasm/wasm_asset.ts`; `src/app/providers.tsx` awaits `ensureBotsAsync` at boot), so every byte is paid twice - once as a Supabase edge cold start, once as a browser download.

## What the web already gets from the kernel

Worth stating first, because it changes the shape of the answer.
The web is not the un-lifted client the two surfaces imply.

- **The conflict model is already shared.** `src/state/optimisticConflicts.ts` calls `animResolveUnconfirmed` -> `wasm_anim_resolve` -> `anim_resolve_unconfirmed_attack_covers`, which is implemented in terms of `anim_conflict_verdict` (`c/src/anim_plan.c:622`) - the exact function `fio_conflict_packed` wraps. The `AnimServerHope` struct in `anim_plan.h` exists specifically for the web's transport. There is no duplicate here to delete.
- **The version gate is shared** (`src/state/clientReconcile.ts:78` -> `animShouldDropStale`).
- **The stale-optimistic release is shared** (`src/state/optimisticAnimation.ts:56`).
- **Move legality is shared.** `src/utils/gameValidation.ts` is delegation only; `src/wasm/clientGuards.ts` runs the real `handle_*` through `guards.wasm`.
- **Cover resolution is shared** (`kernelUnambiguousCover` -> `legal.c unambiguous_cover`), reached from `DragContext`, `ActionButtons`, `KeyboardInputHandler` and `KeyboardPlayMode`.
- **The view decode is shared** (`kernelViewFromPacked`); `sdk/ts/wire/view.ts` explicitly says its byte-for-byte `parseMaskedState` is gone.
- **Replay is shared.** `src/replay/frames.ts` pulls real evwire frames from the kernel; the TS retrodiction fold is gone.

## 1. Inventory: TypeScript that re-derives what a `fio_*` entry already answers

Confidence is stated per row.
Where I could not establish that the two answer the same question, I say so and do not count the lines.

### 1a. Duplicates where the kernel entry is ALREADY exported to wasm (zero module growth)

| `fio_*` / kernel entry | already-exported wasm twin | TS site | TS lines | confidence |
|---|---|---|---|---|
| (the whole anim core, pre-lift) | n/a - dead code | `src/state/__ts_reference.ts` | 237 | certain: the file's own header says "dead code except under test", kept only for `e2e/anim_core_parity.test.ts` until the deferred deletion in `docs/ANIMATION_CORE_C.md` |
| `fio_actor_mask` / `should_bot_act` | `wasm_should_act` | `server/api/common/common_utils.ts:148` `shouldBotActCore` | 34 | high: the comment itself says "the kernel counterpart is should_bot_act in c/src/game.c (e2e/wasm_engine.test.ts polices parity)" |
| `game_done` | `wasm_game_done`, and `clientGuards.gameDone` is already written | `common_utils.ts:183` `game_done` | 10 | certain: same comment, same parity test |
| `can_cover` | `wasm_can_cover`, and `clientGuards.canCoverPair` is already written | `common_utils.ts:80` `canCover` | 7 | certain |
| `get_next_player_index` | `wasm_next_player`, and `clientGuards.nextPlayerIndex` is already written | `common_utils.ts:63` `get_next_player_index` | 16 | high: the TS adds a "only one player left" warning branch the kernel does not have; behaviour is the same for every reachable state |
| **subtotal** | | | **304** | |

Three of those four rules already have a finished TS wrapper in `src/wasm/clientGuards.ts` that nothing on the server path calls.
This is the cheapest 33 lines in the report.

### 1b. Duplicates needing a NEW wasm export

| `fio_*` entry | TS site | TS lines | confidence |
|---|---|---|---|
| `fio_play_probe` | `src/contexts/DragContext.tsx:54` `determineGameAction` | 84 | high: same question ("which menu entry does this drop resolve to"), same inputs modulo the DOM hit-test, and the TS already calls the kernel for the multi-cover half of it |
| `fio_play_probe` | `src/contexts/DragContext.tsx:152` `canPass` selection helper | 12 | high |
| `fio_play_probe` (`play_best_cover_target`) | `src/components/GameDisplay/ActionButtons.tsx:208` `handleCoverClick` | 29 | high, **but the answers differ - see §3** |
| `fio_play_probe` (`play_coverable_battles`) | `src/components/KeyboardInputHandler.tsx:67` | 6 | high |
| `fio_play_probe` (`play_coverable_battles`) | `src/components/GameDisplay/KeyboardPlayMode.tsx:290` | 7 | high |
| `fio_play_human_menu` (`play_can_say_good`) | `ActionButtons.tsx:151` `rawGood` / `rawPickup` | 8 | high |
| `fio_finish_rows` | `server/api/common/common_utils.ts:267` `calculateGameRankings` | 33 | high, with one difference: the TS dedups `elimination_order` "to handle backend bugs"; `anim_finish_rows` does not |
| `fio_finish_rows` | `src/components/WinScreen.tsx:74-121`, the ranking half only | 22 | high; the ELO fetch interleaved with it is web-only and stays |
| `fio_hand_laid_out` + `fio_fan_cards` + `fio_laid_count` | `src/state/clientReconcile.ts:80,133,143` `mergeHandOrder` / `reconcileHandMemory` / `displayedHand` | 24 | high: `anim_hand_laid_out`'s contract ("ids `order` knows keep their relative order, ids it does not know append in kernel order, stale entries fall out") is `displayedHand` restated |
| `fio_selection_after_tap` | `src/contexts/GameContext.tsx:30` `handleCardSelection` | 11 | high, with one difference: the C version intersects the selection with my hand, the TS does not |
| `fio_badge_drops_as_cards_leave` and the plan's count ledger | `src/components/GameDisplay/DeckAndFlipped.tsx:15-23` + `AnimationContext.tsx:1256-1266` | 24 | medium: the web's freeze is an ad-hoc `deck_length - inFlightFromDeck` subtraction, the kernel's is a per-step ledger. Same intent, different fidelity |
| **subtotal** | | **260** | |

### 1c. Where the web has NO counterpart (nothing to delete)

Counted here so the `fio_*` list is walked completely.

- `fio_beats_packed`, `fio_roles_*`, `fio_pre_bout_table_packed` - the web's animation model is a strictly sequential one-event-at-a-time queue with a fixed `ANIMATION_TIME` and a 25 ms gap (`AnimationContext.tsx:1217-1345`). It has no beats, no role freeze and no pre-bout table. These would be new behaviour, not a deletion.
- `fio_veil_*` (8 entries), `fio_holdback_is_mine`, `fio_shown_table`, `fio_covered_sweep_accepts`, `fio_table_covers`, `fio_table_card_ids`, `fio_shown_ledger_allows` - the veil is an iMessage concept. The web's nearest thing is `animatingCards` plus DOM measurement in `AnimationOverlay.tsx`, which is rendering, not a rule.
- `fio_msg_*` (about 30 entries), `fio_nickname_verdict`, `fio_name_max_*`, `fio_seat_*`, `fio_roster_name_taken`, `fio_msg_turn_*` - the iMessage chain transport, its staging state machine and its participant identity model. The web has an authenticated `user_id` and a server; none of this applies. The `/m/[payload]` page already calls `kernelMsgDecode` for the one piece it needs.
- `fio_evw_is_settlement`, `fio_evw_frames_settlement_cut` - a staged-bout-end concept with no web equivalent (grep for "settlement" in `src/`, `sdk/ts/`, `server/` returns nothing).
- `fio_anim_plan_packed` - the twin `wasm_anim_build_plan` **is already exported and has no production caller**; its only consumer is `e2e/anim_core_parity.test.ts`. See §3.
- `fio_state_packed`, `fio_legal_packed`, `fio_bot_drive_packed`, `fio_replay_*`, `fio_apply_awire` - the web reaches the same C through `wasm_view_serialize` / `wasm_legal_moves` / `wasm_bot_drive` / `wasm_replay_*` / `wasm_apply_action`. Already converged, different door.

### 1d. Not a duplicate - would need a new kernel rule written first

`src/contexts/AnimationContext.tsx:329-745` `resolveOptimisticConflicts` is **417 lines** and is a superset of what the kernel answers today.
It delegates the attack/cover verdict to C, then adds three rules C has no entry for:

- the optimistic PASS capacity revert (`totalAttacksIfPassSucceeds > nextDefenderHandSize`, an explicit "kernel PASS_CAPACITY mirror"),
- the attacker-became-defender revert when the server's defender changes under an optimistic attack,
- the merge of kept optimistic cards back into every intermediate state of the incoming sequence.

`anim_conflict_verdict` has the vocabulary for the first two (`ANIM_DEST_MY_HAND`, `AnimServerHope`) and `anim_conflict_reversal` has the shape of the revert plan, but neither answers these questions as shipped.
Plus `AnimationContext.tsx:1217-1360` (144 lines) is the sequential timeline that `anim_build_plan` would replace.
Together **561 lines** that are a stage-8 lift, not a call-the-same-entry refactor.
I have deliberately kept these out of the "duplicate" total.

### 1e. Dead TypeScript found along the way, unrelated to the kernel

`server/api/common/strategies/move_stats.ts` (1,634), `strategies/pass_prob.ts` (321) and `durakai/cardTracker.ts` (360) - **2,315 lines** of TypeScript bot strategy whose only importer in the whole repo is `offlinefun/localtest/console_strategy.ts`, a dev tool.
No production or test path reaches them.
Not a convergence item, but it dwarfs everything in this report and the brief's "prefer delete" rule applies.

## 2. What is genuinely web-only, and stays

The brief's line is that only rendering is irreducibly per-platform - the `CGRect` line.
On the web the equivalents are:

- `src/components/GameDisplay/AnimationOverlay.tsx` (583) - invisible-placeholder DOM measurement, `getBoundingClientRect`, CSS transitions. Pure rendering.
- `src/contexts/DragContext.tsx`'s `getTableCardUnderCursor` (`elementsFromPoint`), pointer/touch handling, drag thresholds. The gesture's *resolution* is liftable; the hit-test is not.
- Every texture and canvas component (`fernFractal` 651, `WoolBackground` 432, `WoodTexture` 295, `SovietIcon` 299, `webglTexture`, `textureCache`, `ConcreteTexture`, `TexturedSurface`).
- `src/contexts/ServerContext.tsx` (1,409) and `src/backend/Connector.ts` - Supabase realtime, auth, fetch, React state.
- `server/impls/supabase/functions/_shared/adapter/utils.ts` (1,180) - row locking, CAS commits, broadcast fan-out, edge scheduling. Server-shaped, not rule-shaped.
- Routing, localization, leaderboard, match history, lobby, chat, the Oracle UI.

## 3. The cost side, honestly

### Module growth

The full set is **+7,572 raw / +3,525 gzip (+5.3%)** for the rules, plus about 173 raw / 62 gzip per `wasm_*` wrapper.
For the recommended subset in §4 the realistic figure is **+1,300 to +1,700 gzip bytes, under 2.5%**.

That is small against the repo's own history (`-Oz` cut the shipped gzip 49,446 -> 46,259; the module has since grown to 67 KB), and it is paid on every Supabase edge cold start and every browser first load.
It is not free, and it does not have a natural floor: each new entry is another permanent export the linker can never drop again.

### The JS marshalling shim, per entry

Every entry needs a hand-written TS shim because the wasm boundary cannot pass pointers.
Measured against the four that exist today in `sdk/ts/wasm/bots.ts`:

| existing shim | TS lines |
|---|---|
| `animShouldDropStale` (4 scalars in, 1 out) | 6 |
| `animStaleOptimisticOnTable` (3 card lists in, index list out) | 20 |
| `animResolveUnconfirmed` (records in, 3 index lists out) | 30 |
| `animBuildPlan` (event stream in, packed plan out) | 55 |

Plus the `wasm_*` C wrapper (about 15-30 lines) and one line in `WASM_BOTS_API_EXPORTS` in `c/Makefile`.
So budget **20-25 TS lines per entry** and treat any candidate that deletes fewer than about 40 TS lines as net-negative on line count.
The fan trio is exactly that case: 24 lines out, about 40 in.

### Reentrancy

`wasm_io_ptr` is one global scratch slot per module instance and the module is single-threaded, so the wasm path is not reentrant the way the Swift path (caller buffers) is.
This blocks nothing in the candidates below.
All of them are synchronous leaf calls - marshal, call, read the answer back, done, with no `await` inside.
It does become a hazard if a future entry is called from inside a React render that can suspend, or from two overlapping realtime handlers.
The related and already-burned landmine is the resident-game slot (see the `residentFor` stale-marshal bug and the "never seal OR READ across an await" note): any entry that needs the resident `Game` rather than pure arguments inherits that.
This is why `play_probe` taking a *menu* rather than reading the live game is the right shape - it reads nothing but its arguments, which is the same property that lets a SwiftUI render pass call it.

### The one performance caveat

`determineGameAction` runs on every drag frame (`TableBattles.tsx:36`, `DragShadow.tsx:32`).
`fio_play_probe` wants a packed menu, and the web does not compute a menu client-side at all today - it validates specific candidates through `guards.wasm`.
Getting a menu means `wasm_legal_moves` + `wasm_export_moves` on `bots.wasm`, which marshals a `Game` and can enumerate thousands of cover combinations.
`legal.h` says the `play_*` rules are "cheap enough to call on every frame of a drag", and they are - but producing their input is not.
The menu must be cached per committed state, not recomputed per frame.
That caching is real work and is the main risk in the top candidate.

## 4. Where the web's behaviour would visibly change

The brief's rule is that the C answer wins.
These are product decisions, not refactors, and should be shown to the owner before the change lands.

1. **The Cover button would aim at a different card.**
   `ActionButtons.handleCoverClick` picks the *first* uncovered battle the selection beats (`uncoveredBattles.find(...)`).
   `play_best_cover_target` picks the *highest* attack it beats, trumps outranking everything, ties to the leftmost - and `legal.h` says explicitly that leftmost-first-match "is not a rule at all, it is the order the attackers happened to throw in".
   The kernel is right, and a web player who has learned the current targeting will see it change.

2. **Ranking would stop deduplicating `elimination_order`.**
   Both TS copies do `Array.from(new Set(...))` "to handle backend bugs"; `anim_finish_rows` does not.
   If that backend bug is still reachable the kernel would produce a wrong finish order where the TS produced a right one.
   Establish whether the bug is dead before taking this one.

3. **Tapping a card not in your hand would become a no-op.**
   `anim_selection_after_tap` intersects with the hand; `handleCardSelection` does not.
   Almost certainly unreachable in the web UI, but it is a difference.

4. **Adopting `anim_build_plan` would change every animation on the site.**
   Parallel beats instead of a strict one-event queue, per-step durations instead of a fixed `ANIMATION_TIME`, a real count-freeze ledger instead of `deck_length - inFlightFromDeck`.
   This is the largest single behaviour change available and the largest single TS deletion.
   It is also the one the brief most clearly endorses, since iMessage is the spec.
   It is not a refactor and should not be sold as one.

## 5. Recommendation, ordered

### Take these

**1. The four already-exported rules, and `__ts_reference.ts`. 304 TS lines, zero wasm growth, near-zero risk.**
Delete `src/state/__ts_reference.ts` (237) and its parity test - the brief's "prefer delete" rule names this exact case, and `docs/ANIMATION_CORE_C.md` already lists it as the deferred deletion.
Point `common_utils.canCover`, `game_done` and `get_next_player_index` at the `clientGuards` wrappers that already exist (33 lines).
Best ratio in the report by a wide margin.
The one thing to check first is `shouldBotActCore` (34): it is called from the browser on a `PublicGame` that has no hands, so verify `wasm_should_act` gives the same answer from a hand-less marshal before switching it, and measure the per-poll cost.

**2. `fio_play_probe` + `fio_play_human_menu`. 146 TS lines out, about 50 back, +887 gzip.**
This is the biggest genuine consolidation left: five separate TS sites (`DragContext`, `ActionButtons`, `KeyboardInputHandler`, `KeyboardPlayMode`, and the good/pickup gate) all answering "what does this gesture mean" in slightly different words, against one kernel entry that was written to be the single answer.
It also fixes the Cover-button targeting to the kernel's rule.
Cost: the menu-caching work in §3, and the behaviour change in §4.1.
Do it after (1) and treat the cache as the design problem, not the shim.

**3. `fio_finish_rows`. 55 TS lines out, about 20 back, +129 gzip.**
The finish order is currently derived in three places - `anim_finish_rows`, `calculateGameRankings`, and `WinScreen` - and the two TS copies already disagree with C on dedup.
Cheapest new export in the report and it collapses a genuine three-way fork.
Settle §4.2 first.

### Consider separately, on its own merits

**4. Adopt `anim_build_plan` in production.**
It is built, exported, shipped in every `bots.wasm` the browser downloads, and called by nothing but a test.
Paying for an entry and not using it is the worst of both worlds.
Adopting it deletes the 144-line sequential queue and the ad-hoc count freeze, and gives the web the phone's beats and count ledger.
But it rewrites `AnimationContext` and `AnimationOverlay` around a timed plan instead of a queue, and it changes every animation on the site.
This is a product decision with a real implementation behind it - put it to the owner as one, not as cleanup.

### Do not do these

**The fan trio** (`fio_fan_cards` / `fio_laid_count` / `fio_hand_laid_out`).
24 TS lines out, about 40 shim lines back, +410 gzip.
Net-negative on both line count and module size.
The rule is worth sharing in principle, but `displayedHand` is nine lines of obviously-correct set arithmetic and the phone's `held`/`deferred` inputs have no web meaning.

**`fio_selection_after_tap`.** 11 lines out, 8 back, plus a behaviour change nobody asked for.

**The veil, beats, pre-bout table, roles, `shown_table`, `covered_sweep_accepts`, `shown_ledger_allows`.**
Nothing to delete - the web has no counterpart.
Exporting them costs +1,220 gzip to give the browser rules it does not call.
They come with the `anim_build_plan` decision if that is taken, and not before.

**Every `fio_msg_*`, `fio_seat_*`, `fio_nickname_*`, `fio_msg_turn_*`, `fio_evw_*` entry.**
The iMessage chain transport has no web analogue.
Exporting them would be growing the module for nothing.

**`fio_conflict_packed`.** Already shared through `wasm_anim_resolve`; a second door onto the same C would be a new way for two hosts to disagree, which is the thing the campaign exists to stop.

### The aside

`move_stats.ts` + `pass_prob.ts` + `cardTracker.ts` = 2,315 lines reachable only from a dev tool.
Deleting them removes four times more TypeScript than every kernel candidate in this report combined, at zero risk and zero wasm cost.
It is not what was asked, but if the goal is less TS, it is the largest lever in the tree.

## Appendix: how to reproduce the measurement

Build the baseline, then relink the same objects with extra exports and re-run the Makefile's `wasm-opt` pass:

    cd c && WASM_CC=/opt/homebrew/opt/llvm/bin/clang make build/bots.wasm
    # then relink build/botobj/*.o with the WASM_API_EXPORTS + WASM_BOTS_API_EXPORTS
    # list plus -Wl,--export=<symbol> for each candidate, and:
    #   wasm-opt --enable-bulk-memory --enable-mutable-globals --enable-sign-ext \
    #            -O2 --inlining-optimizing <out> -o <out>
    # compare `wc -c` and `gzip -9 -c | wc -c` against the baseline link.

Because `anim_plan.c` and `legal.c` are already in `WASM_BOT_SRC`, exporting a bare `anim_*` / `play_*` symbol measures the rule's own recovered code size, and linking a shim object alongside measures rule plus wrapper.
The difference between those two is the per-entry marshalling cost quoted in §3.
