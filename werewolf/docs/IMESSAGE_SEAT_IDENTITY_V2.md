# Seat identity v2 — "shouldn't there be an id → seat mapping?"

> **Status: §3's claim tokens are DEFERRED (owner call, 2026-08-18) — out of
> scope for this branch. Do not build them without re-raising the wire-compat
> and privacy-page questions in §3. Everything in §4 is shipped.**

The question this document answers: why doesn't the payload carry a player-id
→ seat mapping, so that it is simply impossible for one player's device to
show another player's cards? Short answer: the mapping every device CAN have
already exists (it is the seat cache, §6.1, now hardened), a UNIVERSAL one
cannot exist in this protocol, and the gap between those two is closable with
a device-secret claim token — a wire change specced below, waiting on an
owner call.

## 1. Why no universal id exists

An id → seat mapping needs an id that (a) every device computes the same way
for the same human, and (b) another human cannot claim. Serverless iMessage
offers no such thing:

- **No accounts, no server** (§1): nothing mints or verifies identities.
- **Apple's participant UUIDs are scoped per-device-per-conversation** (§6):
  Vera has a different UUID on her own phone, on Alex's phone, and on her own
  iPad. Writing one into the payload is writing a value nobody else can
  compare against anything. That is why §6 says they never enter the wire.
- **Nicknames** are self-reported display strings.

So the payload's `joins` list — `{seat, name}` — IS the id→seat mapping, with
the only id that exists on the wire: a name. Everything below is about how
far a device may trust it, and with which local secret it can do better.

## 2. The mapping that does exist, and today's guarantee

Each device holds a private mapping: `game_id → my seat` (the App Group
cache, written at the moment of the claim — the one moment a device knows its
seat with certainty). After the double-Start hardening, every resolution path
on a board is EXACT or gives up publicly:

| path | why it is exact |
| --- | --- |
| cache, in range, joins name at my seat == the name my row recorded | my own claim, verified against the chain in hand |
| S1 sender ("this device sent the tapped bubble") | I sealed `last_actor_seat` myself |
| DM 2-player inference (`chatIsDM` gated) | exactly two humans hold phones in a DM |
| anything else | **Release: public spectator board** — never a guess, never a hand |

The DEBUG seat picker is compiled out of Release. So an honest client can no
longer be shown another player's hand by mis-resolution. What survives:

- **R1 — forked same-name claims.** Two chains fork; "Alex" claims seat 2 on
  one, a DIFFERENT human also named "Alex" claims seat 2 on the other; one
  fork wins. The name check cannot tell the two Alexes apart. Per-chain name
  uniqueness is now enforced at Join (`NicknameGate.isTaken` — the Join
  button refuses a name the lobby already lists), so R1 additionally requires
  the collision to happen ACROSS forks. Narrow, not zero.
- **R2 — reinstall/second device.** The private mapping is gone; recovery is
  exact-signal-or-spectator. Safe, but a stranded spectator is bad UX.
- **R3 — adversarial peeking.** The seed rides in every payload (§4), so any
  modified client can compute every hand. No id→seat mapping — no CLIENT-side
  anything — can fix R3: it is a property of "the message is the whole
  game". §15's commit-reveal (fair-deal flag, per-seat secrets) is the real
  answer and stays deferred; §17.8's casual-only framing is the posture.

## 3. Claim tokens — the v2 design that closes R1 and R2 exactly

The id that CAN exist is a *self-recognizable* one: a value only the claiming
device can produce, opaque to everyone else, stable across reinstall.

- **Device secret**: 32 random bytes, generated once, kept in the KEYCHAIN
  (survives app deletion; never leaves the device; never enters the wire).
- **Claim token**: `first8(HMAC-SHA256(device_secret, game_id ‖ seat))`,
  sealed into the join: `{seat, name, token8}`.
- **Resolution**: the cache check becomes "recompute the token for (game_id,
  my seat); does the chain's join carry it?" — exact across any fork, any
  duplicate nickname (closes R1). A cache-less reinstall scans the joins,
  recomputing tokens per seat — exact recovery at any player count (closes
  R2, and would let the DM-only inference retire entirely).
- **What it deliberately does not do**: cross-device identity (an iPad has a
  different secret — same as the cache today, §6's picker/spectator remain
  the answer), and R3 (the seed is still in the payload).

**Why it is not in this commit — two owner calls:**

1. **Wire compatibility.** Joins gain 8 bytes: FMSG format 3. Format-2
   payloads live in real threads today (1.0(3) is shipped), so the decoder
   must accept both formats AND the encoder must keep emitting format 2 until
   the fleet can read 3 — a staged rollout with a version gate, exactly the
   class of change docs/IMESSAGE_BODY_CODEC.md warns against doing casually.
2. **Privacy story.** The keychain secret persists after app deletion; the
   shipped privacy page's storage story (round-7) says what is stored where
   and would need a line. A lighter intermediate — mirroring just the seat
   cache into the keychain, no wire change — buys R2 (durability) without
   R1, and carries the same privacy-page implication.

## 4. What landed now

Per-chain nickname uniqueness (`NicknameGate.isTaken`, wired into LobbyView's
Join and defensively into `joinLobby`): every identity check that keys on
names — the ghost-seat disown, the §6.3 picker, the lobby's "(you)" tag —
is now sound within any single chain, shrinking R1 to the cross-fork
duplicate-name case that only tokens can close.
