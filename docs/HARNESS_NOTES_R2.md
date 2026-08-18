# Harness notes, round 2 - triage

Second pass of on-device testing (2026-07-20), after batches 1-7 landed. Sixteen
notes. This file is the same shape as `HARNESS_NOTES_TRIAGE.md`: every note gets a
root cause with evidence before anything is written, and the batch order is driven
by which files collide, not by how annoying each symptom is.

Confidence is stated per row and is load-bearing. One note (the 2p self-deal draw)
is deliberately NOT scheduled for a fix, because the investigation could not
reproduce it and a guess would be worse than an open ticket.

## The notes

| # | Note | Root cause | Conf | Batch |
|---|------|-----------|------|-------|
| 1 | Flipped trump card should sit centered under the deck, like the deck count | Layout only. Trump card is anchored independently of `FDeckWell`'s count label | high | 11 |
| 2 | 2p games need a lobby, else the creator can reroll for a good hand | `startGenesis` (MessagesRootView) deals LIVE immediately on create, so the creator sees their hand before committing and can tap New game until it is good | high | 9 |
| 3 | The chat preview bubble shows the wool better than the extension does | The bubble supersamples wool 900x585 into an exact-aspect 300x195 frame with NO vignette. The live board aspect-fills a 1600x3400 texture into a ~375x554 stage (cropped, off-ratio) and lays a `RadialGradient` vignette to `black.opacity(0.32)` with `endRadius: 700` over it - on a stage that small the vignette never reaches its clear centre, so it muddies the whole surface | high | 11 |
| 4 | **Opening the extension in a different chat shows the previous chat's game. Critical security bug** | `MessagesRootView.load()` reopens `MessageGameStore.games().first` - the whole device cache, unscoped by conversation | high | **8** |
| 5 | Wool does not reach the bottom in expanded (harness only) | Harness `SimulatedStage` sizing, not the extension | high | 11 |
| 6 | Cover animation: attack starts rotated, returns to center, rotates again | `replayLastMoveOnOpen` paints the already-final `GameView` before pre-hiding battle cards, so the cover shows landed; the flight then hides the real card (un-rotate) and lands it (re-rotate). `FBattleGrid.pair()` keys rotation on `coverLanded = !hidden.contains(...)` | high | 10 |
| 7 | Wooden buttons should say "good", the checkbox is only for the status icon | Batch 4 put `FCheck` on the action button as well as the status pip | high | 11 |
| 8 | Collapses mid-animation when several animations are queued | `MessagesViewController.stage()` sleeps a hardcoded 900ms then collapses. Real sequences run ~0.55s per step and a bout end plays one step per drawing player, so multi-step sequences routinely exceed 900ms | high | 10 |
| 9 | Self-deal draw does not animate in 2p when the opponent hits good | **UNRESOLVED.** Kernel `refill_player_hands` shows no 2p asymmetry. Best candidate is an empty `openReplayNewHandCards` when `prevPayload` is nil, but no deterministic repro was found | low | **deferred** |
| 10 | Discard and draw decks should share a y baseline | Layout only | high | 11 |
| 11 | The animation chain after a "good" replays after hitting send | `insert()` makes the staged bubble the `selectedMessage`; the 900ms collapse then fires `willTransition` -> `present()` with that URL -> `loadKey` changes -> `GameSurface` rebuilds and replays my own move. The cache has not committed yet (that happens in `didStartSending`), so the delta looks fresh | high | 10 |
| 12 | Multi-cover shows all covers landed, then animates them one at a time | Same first-paint cause as note 6, seen on the cover cards instead of the attacker's rotation | high | 10 |
| 13 | Pickup animation on reopen plays sometimes, not always | `adopt()` nils `prevPayload` when the cached bytes equal the adopted chain, so a reopen falls back to the structural heuristic in `openReplayDelta`, which asks only "is the table empty" and has no memory of what it already showed | high | 10 |
| 14 | 3+ player start is awkward: invite offered after inviting; rejoining shows me absent; Start offered when I am not in the game | `lobbySeat()` resolves my seat from the CACHE and never checks the seat appears in the bubble's own `joins`, so a stale invite bubble grants Start and Send invite to someone it does not list | high | 9 |
| 15 | Some viewers see a phantom 8-player game, others 4 | `createWaiting` seals `n_players: 8` as the open-lobby convention. Anyone opening a stale WAITING bubble after the game went LIVE at 4 renders 8 seats, because lobby bubbles are explicitly exempted from Rule P | high | 9 |
| 16 | Lobby is tight; drop "send invite" and "waiting for players" | Follows from note 14's redesign | high | 9 |

## Batch order

Ordered by file collision, not severity, with the one exception that note 4 goes
first regardless.

**8 - chat scoping.** Note 4 alone. Scope `MessageGameStore` by a conversation key
derived from `MSConversation.localParticipantIdentifier`. Lands first because
batch 9 rewrites the same lookups.

**9 - lobby v3.** Notes 2, 14, 15, 16. Decided with the owner:
- 2p uses the same lobby machinery as 3-8p. The creator creates and sends; the
  seed is locked by that first message, so the hand is fixed before anyone can
  see it. The opponent may join, or join and start in one action, and once both
  are in either player may start.
- Creating auto-stages the invite. The "Send invite" button is removed entirely;
  joining auto-stages the same way.
- A stale WAITING bubble adopts a newer LIVE chain from the cache if one exists,
  so it opens the real board instead of a phantom 8-seat lobby. This is Rule P
  extended to lobby bubbles, which today are exempt.
- Drop the "waiting for players" line.

**10 - animation correctness.** Notes 6, 8, 11, 12, 13. Notes 6 and 12 are one
fix (pre-hide the delta's cards before first paint). Note 8 needs a real
completion signal in place of the 900ms sleep; `BoardAnimator.sequenceDepth`
already exists but is currently marked harness-only. Note 11 needs the extension
to recognise its own staged bubble instead of adopting it as new. Note 13 needs
the "already fully seen" case to resolve to an empty event list rather than
falling through to the heuristic.

**11 - layout polish.** Notes 1, 3, 5, 7, 10. Presentation only, no state
machinery, so it lands last where it cannot mask a functional regression.

## Deferred

Note 9 stays open. The kernel shows no 2-player asymmetry in `refill_player_hands`
and no repro was found, so the next step is instrumentation (log
`openReplayFromLog`, `openReplayNewHandCards` and the resolved event list at the
moment it is observed), not a speculative patch.
