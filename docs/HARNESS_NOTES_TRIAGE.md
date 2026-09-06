# Test-harness phone notes — triage (2026-07-20)

Owner went through the iMessage test harness on-device and left ~44 notes.
Each note below is mapped to a root cause (verified against the tree at
`c9751ae`) and an implementation batch. Batches are landed one at a time, in
order, each as its own commit on `claude/test-harness-notes-triage-js4y8q`.

Status legend: `[1]`..`[7]` = batch number · `[D]` = deferred (documented in
batch 7, not built now) · `[A]` = answered, no code.

## The notes, verbatim → cause → disposition

| # | Note (abridged) | Root cause / finding | Batch |
|---|---|---|---|
| 1 | "Who are you playing as" screen is a security violation in release; spectate instead | `GameSurface.adopt` `.ambiguous` → `SeatPicker` is compiled into Release (§6.3 path), and picking a seat shows that hand | [1] |
| 2 | Collapsed should actually collapse the size | Harness passes `.compact` but never resizes the surface — the real host shrinks the viewport, the harness doesn't | [5] |
| 3 | Own seat card view isn't shown (and pickup animates to the player on the right) | Local seat is drawn only as the hand; no role indicator (shield/sword/good) for self. The wrong pickup direction is actually note 4/9's replay bug | [2] |
| 4 | Pickup move (+consequent draws) isn't replayed on open | `replayLastMoveOnOpen` only replays the last `LOG_ATTACK/LOG_COVER`; a pickup leaves an empty table → early return, draws never animated | [3] |
| 5 | Rearrange cards like in react | `FHandFan` renders kernel hand order; no local reorder state or in-hand drag handling | [4] |
| 6 | Only the first attacker gets a sword before he attacks | Engine rule is correct (only first attacker may open); the *display* gate is the bug — see note 12 | [1] |
| 7 | Slightly smaller opponent-hand cards, larger sword/shield | `FSeatBadge` constants (24×34 backs; shield 19 / sword 16) | [2] |
| 8 | Defender icon shifts as soon as pass is done — TODO for react | iOS derives defender from the post-move view, web waits for `defender_move`; leave TODO in `PlayerRing.tsx` | [1] |
| 9 | After a pass, replay-on-open throws in the wrong cards from the player on the right | `replayLastMoveOnOpen` ignores `LOG_PASS` (type 3): it finds the *previous* `LOG_ATTACK` — the original attacker's cards, from their seat | [3] |
| 10 | Attack → undo animates a deal from the previous bout | `flyBoutEndToDiscard` sees battles → empty on undo and misreads it as a bout end; `lastBoutDraws` then replays the previous bout's draw log | [3] |
| 11 | Card selected → Take (defender) / Good (attacker) buttons should hide; TODO react | `actionBar` gates `canPickup/canDone` only on the legal menu, not on selection emptiness | [1] |
| 12 | Swords appear inconsistently — attacker should keep sword until *they* say good, even with all attacks covered | `isAttacker` is computed with `attackersActive` (any uncovered battle), so covering everything removes every sword | [1] |
| 13 | Good should be a green SVG check | Button is the word "Good"; badge uses SF `checkmark.seal.fill` (SF symbols are risky in `ImageRenderer` snapshots — swords/shields are hand-built for that reason) | [1] |
| 14 | Trump card (empty deck) equal left/top distance from edges | `FDeckWell` centers in a 92×108 frame plus a `-30` board offset tuned for the stacked state | [2] |
| 15 | Octogen research: random rollout? cheater rollout? | Research idea | [D] |
| 16 | Speedup: 4 same-value attacks + 4 same-value covers → auto discard | Engine rule change, needs wire/replay compat thought | [D] |
| 17 | Defender covering with last card: needs cover animation *then* discard; today the covering cards just disappear | `apply` jumps state to the cleared table in one diff; the cover flight has no slot to land in before `flyBoutEndToDiscard` runs | [3] |
| 18 | Final screen zooms the background; leaderboard hard to see | The board→`FGameOverList` swap happens inside `.animation(value: controller.view)`; the `WoodFill` plank stretches in under a spring (worse with more rows — invisible in 2p). Rank colors are near-black on wood | [2] |
| 19 | Player-count picker is out-of-theme glass | Segmented `Picker` in `NewGameSetup`; superseded by lobby v2 (picker removed) | [6] |
| 20 | DMs auto-2; groups: create text → join texts (nickname) → any joined player can start at 2+; seed locked at create | Today WAITING requires a fixed player count at create; seed *is* already locked at create (`createWaiting`). Needs open-count lobby + explicit start | [6] |
| 21 | Final move should be a separate text from the new-game text | `stage()` always reuses `selectedMessage?.session`, so the next game's first bubble replaces the finished game's result bubble | [6] |
| 22 | Fair tempo: no pickup within 10s of first attack | Needs a bout-start timestamp in the payload (wire change) + honor-system clock caveats | [D] |
| 23 | Fair tempo: 120s after last cover → next good advances round | Same design doc | [D] |
| 24 | Fair tempo: defender AWOL ~5min → next good forces pickup + advance | Same design doc | [D] |
| 25 | New game should just stage the lobby (unspecified count until start) | Same as note 20 | [6] |
| 26 | Do we need a privacy policy link? | Store metadata requires a privacy-policy URL in App Store Connect; not required inside the extension UI. See batch-7 doc | [A]/[D] |
| 27 | Should link to the real app once it's ready | `/m/` page + finished bubble already funnel to foolish.cards; App Store link once live | [D] |
| 28 | White text on wool is basically invisible | Setup/lobby/name-gate/seat-picker screens use `FColor.textPrimary` (bone) over the light beige/wool surface | [2] |
| 29 | Name input same width as Continue | `NameGateView` field uses `.padding(.horizontal)` while the button is full-width | [2] |
| 30 | Suit icon in card corner shouldn't depend on rank width (10 offsets it) | `FCard.corner` VStack centers the suit under the rank text | [2] |
| 31 | Harness should simulate exact iPhone SE measurements, centered, rounded, divot | Harness fills the window; no fixed stage | [5] |
| 32 | Pickup → undo still allows you to stage | Harness `staged` (and the real extension's inserted bubble) survives an undo that empties `pending`; `stageNow` no-ops instead of retracting | [1] |
| 33 | Drag verb hint ("attack"/"cover"/"pass") like react | Not built; the kernel's `play_resolve` can answer it live | [4] |
| 34 | Pass drag: don't highlight the pass card; open + highlight a new empty slot | Drop preview only highlights coverable battles; no pass affordance | [4] |
| 35 | It's "pickup" not Take! | `FStrings` en `"pickup": "Take"` | [1] |
| 36 | Good can flash: cards appear in hand, fade, re-fly from deck | State renders the refilled hand before `BoardAnimator.hidden` is set (the hide happens per-step after frame polling) | [3] |
| 37 | Rotate discarded cards 90° | `FDiscardPile` layers are portrait with ±20° jitter only | [2] |
| 38 | Triple cover replay only replays rightmost cover | `replay.logs.last(where:)` — a single event, not the move's whole event span | [3] |
| 39 | Ending game should play final animation, for receivers too | `isOver` swaps to `FGameOverList` immediately; `replayLastMoveOnOpen` guards `!controller.isOver` | [3] |
| 40 | End-screen zoom absent in 2p — shouldn't happen at any size | Confirms note 18's plank-stretch diagnosis (2 rows ≈ no visible stretch) | [2] |
| 41 | Ranking scores dark on wood | `FGameOverList` rank colors `.black.opacity(0.55)` / `0x5A3B00` | [2] |

## Batch order & rationale

1. **Prod-safety + action gating** — the release security hole (spectate, not
   seat-pick), sword-visibility rule, selection-aware Take/Good, "Pickup"
   wording, hand-built green check, undo-retracts-stage. Small, independent,
   high value.
2. **Visual polish** — pure cosmetics: discard rotation, deck-well spacing,
   badge/icon sizes, wool-text contrast, name-field width, card-corner
   alignment, end-screen contrast + zoom kill, self role indicator.
3. **Animation correctness** — one engine: replay the *event delta* since the
   previously-seen chain (pickup, pass, multi-cover, draws, final move), stop
   misfiring on undo, pre-hide incoming cards, sequence cover→discard, hold the
   end screen until the last flight lands. Hardest batch, isolated to the
   animation layer.
4. **Drag & hand UX** — reorder, verb hint, pass ghost slot.
5. **Harness SE stage** — layout-testing aid; after the UI batches so it
   measures the final layout.
6. **Message semantics + lobby v2** — session separation and the open-count
   lobby. Biggest protocol surface (may touch `msg_wire.c`; C tests run on
   Linux), so it goes last among code batches.
7. **Deferred doc** — fair-tempo design, octogen research note, auto-discard
   speedup, privacy-policy answer, app link.

## Verification constraints

No Mac in this environment: Swift cannot be compiled here. Swift changes are
kept conservative and mirrored on existing idioms; C/wasm/e2e suites run on
Linux and MUST stay green for any batch touching `c/` or the TS side. The
first Mac session should run `xcodebuild build test` + the HarnessTests suite
before shipping.

## Batch status (2026-07-20)

Seven commits landed on this branch so far — the triage doc itself plus
batches 1–6 (`git log --oneline`, newest first):

| Commit | What |
| --- | --- |
| `3e296f2` | batch 6 — session-per-game bubbles, decodable FINISHED bubble + web replay funnel, open-count lobby with explicit start |
| `ca1e6d8` | batch 5 — simulate iPhone SE extension viewport (real collapse, centered stage, rounded corners + grabber) |
| `2b24965` | batch 4 — hand reordering, drag verb hint, pass ghost-slot preview |
| `a4ec82c` | batch 3 — delta replay on open, undo/animation isolation, pre-hide, cover-then-discard, final animation before results |
| `5d308e0` | batch 2 — discard rotation, deck-corner spacing, badge sizes, wool contrast, corner indices, end-screen zoom+contrast, self role icon |
| `77647b2` | batch 1 — release spectate, sword/button gating, Pickup wording, green check, undo retracts stage |
| `5eccd20` | docs: triage the on-device harness notes into batched, root-caused work (this doc, pre-batch-1) |

**Batch 7 = `docs/IMESSAGE_DEFERRED_V2.md`** (this commit): design sketches for
the deferred/answered notes (15, 16, 22–24, 26, 27) — fair-tempo enforcement,
the octogen rollout-policy research idea, the same-value auto-discard variant,
the privacy-policy answer, and the post-launch app-store funnel. Docs only, no
code.

**Before shipping any of batches 1–6:** all of it is Swift UI/animation work
verified only by inspection and the existing Linux-side C/wasm/e2e suites — no
Mac has compiled or run this tree yet. The first Mac session must run a full
`xcodebuild build test` pass and **re-record snapshot references**, since
batch 2 changed `FCard`/`FSeatBadge`/`FDeckWell` visuals (discard rotation,
badge sizes, deck-well spacing, corner indices) that the committed snapshot
refs predate. Treat every batch-1–6 commit as unverified on-device until that
session happens.
