# iMessage lobby v3

Supersedes `IMESSAGE_LOBBY_V2.md`. Driven by round-2 harness notes 2, 14, 15 and
16 (see `HARNESS_NOTES_R2.md` for the triage and root causes).

v2's shape was right - an open lobby with no player-count picker, the count
decided at Start. v3 keeps that and fixes four things testing exposed: a DM
could reroll its deal, the lobby trusted the cache over the bubble in front of
it, a stale invite rendered a phantom 8-seat game, and the invite button offered
to do again what had just been done.

## One path for every chat shape

v2 split on `chatIsDM`: a DM dealt LIVE immediately (`startGenesis`), a group
opened a lobby. That split was the reroll hole. The creator of a DM game saw
their hand *before* anything committed, so tapping New game until the deck
favoured them cost nothing and left no trace.

v3 routes every chat shape through `createWaiting`. Creating locks the seed and
the game id and seals a WAITING bubble seating only the creator. Nobody is dealt
in until Start. `startGenesis` is deleted rather than left orphaned.

The owner's framing, which the implementation follows literally:

> 2p - creator creates the game and sends the first chat. The other player can
> join, or do join+start. Either one. If they do just join, then either the
> creator or the other player can start the game in a follow up text. either
> way, it should be the same hand because the seed was set by the first chat the
> creator sent.

Lobby capacity is the one thing that still varies by chat shape: 8 for a group
(the wire's max, the open-lobby convention), 2 for a DM. A DM has exactly two
people in it, so "all seats taken" must read correctly once the single possible
opponent has joined, rather than "waiting for 6 more".

## Two routes to Start, provably one deal

- **Join, then Start.** The joiner reseals WAITING with their claim. Any joined
  player then taps Start, which re-derives the locked seed at `joins.count`.
- **Join and start.** The joiner reseats and seals LIVE directly off the
  creator's lobby chain. Their join never exists as its own WAITING bubble. One
  text instead of two.

Both call `MessageKernel.startFromLobby`, which re-adopts the lobby chain,
reseats to `joins.count`, and seals the LIVE handoff. The deal therefore depends
only on the seed carried by the lobby chain and the final join count - never on
which UI path assembled the roster or when it was sealed.

`MessageLobbyTests.testJoinThenStartAndJoinAndStartDealIdenticalHands` compares
both routes' per-seat hands.

### Why `startFromLobby` re-adopts first

The `decode(payload: lobbyPayload)` at the top of `startFromLobby` is
load-bearing and easy to mistake for redundant. The extension is a single
resident kernel that every chat, lobby and board decodes through, and decoding
adopts (§7.3) - so by the time a human taps Start, the resident game routinely
belongs to something else entirely. Without the re-adopt, Start reseats whatever
happens to be resident and deals from the wrong seed, silently.

The route-equivalence test above does NOT catch this: its two routes run back to
back off the same already-resident seed, so deleting the re-adopt leaves it
green (confirmed by mutation). `testStartFromLobbyReDerivesTheLockedSeedNotThe
ResidentGame` exists specifically to pin it, by polluting the resident kernel
with an unrelated game at a different seed and seat count before starting.

## The lobby reads the bubble, not just the cache

`SeatIdentity.resolve` answers "who does the cache or sender signal say I am".
That is correct for a live board, where every chain in a game carries every
seated player forward. It is wrong for a lobby, where an older WAITING bubble
predates a join that has since happened.

The symptom (note 14): join, leave, reopen the original invite, and the lobby
does not list you - yet still offers Start and Send invite, because the cache
still resolved your seat. You were simultaneously not in the game and able to
start it.

`SeatIdentity.resolveInLobby` gates the resolved seat on that bubble's own
`joins`. A seat is mine only if the bubble I am looking at says so.

## Rule P extended to lobby bubbles

v2 returned early for `env.phase == 0`, skipping Rule P entirely, on the
reasoning that every lobby sits at round 0 / turn 0 so the round/turn comparison
is meaningless. It is - but staleness is not.

A WAITING invite stays tappable in the transcript after the game has gone LIVE,
and every WAITING envelope renders as an open lobby at its capacity. So tapping
an old invite for a game that started at 4 showed a phantom 8-seat lobby, and
different people in the same thread saw different player counts depending on
which bubble they happened to tap (note 15).

v3 consults the cache on phase-0 bubbles: if a chain for the same game is cached
at a strictly later phase, adopt that instead. The stale invite opens the real
board. No cached row, or nothing past WAITING, and the bubble really is the
lobby - unchanged. The comparison is `MessageGameStore.lobbyCachePreferred`.

Note this depends on the chat scoping added alongside it: the cache lookup is
keyed by game id *and* chat key, so a lobby can never be superseded by a
same-id row belonging to a different conversation.

## The invite button is gone

Creating auto-stages the invite; joining auto-stages the reseal. The human's
next tap is Messages' own Send, which is the only send that ever existed
(§11.4 - staging never auto-sends).

v2 additionally offered a "Send invite" button that staged the same bubble
again, including immediately after creating, which is what the tester hit:
"the original creator still gets the option to send invite even after sending.
The creator already sent an invite!" The button is removed, not conditionally
hidden.

The "Waiting for players - N joined" line is also gone (note 16). The joined
list directly above it already says exactly that, and the screen was tight.

## What did not change

- The kernel decides everything about the game. Swift orchestrates.
- Seats are claimed lowest-free-first, so a started game's seats are contiguous
  `0..<k`.
- Any joined player may Start once 2+ have joined.
- The joined list is the player count so far. There are no open-seat
  placeholders, because there is no fixed count to fill.

## Racing Starts (the 4-player deadlock) — Rule P rule 3

"Any joined player may Start" has a race v3 shipped without an answer for: two
players tap Start off different views of the lobby — or one taps it off a
stale bubble that predates the last join — and the thread now holds TWO LIVE
handoffs, both round 0 / turn 0, dealt from the same locked seed at DIFFERENT
player counts. Those are different games: a different flip, a different trump,
a different first attacker (the deal is player-major off one shuffled deck, so
the HANDS of the shared seats even match — only the flip and stock move —
which made the fork nearly invisible).

Rule P as shipped compared (round, turn, digest): the two Starts tied to the
digest — a coin flip between two different games. Measured over 2000 seeds of
the 4-players-joined / one-Start-off-the-3-join-view race: the 3-player fork
won 1008 times, the forks disagreed about the first attacker 1334 times, and
in 149 the full game's first attacker was exactly the player whose own device
sat on the small fork's board — every screen in the chat waiting on a player
whose own screen shows the attacker position on somebody else (their
right-hand neighbour, in the shipped report) and offers them no legal move.
The last joiner is stranded in every 3p-fork win too: their cached seat 3 is
out of range of a 3-player game, which Release renders as the spectator board.

Two fixes, one in each layer that owned a piece of the hole:

- **Kernel — rule 3 (`msg_wire.h`)**: at an equal (round, turn), MORE JOINS
  wins, before the digest. Every device now resolves a Start race to the
  fullest roster, deterministically. It sits BELOW turn on purpose: a chain
  someone has actually played on is never clobbered by a stale wider Start
  sealed after the fact. It also orders WAITING chains among themselves (a
  3-join lobby beats the 2-join lobby it grew from), which is what lets the
  arrival path below refresh a lobby roster instead of coin-flipping against
  its own cached invite. Pinned in `c/tests/msg_wire_test.c`
  (test_rule_p_fuller_start_wins), `e2e/msg_lobby_v2.test.ts` (the wasm the
  web replays through), and `MessageLobbyTests.testFullerStartBeatsAStale
  SmallerStart` (the xcframework binding).

- **Extension — adopt on arrival (`GameSurface.maybeAdoptIncoming`)**: Apple
  does not make a `didReceive` arrival the `selectedMessage`, so the surface
  never reloaded for it — the losing starter's device stayed on its fork's
  board until the human happened to re-tap a bubble. The arrival is now
  threaded through as its own input and adopted iff Rule P says it strictly
  out-ranks what is showing; a stale or duplicate arrival changes nothing (no
  teardown, no replay). The same path live-refreshes an open lobby when a
  join arrives.

What rule 3 deliberately does NOT solve: a single Start off a stale bubble
while the last join is still in flight produces only ONE started chain, and it
wins (rule 0 — started beats lobby, which is what prevents the round-3
deadlock). The player whose join lost that race stays a spectator of a game
that started without them; serverless, nobody can know a join is "in flight".
Round-5 M9's authorship gate (the newest bubble's sender cannot Start while
the lobby has room) already narrows that window; the full answer would need a
transport with ordering, which iMessage is not.
