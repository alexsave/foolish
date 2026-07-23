# Correspondence Durak in iMessage — Full Design & Implementation Spec

*A zero-context, implementation-ready design for turning Foolish into an iMessage
game. Written so an engineer (or LLM) who has never seen this repository can
build it end-to-end. Every claim about existing code carries a `file:line`-style
anchor verified against the repo on 2026-07-13. Business context lives in
`docs/ORACLE_MONETIZATION_ENGINEERING.md` §7.5; this doc is pure engineering.*

---

## Table of contents

1. [Context for the zero-context reader](#1-context)
2. [What we're building (product spec)](#2-product-spec)
3. [The key design insight: seed + action list, not a codec surgery](#3-key-insight)
4. [Payload format `FMSG` v1 (byte-level spec)](#4-payload-format)
5. [The message protocol (state machine)](#5-message-protocol)
6. [Identity & seats without a server](#6-identity--seats)
7. [The concurrency model: concurrent moves, forks, and rebase](#7-consistency)
8. [The native engine library (C → iOS)](#8-native-engine)
9. [Xcode project structure & the host-app decision](#9-xcode--host-app)
10. [The extension UI (SwiftUI spec)](#10-ui-spec)
11. [Messages framework wiring (exact APIs)](#11-messages-wiring)
12. [Game end: bridging back into the ecosystem](#12-game-end)
13. [Web fallback route `/m/`](#13-web-fallback)
14. [Concurrency worked examples (incl. pickup ∥ attack)](#14-multiplayer)
15. [Fair-deal protocol (optional, spec'd now, build later)](#15-fair-deal)
16. [Format 6: the true partial-game rANS codec (optional optimization)](#16-format-6)
17. [Gotchas catalog (read before writing any code)](#17-gotchas)
18. [Test plan](#18-test-plan)
19. [Milestones with acceptance criteria](#19-milestones)
20. [**Implementation handoff — repo state 2026-07-15**](#20-handoff) ← start here

---

## 1. Context

**The game.** Durak ("fool") is a Russian card game for 2–8 players, 36-card
deck by default. One player attacks with cards; the defender must beat ("cover")
each attack card with a higher card of the same suit or a trump; covered rounds
discard, failed defenses are picked up. Last player holding cards is the fool.
Turns are discrete and the rules engine fully determines whose move it is —
which is what makes asynchronous, message-at-a-time play possible.

**The repo.** `foolish.cards` is a Next.js web client + Supabase backend, but
the domain logic has exactly ONE implementation, in portable C, that runs
everywhere (see `docs/ARCHITECTURE_AS_A_PATTERN.md`):

| Piece | Where | What it gives this project |
| --- | --- | --- |
| Rules kernel (deal, legality, apply, views) | `c/src/game.c`, `legal.c`, `view.c` | The complete game engine, compiled to wasm today; compiles natively for iOS tomorrow |
| **Seeded deals** | `c/src/game.h:139-151` — `game_set_deal_seed_bytes(seed,len)`: ChaCha shuffle over the full 52!/36! space, whole game reproducible from the seed; state records `deterministic_deck` | A 16-byte seed replaces the entire hidden deal — the foundation of the payload format |
| Durable state blob v2 | `c/wasm/wasm_api.c:316-327` — `wasm_state_serialize/deserialize` (carries the deal seed) | Not used in the payload (see §3) but useful for local caching |
| Replay codec v5 (finished games → short URL) | `c/src/replay.{h,c}` (C is canonical; frozen wire format), TS bridge `server/api/common/replay/` | The game-over artifact: a finished iMessage game becomes a normal `foolish.cards/<code>` replay, Oracle-analyzable |
| Packed action wire format | `server/impls/supabase/functions/_shared/packed_action.ts`, `c/wasm/wire.h` | The per-move byte encoding the payload reuses verbatim |
| Kernel-in-browser bridge | `src/wasm/clientGuards.ts`, `sdk/ts/wasm/engine.ts` | The web client already loads the C kernel as wasm — the `/m/` fallback route rides this |
| Full export list of the wasm rules build | `c/Makefile:331-353` | Shows every kernel entry point already exposed (`wasm_start_game`, `wasm_apply_action`, `wasm_legal_moves`, `wasm_replay_encode`, …) |

**The constraint that shapes everything:** an iMessage extension has *no server
in the loop*. Each turn is an `MSMessage` whose **URL payload must carry the
entire game**. Apple's Messages infrastructure is the transport, storage, and
notification system. The extension only runs while the user is looking at it.

---

## 2. Product spec

- **v1 supports 2–4 players from day one** (the engine allows 2–8,
  `c/src/game.h:12`; the payload format allows 8; **4 is a v1 UI cap**,
  not a format cap). 1v1 in a DM and 3–4 players in a group thread are the
  same protocol — 2p is just the N=2 special case. This is deliberate: Durak
  is a **multi-actor game** (several players can legally act at the same
  moment — see §7), so the concurrency machinery must exist even for 1v1, and
  retrofitting N-player onto a turn-alternation protocol later would mean
  redesigning the payload, the identity rules, and the consistency model. We
  build it once, now.
- Flow: player A opens the Foolish iMessage app in a conversation → taps
  **New game (2/3/4 players)** → a game bubble appears in the thread. In a DM
  the game starts immediately; in a group, joiners tap the bubble and claim
  seats (§6) until full, then play begins. Tapping the bubble opens the table
  from your seat's perspective; play your move(s); a new bubble replaces the
  old one (same `MSSession`) — until someone is the fool.
- The bubble always shows: a rendered snapshot of the public table, whose turn
  it is, and turn number. `summaryText` (the fallback/notification line) says
  what happened: "Alex attacked with 7♠".
- Game over → the bubble becomes a result card with a **standard replay link**
  (`https://foolish.cards/<code>`) — watch, share, and (in the main app / on
  web) run the Infinite Oracle on it.
- Recipients without the app: iPhone users get the App Store install sheet when
  tapping the bubble; macOS/Android/SMS recipients see the raw URL, which is a
  real web page (§13) rendering the game read-only with install/play CTAs.
- **Casual-only, unrated.** Hidden information (opponent hand, deck order) is
  *encoded in the payload*, so a motivated user can decode and peek. This is
  accepted for friends-casual (GamePigeon's Sea Battle has the same property)
  and is why rated play must stay on the server. Show a one-time notice:
  "Friendly game — fair play on the honor system."
- No accounts, no login, no IAP, no network calls in v1. The extension works
  fully offline. (This also makes App Review trivial.)

---

## 3. Key insight

### 3.1 Why the existing replay codec cannot carry a mid-game state

The v5 replay codec (`c/src/replay.c`) encodes a **finished** game. Its
decoder replays the game from the start and learns hidden cards lazily: each
reveal is entropy-coded against a hypergeometric model, and the cards the loser
never played are recovered **by complement once the fool is known** — the
format literally has an error for a stream that ends early:
`REPLAY_EINCOMPLETE (=16) — "logs ended before the fool was known"`
(`c/src/replay.h:56`). There is no way to terminate a v5 stream mid-game;
the trailing backfill step is load-bearing. So the original intuition is
correct: v5 cannot encode a partial game, and *extending v5 itself* would be
surgery on a frozen wire format.

### 3.2 …and why we don't need to touch it

A partial game does not need the reveal machinery at all. In serverless
iMessage play, **both devices must know the full deal anyway** (each device
continues the game locally, including drawing from the deck). Since the kernel
already supports fully deterministic seeded deals —
`game_set_deal_seed_bytes()` (`c/src/game.h:139-151`) — a mid-game state
is exactly:

```
partial game ≡ (deal seed, ordered list of actions applied so far)
```

Decode = fresh game → set seed → deal → `start_game` → apply the actions one
by one through the kernel's legality check. This is ~microseconds of C per
open, reconstructs *everything* (state, per-move history, logs for the Oracle,
eventual v5 encode), and — crucially — **re-validates every action on every
device on every open**, so a tampered payload is detected for free (an illegal
action simply fails to apply; see §7.3).

So: **no rare codec update is required for v1.** The "real" partial-game rANS
format (seed + entropy-coded moves, smaller still) is spec'd as an optional
compression in §16, to be built only if payload sizes demand it (§4.4 gives
the numeric trigger).

---

## 4. Payload format

> **⚠ Correction (2026-07-15):** the deal seed is **32 bytes**, not 16 —
> `game_set_deal_seed_bytes` requires `len >= 32` (`c/src/game.c`). Read
> every "16" about the seed in this section as 32; the reference layout in
> `IMESSAGE_IMPLEMENTATION_HANDOFF.md` §3.1 is authoritative.

### 4.1 Envelope `FMSG` v1 (binary, little-endian, before text encoding)

A new, small, versioned envelope. Implemented **in C** — single source of truth,
like everything else in this repo — as `c/src/msg_wire.{h,c}`, compiled
into (a) the iOS static library (§8) and (b) the existing wasm rules build with
two new exports `wasm_msg_encode` / `wasm_msg_decode` added to the export list
at `c/Makefile:331-353`, bridged to TS in
`sdk/ts/wasm/engine.ts` for the web route (§13) and e2e
tests (§18).

```
offset  size  field            notes
0       1     magic      0xF7
1       1     format     1
2       1     flags      bit0 fair_deal (§15)  bit1 gzip-body  bits2-7 reserved=0
3       1     phase      0=WAITING (seats unclaimed)  1=ACCEPT (fair-deal only, §15)
                         2=LIVE  3=FINISHED
4       8     game_id    random u64, generated at creation, constant for the game
12      2     turn       u16, count of kernel actions applied (0 = fresh deal)
14      1     last_actor_seat  seat (0-based) of the player who SENT this message
15      1     n_players  2..8 on the wire (creator fixes it at creation; UI caps 4 in v1)
16      1     variant    reserved rules-variant byte, =0 (36-card podkidnoy defaults)
17      1     round      u8, count of completed rounds in the chain (== number of
                         discard/pickup round-closures; used by the rebase guard, §7.4)
18      8     parent8    first 8 bytes of SHA-256 of the previous envelope's
                         bytes (all zeros for the creation message). §7
26      16    seed       deal seed → game_set_deal_seed_bytes(seed,16).
                         All zeros only in fair-deal WAITING/ACCEPT (§15)
42      1     n_joins    seats claimed so far (creator counts: >=1)
43      var   joins      n_joins × { u8 seat, u8 name_len (<=64), name utf8 }
                         ordered by claim time; creator is always seat 0 (§6)
var     2     n_actions  u16
var     3×n   actions    packed kernel actions, 3 bytes each (§4.2)
var     var   [fair-deal extras, only when flags.bit0 — §15]
```

Everything a device needs is here; there is deliberately **no state snapshot**
in the payload (state is derived) and no participant identifiers (they don't
transfer across devices — §6). Nicknames are the only self-reported identity:
typed once at create/join, max 12 UTF-8 bytes, shown as seat labels ("waiting
for Sveta") and used for seat recovery (§6.3). The `joins` list is
protocol-layer data, not kernel actions — the kernel replay never sees it.

### 4.2 Action encoding (3 bytes/action)

> **⚠ Superseded (2026-07-15):** actions are stored as **seat-prefixed awire
> frames** (`u8 seat` + the self-delimiting awire frame), not fixed 3-byte
> triples — chains interleave DIFFERENT seats' actions, and multi-card moves
> are single kernel actions with n>1. See
> `IMESSAGE_IMPLEMENTATION_HANDOFF.md` §3.2; the paragraph below stands only
> for the card-byte conventions it cites.

Reuse the existing packed-action wire encoding — do **not** invent a new one.
The canonical definitions are `server/impls/supabase/functions/_shared/packed_action.ts`
(TS) and `c/wasm/wire.h` (C): action type byte + 1-byte wire cards
(`0..51 = suit*13+(value-1)`, `0xFE` hidden, `0xFF` none — `replay.h:31-36`
documents the same card byte). `msg_wire.c` stores each action as
`{u8 type, u8 card_a, u8 card_b}` using those exact byte conventions; multi-card
actions (multi-attack, multi-pass) are stored as consecutive single-card
actions of the same type where the engine's apply path supports it, otherwise
as the packed multi-action form already defined in `packed_action.ts` — the
implementer MUST read that file first and mirror it, not approximate it.

### 4.3 Text encoding & URL

`bytes → base32` using the codec's existing base32 (same alphabet/util as
`server/api/common/replay/codec.ts` — QR-alphanumeric-safe, already
proven in URLs). URL shape:

```
https://foolish.cards/m/1<base32-payload>
```

`/m/` is the new web route (§13); the leading `1` is the text-level format
version so the route can dispatch before decoding binary. The full URL is also
set as the `MSMessage.url`, which is exactly what non-iMessage recipients see.

### 4.4 Size budget (measured targets, with the escape hatch)

Header+seed = 44 bytes. A long heads-up game is ~60–90 actions → 224–314 bytes
→ **~360–510 base32 chars**. Median mid-game turns land ~200–350 chars. Turn
bubbles are never QR'd, so the only limits are Messages' URL handling and web
fallback ergonomics. **Guardrails:** e2e test asserts P95 full-game envelope
< 1,000 chars; if real-world games breach it (they shouldn't), that is the
trigger to build §16. gzip (flags.bit1) is specced but expected OFF — at these
sizes it rarely pays; keep the bit reserved so it can be enabled without a
format bump.

---

## 5. Message protocol

### 5.1 A truth to internalize first: Durak has no single "next actor"

This engine is faithful to podkidnoy: while the defender is deciding, any
eligible attacker may throw in more cards; several non-defenders may hold
"awaiting" status at once (`awaiting_attack` flags, `c/src/game.c:591,707`);
the defender may cover or pick up at any point. **Multiple seats can have
non-empty legal-move sets simultaneously — including in 2-player games** (the
attacker can legally add a card at the same moment the defender can legally
pick up). So the protocol below is NOT turn alternation. The rule is:

> **Staging rule:** whenever the kernel gives *your* seat a non-empty legal-move
> set, you may play. Apply your chosen action(s) locally, then stage ONE message
> carrying the full updated chain. Concurrent messages from other players are
> resolved by §7 — never by assuming you were the only one moving.

`turn` counts kernel actions, not messages; one message may carry several
actions by its sender (attack two cards, then "done").

### 5.2 State machine (simple mode — fair_deal off, the v1 default)

```
Creation (player A taps "New game", picks n_players and a nickname):
  A's device: game_id=rand64, seed=rand128, joins=[{seat 0, "Alex"}]
    n_players == 2 (DM): deal + start_game now → phase=LIVE; A may also
                         immediately play own first actions before staging
    n_players >= 3:      phase=WAITING (deal computed lazily — the seed is
                         fixed, so dealing early or late is equivalent)
  A stages the bubble, A taps Messages' send (extensions can't auto-send, §17.1)

Joining (phase=WAITING, group thread):
  each joiner taps the bubble → types a nickname → device appends
  {next_free_seat, name} to joins, stages the updated WAITING bubble → sends.
  The joiner whose claim FILLS the last seat also deals (seed is already in
  the envelope), applies nothing, and stages phase=LIVE: "Game on — Sveta
  attacks first" (kernel picks first attacker: lowest trump).

Playing (phase=LIVE):
  any player, on opening the current bubble:
    decode → fresh kernel game → set seed → deal → replay all actions
    (validating each) → render own seat's view
    if my seat has legal moves → I may act (§5.1 staging rule)
    if not → spectator view: "waiting for Sveta / Boris"
  staged message: turn+=k, round updated, parent8=digest(prev), last_actor_seat=me

Finish: whoever applies the terminal action stages the FINISHED bubble (§12).
Laggards who open stale bubbles afterwards get §7's staleness handling.
```

- **One bubble per game** via `MSSession` — Messages collapses older messages
  in the same session to their `summaryText` and keeps the latest interactive
  (§11.3). The thread never fills with 60 bubbles.
- Seat-claim messages (WAITING) intentionally contain zero kernel actions, so
  join races are trivially mergeable: two simultaneous claims of seat 2 fork
  the joins list only; §7's preference rule picks one, and the loser's device
  re-claims the next free seat automatically on next open.

### 5.3 What each phase means

| phase | contents | staged by | UI on open |
| --- | --- | --- | --- |
| WAITING (0) | seed + joins (no actions) | creator, then each joiner | lobby: claimed seats by nickname, "join" button |
| ACCEPT (1) | fair-deal handshake only | see §15 | — |
| LIVE (2) | seed + joins + actions | any player with legal moves | table from viewer's seat; inputs enabled iff kernel grants viewer legal moves |
| FINISHED (3) | seed + joins + full actions | player who applied terminal action | result card + replay link (§12) |

---

## 6. Identity & seats

There are no accounts. Apple gives the extension **opaque participant UUIDs**
(`conversation.localParticipantIdentifier`, `MSMessage.senderParticipantIdentifier`)
with a brutal caveat: **they are scoped per-device-per-conversation** — the
same human has *different* UUIDs on their own iPhone vs iPad, and on their
opponent's device. Therefore participant UUIDs never go in the payload. Seat
identity uses three layers, in order:

### 6.1 Primary: the App Group cache

At create/join time the device knows its seat with certainty (it just claimed
it). Store `{game_id → mySeat}` in App Group storage (§9.3). This answers the
question for the life of the install.

### 6.2 Secondary: sender inference (exact, but only for the last actor)

```
Rule S1: on opening a bubble,
  if selectedMessage.senderParticipantIdentifier
     == conversation.localParticipantIdentifier
  then mySeat = envelope.last_actor_seat        // exact, any N
  else if n_players == 2
  then mySeat = 1 - envelope.last_actor_seat    // exact for 2p
  else fall through to 6.3                      // N>=3, cache lost, not last actor
```

### 6.3 Tertiary: nickname recovery (N≥3 after reinstall)

The joins list carries every seat's nickname (§4.1). If the cache is gone and
S1 can't decide, show a one-tap picker: "Which player are you? — Alex / Sveta /
Boris". Wrong self-identification lets a user look at another seat's hand in a
casual game among friends; acceptable, same trust level as passing the phone.
(A cheat-resistant version is the per-seat secret commitment sketched in §15's
appendix — server-free, but adds UX weight; not v1.)

- Seat 0 is always the game creator; joiners take the lowest free seat at the
  time their claim lands in the canonical chain (§5.2, §7).
- **Same-user-multiple-devices:** iPad has no cache; S1 or the picker recovers
  the seat, then that device caches it too.
- **Spoofing** (editing a payload to move for another seat) is detectable only
  as "actions by seat X in a message sent by not-X" for the tapped message.
  Accepted for casual play; noted in §17.9.

---

## 7. The concurrency model

This is the heart of the design. Read §5.1 first: multiple players can legally
act at the same moment, in every game size. Two players WILL compose messages
against the same parent state — the canonical example being **the defender
sends "pick up" at the same instant an attacker sends a throw-in attack**. The
model below makes every such race deterministic, convergent, and rules-faithful,
with no coordinator. Mental model: **git**. Each bubble is a self-contained
branch tip (seed + full action chain); Messages is an unreliable branch pointer;
devices converge by a deterministic chain-preference rule and rebase their own
unmerged moves.

### 7.1 Chains, not diffs

Every envelope carries the FULL action chain from the deal (§4). Consequence:
any single bubble is sufficient to adopt the game — devices never need to have
seen prior messages, and a "lost" concurrent message loses only its *delta*,
which its own author can rebase (§7.4). This property is what makes serverless
concurrency tractable; do not ever optimize the payload into deltas.

### 7.2 The chain-preference rule (deterministic convergence)

Two chains for the same `game_id` are compared as follows — every device
computes the same winner regardless of message delivery order:

```
Rule P (total preference order):
  1. higher round wins                (a closed round is settled history)
  2. else higher turn wins            (more accepted actions)
  3. else lexicographically smaller SHA-256(envelope bytes) wins  (arbitrary but universal)
```

The device's cache stores the preferred chain seen so far. On every open
(`selectedMessage`) or live receive (`didReceive`), compare the incoming chain
to the cached one under Rule P; adopt the winner. Note Apple's session
replacement usually makes the thread's visible bubble the newest delivered —
but Rule P deliberately does NOT trust delivery order, because two devices can
transiently disagree about "newest". Rule P needs no clocks and no ordering
guarantees from the transport.

### 7.3 Validation = replay

Adopting a chain always means replaying every action through the kernel's
legality machinery — a corrupted or hand-edited payload fails loudly (an
illegal action simply won't apply). Error UI: "This game link is damaged."
Never attempt partial recovery. The kernel is already hardened against hostile
bytes (`docs/SECURITY_WASM_BOUNDARY.md`); `msg_wire.c` constructs games only
via public kernel calls, never memcpy into `Game`.

### 7.4 Rebase: no legal move is silently lost

Per `game_id`, the device keeps a small **pending ledger**: the actions *this
seat* has staged/sent, each tagged with the `(round, turn, parent8)` it was
composed against. When the device adopts a preferred chain that does NOT
contain those actions (they lost a race):

```
Rule R (rebase):
  for each pending action, in order:
    if action.round < adopted_chain.round:      DISCARD  (round-boundary guard)
    else if kernel says action is legal now:    RE-APPLY on top of adopted chain
    else:                                       DISCARD
  if anything re-applied → auto-stage the merged envelope,
     UI: "Your move was re-applied on the new state — send to confirm"
  if anything discarded → toast: "Your <move> was superseded (the round ended
     / Sveta picked up first)" — and the hand re-renders from adopted state
```

**The round-boundary guard is essential.** Without it, a rebased action can be
*legal but semantically different*: e.g. the attacker's throw-in composed
against round 5's table would, after the defender's pickup closed round 5
(pickup closes the round immediately and rotates roles —
`c/src/game.c:776-808`), re-validate as an *opening attack of round 6* —
legal per the kernel, but not what the player chose to do. An action never
survives rebase across a round boundary; within the same round, kernel
legality is the arbiter. This rule is small, explainable to users, and
rules-faithful.

### 7.5 The race, resolved end-to-end (defender's pickup ∥ attacker's throw-in)

Both compose against parent P (round 5, turn 20):

- Defender's chain D: P + pickup → round 6, turn 21.
- Attacker's chain A: P + attack(9♣) → round 5, turn 21.
- Rule P: **D wins everywhere** (higher round beats equal turn). This is also
  the right *game* outcome: picking up is the defender's prerogative at any
  moment; the throw-in "didn't make it in time".
- Attacker's device adopts D, rebases attack(9♣): `action.round (5) <
  adopted.round (6)` → **discarded** with "Sveta picked up before your 9♣
  landed." The attacker still holds the 9♣ — nothing is lost but tempo.
- Had the *attack* landed first thread-wise, nothing changes: Rule P is
  delivery-order-independent. Note the symmetric case — attack A adopted by
  the defender's device *before* they send pickup — needs no machinery at all:
  the defender simply picks up one more card (`handle_pickup` takes the whole
  table, including the throw-in).

More worked examples, including 3–4 player races, in §14.

### 7.6 Staleness & lifecycle events

| Condition | Meaning | UI |
| --- | --- | --- |
| incoming chain loses Rule P to cache | user tapped an old (collapsed) bubble | banner "This game has moved on — open the newest message", read-only, offer "show that state anyway" |
| incoming chain wins Rule P | progress (or a fork we adopt) | §7.4 rebase, then play |
| `didStartSending` fires for our staged message | our chain is committed to the thread | cache := our envelope; pending ledger := its unacked tail |
| `didCancelSending` fires | user deleted the staged bubble | roll cache/ledger back to pre-stage snapshot (§17.2) |
| FINISHED chain adopted while we had pending moves | game over won the race | all pending discarded; show result card |

---

## 8. Native engine

### 8.1 New build target: `libfoolish.a`

Add to `c/Makefile`:

```
ios-lib:  # arm64 device + arm64/x86_64 simulator slices, or an .xcframework
  sources: game.c legal.c view.c replay.c deal_rng.c msg_wire.c ios_api.c
  flags:   plain clang -O2 for Apple targets; NO wasm flags; C11; no OMP
```

The wasm builds prove these sources are dependency-clean (`-nostdlib
-ffreestanding` at `c/Makefile:287`); natively they may use libc freely
(they already do for native builds — `cordite_sim.c` includes stdio/stdlib for
the research binaries, but note the **bot/strategy sources are NOT in this
library** — the iMessage v1 ships rules only, no bots, keeping the extension
binary tiny).

`ios_api.c` is a thin shim in the spirit of `c/wasm/wasm_api.c` (which is
wasm-specific — IO buffer indirection, `__builtin_wasm_memory_grow`; do not
compile it for iOS). The shim owns one static `Game` and exposes exactly:

```c
// all return 0 on success / negative error codes mirroring wasm_api conventions
int  fio_new_game(const uint8_t seed[16], int n_players);   // set_deal_seed + deal + start
int  fio_apply_action(uint8_t type, uint8_t card_a, uint8_t card_b);
int  fio_legal_moves(uint8_t *out, int cap);                // packed 3-byte moves, returns count
int  fio_actor_seat(void);                                  // whose move; -1 if game over
int  fio_view(uint8_t viewer_seat, uint8_t *out, int cap);  // masked per-viewer view (reuse view.c / put_state conventions)
int  fio_game_over(void);                                   // fool seat or -1
int  fio_msg_encode(/* envelope fields */ ...);             // msg_wire.c
int  fio_msg_decode(const uint8_t *in, int len, ...);       // msg_wire.c (also replays+validates)
int  fio_replay_encode(uint8_t *out, int cap);              // finished games → v5 bytes (replay.c)
```

Swift consumes this through a bridging header (or a small SwiftPM C target).
No Objective-C needed. Memory footprint: one `Game` (~KBs) — three orders of
magnitude under any extension ceiling.

### 8.2 Parity discipline

The envelope and the action encoding exist in C once; wasm exports make the
*identical* bits testable in Node (§18). Golden vectors (hex fixtures of
envelopes at various phases) are committed and asserted equal from (a) e2e via
wasm and (b) an XCTest via `libfoolish.a`. This is the same frozen-oracle
pattern the repo already uses for replay v5 (`e2e/replay_ts_oracle.ts`).

---

## 9. Xcode & host app

### 9.1 The decision Apple forces (read this before creating the app record)

> **Reversed 2026-07-18 (`e9b9120`).** The "choose the bundled form" decision
> below was superseded: the iMessage game now ships as its own standalone App
> Store record (`cards.foolish.msg`, `ios/project.yml`'s `FoolishMessagesApp`
> target) rather than embedded in the host app. The tradeoff/reasoning
> section immediately below is kept for the historical record of why bundled
> was chosen first; `ios/project.yml`'s own comments and
> `docs/IMESSAGE_APP_STORE_SUBMISSION.md` are ground truth for the current
> (standalone) submission model.

Apple: an iMessage app is either **standalone** (no visible iOS app) or an
**extension bundled in an iOS app** — and converting between the two later
means creating a **new app record**
([App Store Connect help](https://developer.apple.com/help/app-store-connect/create-an-app-record/add-imessage-app-information/),
[dev forums](https://developer.apple.com/forums/thread/61482)). Since
`ORACLE_MONETIZATION_ENGINEERING.md` §7 plans a full iOS app whose Oracle the
iMessage game funnels into, **choose the bundled form now**:

> **Decision: create ONE iOS app record ("Foolish — Durak") containing (a) a
> deliberately small but genuine host app and (b) the Messages extension.**

The v1 host app is NOT a stub (stubs risk Guideline 4.2). **Superseded note
(2026-07-13):** the host is now the full native iOS app specified in
`docs/IOS_APP_DESIGN.md` — build that first; this extension ships as a target
inside its app record (`IOS_APP_DESIGN.md` §2 goal 6, §4). The extension's own
targets, protocol, and milestones in this document are unchanged; wherever
this doc says "host app", read "the IOS_APP_DESIGN.md app", and the shared
SwiftUI board/engine bridge comes from its `FoolishKit`.

### 9.2 Targets

```
Foolish.xcodeproj
├─ Foolish            (iOS app target — minimal host, SwiftUI)
├─ FoolishMessages    (iMessage extension target, MSMessagesAppViewController)
├─ FoolishKit         (shared Swift framework: engine bridge, models, board UI)
└─ libfoolish.a / Foolish.xcframework   (from c/Makefile ios-lib)
```

Both app + extension link FoolishKit; FoolishKit links the C library. Bundle
IDs `cards.foolish.app` / `cards.foolish.app.MessagesExtension`.

### 9.3 App Group

`group.cards.foolish` — shared UserDefaults (or one JSON file) for the cache:

```json
{ "games": { "<game_id-hex>": {
    "mySeat": 0, "maxTurnSeen": 24,
    "latestDigest8": "…", "finished": false, "updatedAt": 1720900000 } } }
```

Purely an optimization + game-list source; §6/§7 rules must survive its loss.

---

## 10. UI spec

Two presentation styles (§11.2):

- **Compact (drawer, ~250pt tall):** list of known games from the App Group
  cache (opponent conversation can't be named — show "Game from July 12 · turn
  24 · your move") + a big **New game** button. Tapping New game requests
  expanded style.
- **Expanded:** the table. Layout top-to-bottom: opponent card backs (count),
  table battles (attack card, cover card stacked), trump card + deck count,
  the local hand as a horizontal fan, action bar.
- **Interaction model (keep v1 dumb and reliable):** tap a hand card → if it's
  a legal attack/cover per `fio_legal_moves`, apply optimistically and re-render;
  action bar shows the kernel-driven set of non-card moves: **Pass / Pick up /
  Done (good)** — enabled strictly from the legal-move list. **Send move**
  enables as soon as ≥1 local action is applied (there is no "your turn is
  over" in a multi-actor game — §5.1; a subtle "others may be playing too"
  hint sits next to it, §14). Staging follows §11.4; incoming chains while
  composing are handled by §7.4 rebase. Undo (before staging) = rebuild from
  envelope + replay minus local moves — trivial since state is derived.
- Rendering: pure SwiftUI shapes + SF Symbols (`suit.spade.fill` etc.), white
  rounded-rect cards, red/black glyphs; the procedural fractal aesthetic is a
  later port, NOT v1. Dark mode via system colors. Localize all strings from
  day one — reuse the exact keys/strings from `src/localization/strings.ts`
  (en/ru/ko) so the two clients never drift in terminology.
- Bubble snapshot: render the *public* table (both hands as backs, battles,
  trump, counts) with `UIGraphicsImageRenderer` at 300×195pt into
  `MSMessageTemplateLayout.image` — never include either hand face-up (the
  bubble is visible to both players and in notifications).

---

## 11. Messages wiring

### 11.1 Lifecycle (MSMessagesAppViewController)

| Callback | What we do |
| --- | --- |
| `willBecomeActive(with:)` | grab `activeConversation`; if `selectedMessage` exists → decode & route (§7.2), else show drawer list |
| `didReceive(_:conversation:)` | a new message for our extension arrived *while we're open* — re-decode and refresh (opponent may be live-playing) |
| `didStartSending(_:conversation:)` | commit App Group cache to the staged envelope (§7.2) |
| `didCancelSending(_:conversation:)` | roll cache back; return UI to pre-stage state |
| `willTransition(to:)` / `didTransition(to:)` | compact↔expanded relayout |

### 11.2 Presentation

`requestPresentationStyle(.expanded)` when a game opens or New game is tapped;
Apple requires user-comprehensible transitions — never bounce styles
programmatically mid-interaction.

### 11.3 Composing a turn message (exact recipe)

```swift
let session = selectedMessage?.session ?? MSSession()   // reuse = replace old bubble
let message = MSMessage(session: session)
var comps = URLComponents(string: "https://foolish.cards")!
comps.path = "/m/1\(base32Payload)"
message.url = comps.url
let layout = MSMessageTemplateLayout()
layout.image = tableSnapshot()                 // §10 public snapshot
layout.caption = L("Your move")                // localized; Messages appends sender name context
layout.trailingCaption = L("Turn %d", turn)
message.layout = layout
message.summaryText = L("%@ attacked with 7♠") // the collapsed/notification line
activeConversation?.insert(message) { error in … }   // STAGES it; user must tap send
```

### 11.4 Hard rules

- `insert` only **stages** the message into the input field — the human always
  sends. Design the UI to say "Move staged — hit send!" and handle both
  `didStartSending` and `didCancelSending`.
- Never `insert` twice for one logical turn; disable Send-move until resolved.
- The extension gets NO background execution and NO push — the message *is*
  the notification. Do not attempt timers, polling, or silent updates.

---

## 12. Game end

When `fio_game_over()` reports the fool:

1. Encode the finished game to **standard replay v5** via `fio_replay_encode`
   (the fool is known, so v5's complement backfill is satisfied — this is the
   moment the frozen codec becomes usable). Build the canonical URL
   `https://foolish.cards/<code>` exactly like the web client does.
2. Stage the FINISHED bubble: snapshot shows the final table + "Alex won 🎉 /
   Boris is the fool 🃏"; `message.url` = **the replay URL** (not `/m/`), so
   every tap — on any platform — lands on the existing replay page, where the
   Infinite Oracle button already lives (`src/components/ReplayScreen.tsx`).
   This single line is the entire ecosystem funnel.
3. Mark the game finished in the App Group cache; the drawer list shows a
   trophy row with "Watch replay".

---

## 13. Web fallback

New Next.js route `src/app/m/[payload]/page.tsx`:

- Client-side only (like the replay page — no auth, `src/app/[game_id]/page.tsx`
  precedent). Parse text version prefix, base32-decode, call the new
  `wasm_msg_decode` through the existing kernel bridge
  (`sdk/ts/wasm/engine.ts` + `src/wasm/clientGuards.ts`
  wiring pattern), which replays and returns the state.
- Render the table **read-only from the public viewpoint** (both hands as
  backs) with the existing board components, plus: "This is a live iMessage
  game between two players. 📲 Get Foolish on the App Store to play in
  Messages — or play Durak free right here." (CTA to `/`).
- v1 is deliberately view-only. Web-side *play* (decode → move → produce new
  link → human pastes it back into the SMS thread) is a v2 experiment; it
  works mechanically but the paste-back UX is unproven.
- SEO/meta: OG tags rendering the same public snapshot so the link unfurls
  nicely in other messengers too (free marketing surface).

---

## 14. Concurrency worked examples

§7 defines the machinery; this section walks the cases an implementer (and a
tester) must reason through. Notation: `P` = shared parent state.

1. **Pickup ∥ throw-in (any N, incl. 2p):** worked end-to-end in §7.5. Winner:
   pickup (round closure outranks). Loser's throw-in discarded by the
   round-boundary guard with a human-readable toast.
2. **Two attackers throw in simultaneously (N≥3):** chains A₁ = P+attack(9♣),
   A₂ = P+attack(9♥). Same round, same turn count → Rule P tiebreak by digest.
   Loser's device rebases: same round, and the kernel re-validates capacity
   (defender's hand size / attack limit, enforced in `legal.c`). If capacity
   remains → re-applied and auto-staged ("your 9♥ was re-applied — send").
   If the winner's card consumed the last slot → discarded ("table is full").
   Exactly the semantics of a physical table: fastest hand lands first.
3. **Cover ∥ throw-in:** defender covers battle 1 while attacker adds a new
   attack. Different battles — the rebase re-applies cleanly in either order.
   No user-visible conflict at all; this is the common, boring race.
4. **Good ∥ good (N≥3):** multiple attackers declare "done". Re-applied in
   sequence; the kernel's `good_players_mask` (`game.c`) makes repeats
   idempotent-by-legality; the final good triggers the round transition in
   whichever chain lands last. Converges without special-casing.
5. **Seat-claim ∥ seat-claim (WAITING):** both claim seat 2. Rule P digest
   tiebreak picks one; loser's device auto-claims seat 3 on next open (no
   kernel actions exist yet, so this can never conflict with play).
6. **Move ∥ game-over:** a straggler's action races the terminal action.
   FINISHED chain has the higher turn (or round) → wins; straggler's pending
   is discarded by the FINISHED row of §7.6.
7. **Same player, two devices (iPhone staged, iPad plays):** the pending
   ledger is per-device; the iPhone's unsent staged move rebases against the
   iPad's sent chain like any other race. No special handling.

Design consequences worth stating explicitly:

- **No rules variant is needed to tame races.** Earlier drafts considered a
  strict-rotation variant; the rebase model makes full podkidnoy semantics
  work asynchronously, and the physical-table analogy ("fastest hand wins,
  pickup ends the argument") is the correct arbiter. The `variant` byte stays
  reserved for actual rules options (deck size, transfer/perevodnoy), not for
  concurrency workarounds.
- **UI must expose contention gently:** while any other seat *could* act, show
  a subtle "others may be playing too" hint next to Send, and always treat an
  adopted foreign chain as normal progress, not an error.
- v1 UI caps games at 4 players purely for layout; the protocol and tests run
  at 8 (`MAX_PLAYERS`, `game.h:12`).

---

## 15. Fair-deal

Optional commit–reveal so the game creator cannot grind seeds for a favorable
deal (they know the whole deal at INVITE time). OFF in v1 (friends), spec'd so
the envelope never needs a format bump:

```
flags.bit0 = 1
WAITING (phase 0): seed field = zeros; append commit32 = SHA-256(s1)   [creator picks s1]
ACCEPT  (phase 1): seed field = zeros; each joiner appends their s_i (16 bytes)
                   alongside their seat claim
first LIVE turn:   seed = first 16 bytes of SHA-256(s1 ‖ s2 ‖ … ‖ sN);
                   creator appends s1; every device verifies SHA-256(s1) ==
                   commit32 and recomputes the seed
```

Cost: one extra message round before play (in a 2p DM). UI copy: "🎲 Fair deal
verified." Appendix idea (not v1): each joiner could also commit
H(seat_secret) here, making §6.3's seat recovery cheat-resistant — a device
proves a seat by presenting the preimage.

---

## 16. Format 6

> **⚠ Superseded by shipped work (2026-07-15):** a format 6 now
> EXISTS (`docs/REPLAY_FORMAT6_HIDDEN_STATE.md`) but it is the *other* design —
> inline hidden-card reveals, **no seed**, mid-game cut legal. It cannot drive
> continued play by itself (future stock order is absent) and therefore does
> NOT replace the FMSG seed+actions payload. Do not build the sketch below as
> written; `IMESSAGE_IMPLEMENTATION_HANDOFF.md` §3.3 explains what shipped and
> what an eventual FMSG v2 would do.

The true partial-game entropy-coded format — build **only** if §4.4's size
guardrail trips or QR-able mid-game states become a product need:

- New `REPLAY_FORMAT_VERSION`-adjacent format id 6 in `c/src/replay.c`,
  sharing the menu/weight machinery of v5 but: (a) header carries the 16-byte
  deal seed; (b) the decoder deals from the seed so **the entire reveal /
  hypergeometric / complement subsystem is bypassed** (every card is known);
  (c) an explicit varint action-count prefix replaces "decode until fool
  known", eliminating `REPLAY_EINCOMPLETE` for this format; (d) moves are
  rANS-coded against the same legal-move menus (`replay.c` weights) — expected
  ~1–2 bits/move vs 24 bits/move in `FMSG` v1 → whole-game payloads of
  ~50–80 bytes.
- v5 stays frozen and untouched; format 6 is additive with its own version
  byte. `FMSG` v2 would then reference format-6 bytes as its body.
- Everything else in this document (protocol, identity, UI) is unchanged —
  which is exactly why the codec work is deferrable.

---

## 17. Gotchas

1. **Extensions cannot send messages** — only stage into the input field; the
   human sends. All state transitions must ride `didStartSending` /
   `didCancelSending`, not the moment you call `insert`.
2. **Staged ≠ sent, forever:** a user can stage a move, close Messages, and
   the staged bubble evaporates without either callback in some paths — treat
   the App Group cache as advisory and always trust the tapped payload (§7).
3. **Participant UUIDs don't transfer across devices** (§6). Any design that
   puts them in the payload is wrong. Rule S1 or bust.
4. **Same session ≠ guaranteed replacement everywhere:** older bubbles collapse
   to `summaryText` — write summaries that read sensibly as a transcript
   ("Alex covered with Q♦ and attacked 9♣").
5. **The extension memory ceiling** (undocumented, tens of MB — see forums
   thread cited in `ORACLE_MONETIZATION_ENGINEERING.md` §7.5): keep the
   extension to SwiftUI + `libfoolish.a`. No WKWebView, no wasm, no bots, no
   Oracle in-extension — the Oracle runs in the host app / web via §12's link.
6. **macOS Messages does not run iOS iMessage extensions** — Mac recipients
   see the URL text. The `/m/` route (§13) is therefore not optional polish;
   it IS the Mac/Android experience.
7. **No push, no background:** if B never taps the bubble, the game just waits.
   Don't promise "your opponent was notified" beyond the message itself.
8. **Hidden state is peekable** (§2). Casual-only framing everywhere; rated
   play stays server-side. Seed grinding by the creator exists until §15 ships.
9. **Sender spoofing / payload tampering** is detectable only per §7.3's
   legality replay (illegal actions can't be smuggled) — but a *legal* move
   made "for" the opponent by editing bytes is not preventable serverlessly.
   Accepted; casual.
10. **App identity trap:** standalone iMessage apps can't later merge into an
    iOS app record (§9.1). Create the bundled form on day one.
11. **App Review:** even the minimal host must not feel like a stub (4.2), the
    extension needs its own icon set, the app needs privacy nutrition labels
    ("data not collected" — v1 truly collects nothing), and the age-rating
    questionnaire should declare no gambling (see
    `ORACLE_MONETIZATION_ENGINEERING.md` §7.3).
12. **Testing trap:** the Xcode Simulator's Messages test harness supports a
    two-participant conversation, but `didStartSending` timing and session
    collapse behave slightly differently on device — budget device-pair
    testing before TestFlight.
13. **Locale:** card glyphs and "Дурак" strings must come from the shared
    localization keys; don't hardcode English in summaries — `summaryText`
    localizes on the *sender's* device (Messages sends the resolved string),
    so use the conversation-neutral phrasing from the shared strings file.
14. **URL escaping:** base32 alphabet is URL-safe by construction (that's why
    we reuse the codec's); never URL-encode twice; assert round-trip in tests.
15. **The pending ledger must be durable and small.** Store it in the App Group
    alongside the cache (§9.3) so a killed extension or a cancel-after-stage
    can't strand a move, and cap it at the current round's actions — the
    round-boundary guard (§7.4) makes anything older unreplayable by
    definition, so garbage-collect on every round closure.
16. **Never special-case "whose turn it is" in UI logic.** Every piece of turn
    logic must query the kernel's legal-move set for the local seat (§5.1).
    Any hand-rolled "it's my turn" boolean will be wrong in multi-actor
    states — this is the exact class of bug the repo previously eliminated on
    the web client (see `git log 750d0b4`, "no hand-rolled rotation").

---

## 18. Test plan

**CI (no Mac required) — extend the existing e2e suite (`e2e/`, runs real
kernel wasm against Node):**

- `e2e/msg_wire.test.ts`: envelope round-trip (encode→decode→re-encode
  byte-identical); golden hex vectors for WAITING/LIVE/FINISHED at 2, 3, and 4
  players; tamper matrix (flip every byte class → decode must fail cleanly,
  never crash — reuse the hostile-bytes idioms from `e2e/client_guards.test.ts`);
  size guardrail (P95 simulated full game < 1,000 chars **at 4 players**, the
  worst case).
- `e2e/msg_concurrency.test.ts` — the §7 model as pure functions over the C
  exports: (a) Rule P is a total order (antisymmetric, transitive — property
  test over random chain pairs); (b) delivery-order independence: for random
  concurrent sends, all interleavings of adoption converge every simulated
  device to the same chain; (c) rebase determinism incl. the round-boundary
  guard — encode §14's seven worked examples as named regression cases, with
  the pickup ∥ throw-in race asserted in both delivery orders; (d) N-player
  fuzz: 3–8 simulated players acting concurrently at random through full
  games — every game converges, every discarded action maps to a §7.4 reason,
  and no chain ever contains a kernel-illegal action.
- Cross-engine determinism: same seed → identical deal + identical legal-move
  menus in (a) kernel wasm and (b) `libfoolish.a` — golden vectors bridge the
  two (§8.2). Any divergence is a release blocker (both devices must replay
  identically).
- Full-game simulation: random legal playouts through `FMSG` encode/decode at
  every half-move; at game end, `replay_encode` must produce a v5 code whose
  standard decode matches the final state (this chains the new format to the
  frozen one).

**XCTest (Mac):** bridge smoke tests (deal/legal/apply parity against the same
golden vectors), snapshot-render tests of the bubble image.

**Manual matrix:** two simulators (Xcode Messages harness) → full game; two
physical devices; reinstall-mid-game (cache loss → Rule S1 recovery); tap
ancient collapsed bubble; cancel a staged send; airplane mode (everything
works; message queues); Mac recipient sees working `/m/` URL.

---

## 19. Milestones

| M | Deliverable | Effort | Acceptance criteria |
| --- | --- | --- | --- |
| **M0** | `msg_wire.c` + wasm exports + TS bridge + `e2e/msg_wire.test.ts` + **`e2e/msg_concurrency.test.ts` (Rule P / rebase / N-player fuzz)** + `/m/` view-only route | 5–8 d | CI green incl. tamper, size guardrail, convergence properties; `/m/` renders a live-game URL read-only in prod |
| **M1** | `ios-lib` Makefile target (xcframework) + Xcode project skeleton (§9.2 targets, App Group) + Swift bridge + XCTest parity vs golden vectors | 2–4 d | XCTests green on CI-mac or locally; simulator app launches |
| **M2** | SwiftUI board (2–4 seats) + interaction model (§10) driven end-to-end by `libfoolish.a`; host app pass-and-play mode | 5–8 d | A full local hotseat game (2p and 4p) is playable; snapshot renderer produces bubble images |
| **M3** | Messages wiring (§11): stage/send/receive/replace, lifecycle cache + pending ledger, Rule P adoption, §7.4 rebase UX, lobby/seat-claim flow | 7–10 d | Two simulators complete 2p and 3p games via bubbles; §14 cases 1–2 reproduced manually with correct toasts; reinstall recovery works |
| **M4** | Game end → v5 replay bubble (§12); localization; polish | 2–3 d | Finished game's bubble opens the real replay page; Oracle reachable from it |
| **M5** | App Review prep: host-app polish, icons, privacy labels, age rating, TestFlight, submit | 3–5 d | Approved on the App Store |
| **M6+** | Fair-deal (§15), 5–8 player UI, web-side play (§13), format 6 (§16), main-app absorption per `ORACLE_MONETIZATION_ENGINEERING.md` §7 | — | — |

Total to App Store: **~5–7 weeks solo** (the N-player-from-day-one decision
buys ~1 week of extra work in M0/M2/M3 and removes an entire future rewrite).
The dependency spine is M0 → M1 → M2 → M3; M4/M5 are short tails. M0 is pure
repo work with the existing toolchain — start there today, and note that the
concurrency suite in M0 de-risks the entire design before a single line of
Swift exists.

---

## 20. Implementation handoff — repo state 2026-07-15 {#20-handoff}

**The implementation work order now lives in its own doc:
[`docs/IMESSAGE_IMPLEMENTATION_HANDOFF.md`](IMESSAGE_IMPLEMENTATION_HANDOFF.md)
— read it after this spec; where the two disagree, the handoff wins.** It was
merged from two independent passes on 2026-07-15 and contains: what already
exists (most of §19's M1–M2 fell out of the iOS app build — engine bridge,
board, design system, native replay), the spec corrections summarized below,
verified July-2026 platform facts (documented 5,000-char `MSMessage.url`
cap, exact `MSSession`/`summaryText` semantics, compact-style constraints),
the updated M0–M5 build order, and an invariants checklist. Mockups:
`docs/imessage-layout.html` (M1–M7).

Corrections it makes to THIS doc (each also flagged with a ⚠ banner in
place): the deal seed is **32 bytes**, not 16 (§4.1; envelope offsets shift);
actions are **seat-prefixed awire frames**, not 3-byte triples (§4.2 — chains
interleave seats, multi-card moves are one action); the durable state blob
does NOT carry the deal seed (§1's table); a format 6 **shipped** but as the
no-seed inline-reveal design, so it is not the continuation format — FMSG v1
= seed + action chain stands, v6 upgrades the FINISHED replay and is the
FMSG-v2 compression template (§16).
