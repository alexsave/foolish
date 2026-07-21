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
