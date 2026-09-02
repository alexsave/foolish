# iMessage lobby v2 — open-count groups, session-per-game, FINISHED via /m/

> **SUPERSEDED by `IMESSAGE_LOBBY_V3.md`.** Round-2 testing found four defects in
> what this document specifies: a DM could reroll its deal (v2's "genesis dealt
> immediately" below is exactly that hole), the lobby trusted the cache over the
> bubble being viewed, a stale WAITING invite rendered a phantom 8-seat game, and
> the "Send invite" button offered to re-do what had just been done. v3 routes
> every chat shape through one lobby and deletes that button. Kept for the
> session-per-game and FINISHED-via-`/m/` halves, which v3 does not change.

Batch 6 (docs/HARNESS_NOTES_TRIAGE.md notes 19, 20, 21, 25, 26). Supersedes the
group-lobby half of §5.2/§5.3 in docs/IMESSAGE_GAME_DESIGN.md; DMs (always 2p,
genesis dealt immediately) are unchanged.

## Create / join / start

A DM still deals a 2-player LIVE game the instant you tap Start — no lobby. A
**group chat** now works like this:

1. **Create** locks the game in: a random 32-byte seed + `game_id` are
   generated, the kernel is dealt at the wire's **max capacity, 8**
   (`MessageKernel.newGame(seed:, players: 8)`), then sealed **WAITING** with
   just the creator's own join. The seed is never touched again — that's the
   whole "locked at create" guarantee.
2. **Join** claims the lowest free seat (unchanged) and reseals WAITING.
   Joining **never** starts the game, at any count, including reaching the
   8-seat cap — the old auto-start-when-full branch is gone.
3. **Start** is new and explicit: any already-joined player, once 2+ have
   joined, taps it. The device re-adopts the WAITING chain, re-deals the
   **same** seed at the **actual** joined count
   (`MessageKernel.reseatResidentGame(players:)` — seats are contiguous
   `0..<k`, claimed lowest-free-first), and seals a LIVE handoff: the same
   thing the old "last joiner auto-starts" path did, just triggered by a
   button instead of a seat count.

## Why WAITING's `n_players == 8` means "open"

It's the wire's maximum, not a chosen size — `LobbyView` renders it as an open
lobby (joined list + "N joined"), never as 8 literal seats. The wire has no
cross-check between a child envelope's `n_players` and its parent's: parentage
is only `parent8`, the parent digest's first 8 bytes, carried and read back
but never re-verified against the parent's own bytes. Every envelope re-deals
from its own `seed` + `n_players` on decode (`deal_from_envelope`), so a
WAITING(8) → LIVE(3) transition is not a special case — just two independent,
self-describing envelopes sharing a seed. `c/ios/ios_api_smoke.c`'s
`lobby_v2_reseat_check` and `e2e/msg_lobby_v2.test.ts` prove this end to end:
create → 3 joins (still WAITING/8) → start at 3 → play a move → decode every
leg.

**Caveat, accepted:** the seed lives in the WAITING envelope's own bytes, and
decode is replay for anyone — so a participant can peek at an 8-player deal
from that seed before Start. No worse than the trust already accepted for seat
identity (§6.3); casual-game trust, not cheat-resistant. It also reveals
nothing about the game actually played: Start re-seeds a *different* deal at
the real count, not a trim of the 8-player one.

## Session-per-game (item A)

`stage()` reused `conversation.selectedMessage?.session` unconditionally, so a
game's own turns collapse into one bubble — but reusing it for the **first**
bubble of a **new** game folded the just-finished game's result card into it,
erasing it from the transcript. Fix: `startingNewGame` (set on the New game
tap, cleared by `didReceive`/`didStartSending`) gates it — `session:
startingNewGame ? nil : conversation.selectedMessage?.session`. One session
per game; a new game never collapses the previous game's final bubble.

## FINISHED bubble: `/m/` + the web funnel (item B)

The FINISHED bubble's URL is now a normal `/m/` payload link, not the bare
`foolish.cards/<code>` replay link — that link has no `/m/1<base32>` shape, so
a receiver tapping the final move got the damaged-link screen instead of the
finished board. The funnel moves one hop out: `src/app/m/[payload]/page.tsx`
already decodes any payload through the kernel; for a FINISHED one it also
derives the replay code from what it just decoded (`kernelResidentReplayCodeV6`
— no re-marshal, the decode already left the session log resident) and shows
a "Watch the replay" CTA beside the install/play ones, falling back to a plain
game-over banner if derivation ever fails.
