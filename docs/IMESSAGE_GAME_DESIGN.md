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
7. [Forks, staleness, and double-send (consistency model)](#7-consistency)
8. [The native engine library (C → iOS)](#8-native-engine)
9. [Xcode project structure & the host-app decision](#9-xcode--host-app)
10. [The extension UI (SwiftUI spec)](#10-ui-spec)
11. [Messages framework wiring (exact APIs)](#11-messages-wiring)
12. [Game end: bridging back into the ecosystem](#12-game-end)
13. [Web fallback route `/m/`](#13-web-fallback)
14. [Multiplayer beyond 1v1 (v2 design sketch)](#14-multiplayer)
15. [Fair-deal protocol (optional, spec'd now, build later)](#15-fair-deal)
16. [Format 6: the true partial-game rANS codec (optional optimization)](#16-format-6)
17. [Gotchas catalog (read before writing any code)](#17-gotchas)
18. [Test plan](#18-test-plan)
19. [Milestones with acceptance criteria](#19-milestones)

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
| Rules kernel (deal, legality, apply, views) | `cnitro/src/game.c`, `legal.c`, `view.c` | The complete game engine, compiled to wasm today; compiles natively for iOS tomorrow |
| **Seeded deals** | `cnitro/src/game.h:139-151` — `game_set_deal_seed_bytes(seed,len)`: ChaCha shuffle over the full 52!/36! space, whole game reproducible from the seed; state records `deterministic_deck` | A 16-byte seed replaces the entire hidden deal — the foundation of the payload format |
| Durable state blob v2 | `cnitro/wasm/wasm_api.c:316-327` — `wasm_state_serialize/deserialize` (carries the deal seed) | Not used in the payload (see §3) but useful for local caching |
| Replay codec v5 (finished games → short URL) | `cnitro/src/replay.{h,c}` (C is canonical; frozen wire format), TS bridge `supabase/functions/_shared/replay/` | The game-over artifact: a finished iMessage game becomes a normal `foolish.cards/<code>` replay, Oracle-analyzable |
| Packed action wire format | `supabase/functions/_shared/packed_action.ts`, `cnitro/wasm/wire.h` | The per-move byte encoding the payload reuses verbatim |
| Kernel-in-browser bridge | `src/wasm/clientGuards.ts`, `supabase/functions/_shared/wasm/engine.ts` | The web client already loads the C kernel as wasm — the `/m/` fallback route rides this |
| Full export list of the wasm rules build | `cnitro/Makefile:331-353` | Shows every kernel entry point already exposed (`wasm_start_game`, `wasm_apply_action`, `wasm_legal_moves`, `wasm_replay_encode`, …) |

**The constraint that shapes everything:** an iMessage extension has *no server
in the loop*. Each turn is an `MSMessage` whose **URL payload must carry the
entire game**. Apple's Messages infrastructure is the transport, storage, and
notification system. The extension only runs while the user is looking at it.

---

## 2. Product spec

- **v1 is strictly 1v1** ("heads-up") Durak between two people in an iMessage
  conversation. Group-chat multiplayer is §14, later.
- Flow: player A opens the Foolish iMessage app in a conversation → taps
  **New game** → a game bubble appears in the thread ("♠️ Alex started Durak —
  your move"). B taps the bubble → the extension opens showing the table from
  B's perspective → B plays their move(s) → a new bubble replaces the old one
  (same `MSSession`) → alternating until someone is the fool.
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

The v5 replay codec (`cnitro/src/replay.c`) encodes a **finished** game. Its
decoder replays the game from the start and learns hidden cards lazily: each
reveal is entropy-coded against a hypergeometric model, and the cards the loser
never played are recovered **by complement once the fool is known** — the
format literally has an error for a stream that ends early:
`REPLAY_EINCOMPLETE (=16) — "logs ended before the fool was known"`
(`cnitro/src/replay.h:56`). There is no way to terminate a v5 stream mid-game;
the trailing backfill step is load-bearing. So the original intuition is
correct: v5 cannot encode a partial game, and *extending v5 itself* would be
surgery on a frozen wire format.

### 3.2 …and why we don't need to touch it

A partial game does not need the reveal machinery at all. In serverless
iMessage play, **both devices must know the full deal anyway** (each device
continues the game locally, including drawing from the deck). Since the kernel
already supports fully deterministic seeded deals —
`game_set_deal_seed_bytes()` (`cnitro/src/game.h:139-151`) — a mid-game state
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

### 4.1 Envelope `FMSG` v1 (binary, little-endian, before text encoding)

A new, small, versioned envelope. Implemented **in C** — single source of truth,
like everything else in this repo — as `cnitro/src/msg_wire.{h,c}`, compiled
into (a) the iOS static library (§8) and (b) the existing wasm rules build with
two new exports `wasm_msg_encode` / `wasm_msg_decode` added to the export list
at `cnitro/Makefile:331-353`, bridged to TS in
`supabase/functions/_shared/wasm/engine.ts` for the web route (§13) and e2e
tests (§18).

```
offset  size  field            notes
0       1     magic      0xF7
1       1     format     1
2       1     flags      bit0 fair_deal (§15)  bit1 gzip-body  bits2-7 reserved=0
3       1     phase      0=INVITE  1=ACCEPT  2=LIVE  3=FINISHED
4       8     game_id    random u64, generated at INVITE, constant for the game
12      2     turn       u16, count of actions applied (0 = fresh deal)
14      1     last_actor_seat  seat (0-based) of the player who SENT this message
15      1     n_players  =2 in v1 (engine supports 2..8, game.h:12)
16      1     variant    reserved rules-variant byte, =0 (36-card podkidnoy defaults)
17      1     reserved   =0
18      8     parent8    first 8 bytes of SHA-256 of the previous envelope's
                         bytes (all zeros for INVITE). Fork/staleness detect (§7)
26      16    seed       deal seed → game_set_deal_seed_bytes(seed,16).
                         All zeros only in fair-deal INVITE/ACCEPT (§15)
42      2     n_actions  u16
44      3×n   actions    packed actions, 3 bytes each (§4.2)
44+3n   var   [fair-deal extras, only when flags.bit0 — §15]
```

Everything a device needs is here; there is deliberately **no state snapshot**
in the payload (state is derived), no player names (Messages shows names), and
no participant identifiers (they don't transfer across devices — §6).

### 4.2 Action encoding (3 bytes/action)

Reuse the existing packed-action wire encoding — do **not** invent a new one.
The canonical definitions are `supabase/functions/_shared/packed_action.ts`
(TS) and `cnitro/wasm/wire.h` (C): action type byte + 1-byte wire cards
(`0..51 = suit*13+(value-1)`, `0xFE` hidden, `0xFF` none — `replay.h:31-36`
documents the same card byte). `msg_wire.c` stores each action as
`{u8 type, u8 card_a, u8 card_b}` using those exact byte conventions; multi-card
actions (multi-attack, multi-pass) are stored as consecutive single-card
actions of the same type where the engine's apply path supports it, otherwise
as the packed multi-action form already defined in `packed_action.ts` — the
implementer MUST read that file first and mirror it, not approximate it.

### 4.3 Text encoding & URL

`bytes → base32` using the codec's existing base32 (same alphabet/util as
`supabase/functions/_shared/replay/codec.ts` — QR-alphanumeric-safe, already
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

### 5.1 State machine (simple mode — fair_deal off, the v1 default)

```
A taps "New game"
  A's device: game_id=rand64, seed=rand128, deal via kernel, start_game
              (kernel decides first attacker: lowest trump — engine handles it).
              If A is the first actor, A may immediately play; either way A
              STAGES the INVITE/LIVE bubble:
                phase=LIVE, turn=n_actions_applied, last_actor_seat=A's seat
  A hits Messages' send button (extensions cannot auto-send — §17.1)

B taps the bubble
  B's device: decode envelope → fresh kernel game → set seed → deal →
              replay all actions (validate each) → render B's view.
              B plays until the kernel says B is no longer the actor
              (`wasm_should_act`-equivalent native call), stage new bubble
              with turn+=k, parent8=digest(prev), last_actor_seat=B's seat.
  B sends. Repeat, alternating, until the kernel reports game over.

Whoever applies the terminal action stages the FINISHED bubble (§12).
```

Notes:
- **One bubble per game** via `MSSession` — Messages collapses older messages
  in the same session to their `summaryText` and keeps the latest interactive
  (§11.3). The thread never fills with 60 bubbles.
- **A "turn" may be several kernel actions** (attack two cards, opponent covers
  both…). The staging rule is: keep applying the local player's choices until
  the kernel's next-actor is the opponent (or the game ends), then stage ONE
  message carrying all of them. This is why `turn` counts *actions*, not
  messages.
- In heads-up Durak the kernel's next-actor is always unique, so alternation is
  well-defined. (This uniqueness breaks at 3+ players — the core reason v1 is
  1v1; §14.)

### 5.2 What each phase means

| phase | contents | staged by | UI on open |
| --- | --- | --- | --- |
| LIVE (2) | seed + actions | either player | table from viewer's seat; input enabled iff kernel says viewer is actor |
| FINISHED (3) | seed + full actions | player who applied terminal action | result card + replay link (§12) |
| INVITE (0) / ACCEPT (1) | only used by fair-deal mode | see §15 | — |

---

## 6. Identity & seats

There are no accounts. Apple gives the extension **opaque participant UUIDs**
(`conversation.localParticipantIdentifier`, `MSMessage.senderParticipantIdentifier`)
with a brutal caveat: **they are scoped per-device-per-conversation** — the
same human has *different* UUIDs on their own iPhone vs iPad, and on their
opponent's device. Therefore participant UUIDs never go in the payload; each
device figures out "which seat am I?" statelessly:

```
Rule S1 (stateless seat inference, works even after reinstall):
  on opening a bubble:
    amISender = (selectedMessage.senderParticipantIdentifier
                 == conversation.localParticipantIdentifier)
    mySeat = amISender ? envelope.last_actor_seat
                       : 1 - envelope.last_actor_seat        // 2p only
```

- Seat 0 is defined as the game creator; `last_actor_seat` in the INVITE/first
  LIVE bubble is 0 by construction.
- Cache `{game_id → mySeat}` in App Group storage (§9.3) as an optimization
  and for the game list, but Rule S1 must always be able to rebuild it — the
  payload + message metadata are the only durable truth.
- **Same-user-multiple-devices:** if Alex opens the game on iPad, Rule S1 still
  works (Messages syncs the thread; sender identity resolves per device). The
  App Group cache is per-device and simply rebuilds.
- **Spoofing** (opponent editing a payload to move for you) is detectable only
  as "actions by seat X arrived in a message whose sender wasn't X" — and the
  extension can't verify historical senders, only the tapped message. Accepted
  for casual play; noted in §17.9.

---

## 7. Consistency

### 7.1 The canonical state is "the newest message in the session"

Messages' session replacement means the thread naturally shows one current
bubble. The extension cannot enumerate the thread; it only sees the message the
user tapped. So consistency is enforced with payload metadata:

- `turn` — monotonically increasing action count.
- `parent8` — digest chain: each envelope commits to its predecessor.

### 7.2 Staleness & forks

App Group cache stores per `game_id`: `{max_turn_seen, digest_of_latest}`.
On open:

| Condition | Meaning | UI |
| --- | --- | --- |
| `env.turn > cached.max_turn` and `env.parent8` chain consistent | normal progress | play |
| `env.turn < cached.max_turn` | user tapped an old (collapsed) bubble | banner "This game has moved on — open the newest message", read-only |
| `env.turn == cached.max_turn` but digest differs | **fork** (e.g., both sides composed against the same parent; possible only via double-send or manual URL tampering) | Rule F1: the message *received most recently by this device* wins; render banner "Game took a different path — continuing from this bubble", overwrite cache |
| `didStartSending` fires for our staged message | our move is committed | update cache to our new envelope |
| `didCancelSending` fires | user deleted the staged bubble | roll cache back to pre-stage snapshot (§17.2) |

In strict 1v1 alternation a fork requires the same player sending twice from
one parent (Messages allows staging only one message at a time per extension
session, so this is rare) — F1 keeps both devices converging on *some* line,
which is all casual play needs.

### 7.3 Validation = replay

Because decode replays every action through the kernel's legality machinery, a
corrupted or hand-edited payload fails loudly (an action not in the legal-move
set simply won't apply). Error UI: "This game link is damaged." Never attempt
partial recovery. The kernel is already hardened against hostile bytes — see
`docs/SECURITY_WASM_BOUNDARY.md` (counts clamped, cards sanitized, enumeration
bounded); `msg_wire.c` must go through the same `get_state`-style clamps by
constructing games only via the public kernel calls, never by memcpy into
`Game`.

---

## 8. Native engine

### 8.1 New build target: `libfoolish.a`

Add to `cnitro/Makefile`:

```
ios-lib:  # arm64 device + arm64/x86_64 simulator slices, or an .xcframework
  sources: game.c legal.c view.c replay.c deal_rng.c msg_wire.c ios_api.c
  flags:   plain clang -O2 for Apple targets; NO wasm flags; C11; no OMP
```

The wasm builds prove these sources are dependency-clean (`-nostdlib
-ffreestanding` at `cnitro/Makefile:287`); natively they may use libc freely
(they already do for native builds — `cordite_sim.c` includes stdio/stdlib for
the research binaries, but note the **bot/strategy sources are NOT in this
library** — the iMessage v1 ships rules only, no bots, keeping the extension
binary tiny).

`ios_api.c` is a thin shim in the spirit of `cnitro/wasm/wasm_api.c` (which is
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

Apple: an iMessage app is either **standalone** (no visible iOS app) or an
**extension bundled in an iOS app** — and converting between the two later
means creating a **new app record**
([App Store Connect help](https://developer.apple.com/help/app-store-connect/create-an-app-record/add-imessage-app-information/),
[dev forums](https://developer.apple.com/forums/thread/61482)). Since
`ORACLE_MONETIZATION_ENGINEERING.md` §7 plans a full iOS app whose Oracle the
iMessage game funnels into, **choose the bundled form now**:

> **Decision: create ONE iOS app record ("Foolish — Durak") containing (a) a
> deliberately small but genuine host app and (b) the Messages extension.**

The v1 host app is NOT a stub (stubs risk Guideline 4.2): it reuses the same
SwiftUI board + `libfoolish.a` to offer **offline pass-and-play and vs-nothing
practice** (deal a seeded game, both seats on one screen) plus a "paste a
foolish.cards link" replay viewer opening the web replay. That's 2–4 days of
extra work, gives reviewers something real, and is the seed the §7 full app
(and Oracle) grows into without a new app identity.

### 9.2 Targets

```
Foolish.xcodeproj
├─ Foolish            (iOS app target — minimal host, SwiftUI)
├─ FoolishMessages    (iMessage extension target, MSMessagesAppViewController)
├─ FoolishKit         (shared Swift framework: engine bridge, models, board UI)
└─ libfoolish.a / Foolish.xcframework   (from cnitro/Makefile ios-lib)
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
  Done (good)** — enabled strictly from the legal-move list. A **Send move**
  button appears once the local player is no longer the actor; it stages the
  message (§11.4). Undo (before staging) = rebuild from envelope + replay minus
  local moves — trivial since state is derived.
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
  (`supabase/functions/_shared/wasm/engine.ts` + `src/wasm/clientGuards.ts`
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

## 14. Multiplayer

Why v1 is 1v1: with 3+ players the kernel's "who may act" is **not unique** —
in podkidnoy multiple attackers may throw in simultaneously, so asynchronous
message ordering creates real races (two players legally act on the same
parent state → guaranteed forks that F1 resolves arbitrarily and unfairly).

v2 sketch (build only after v1 proves the loop):

- Group iMessage threads support MSSession fine; payload already carries
  `n_players` and 0-based seats; `game_id` keys seat claims.
- **Seat claiming:** INVITE lists open seats; each joiner's ACCEPT-like turn
  claims the lowest free seat (their device records it; Rule S1 generalizes:
  the payload records `seat_claim_order` so any device can recompute claims
  from the action list itself).
- **Race taming:** restrict the async variant's rules — single-attacker strict
  rotation (no mid-round throw-ins by third parties) so the actor is always
  unique. This is a *rules variant* (the `variant` byte in the envelope) —
  wire it through the kernel's config rather than pretending races don't
  exist. Full podkidnoy chaos stays a realtime-server feature.
- Out-of-turn players tapping the bubble get spectator view + "waiting for
  Sveta".

---

## 15. Fair-deal

Optional commit–reveal so the game creator cannot grind seeds for a favorable
deal (they know the whole deal at INVITE time). OFF in v1 (friends), spec'd so
the envelope never needs a format bump:

```
flags.bit0 = 1
INVITE  (phase 0): seed field = zeros; append commit32 = SHA-256(s1)   [A picks s1]
ACCEPT  (phase 1): seed field = zeros; append s2 (16 bytes)            [B picks s2]
first LIVE turn:   seed = first 16 bytes of SHA-256(s1 ‖ s2); append s1
                   [B's device verifies SHA-256(s1) == commit32 and recomputes seed]
```

Cost: one extra message round before play. UI copy: "🎲 Fair deal verified."

---

## 16. Format 6

The true partial-game entropy-coded format — build **only** if §4.4's size
guardrail trips or QR-able mid-game states become a product need:

- New `REPLAY_FORMAT_VERSION`-adjacent format id 6 in `cnitro/src/replay.c`,
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

---

## 18. Test plan

**CI (no Mac required) — extend the existing e2e suite (`e2e/`, runs real
kernel wasm against Node):**

- `e2e/msg_wire.test.ts`: envelope round-trip (encode→decode→re-encode
  byte-identical); golden hex vectors for INVITE/LIVE/FINISHED; tamper matrix
  (flip every byte class → decode must fail cleanly, never crash — reuse the
  hostile-bytes idioms from `e2e/client_guards.test.ts`); size guardrail (P95
  simulated full game < 1,000 chars); fork/staleness rule table (§7.2) as pure
  functions.
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
| **M0** | `msg_wire.c` + wasm exports + TS bridge + `e2e/msg_wire.test.ts` + `/m/` view-only route | 3–5 d | CI green incl. tamper + size guardrail; `/m/` renders a live-game URL read-only in prod |
| **M1** | `ios-lib` Makefile target (xcframework) + Xcode project skeleton (§9.2 targets, App Group) + Swift bridge + XCTest parity vs golden vectors | 2–4 d | XCTests green on CI-mac or locally; simulator app launches |
| **M2** | SwiftUI board + interaction model (§10) driven end-to-end by `libfoolish.a`; host app pass-and-play mode | 4–7 d | A full local hotseat game is playable; snapshot renderer produces bubble images |
| **M3** | Messages wiring (§11): stage/send/receive/replace, lifecycle cache, staleness/fork banners | 5–8 d | Two simulators complete a full game via bubbles; reinstall recovery works |
| **M4** | Game end → v5 replay bubble (§12); localization; polish | 2–3 d | Finished game's bubble opens the real replay page; Oracle reachable from it |
| **M5** | App Review prep: host-app polish, icons, privacy labels, age rating, TestFlight, submit | 3–5 d | Approved on the App Store |
| **M6+** | Fair-deal (§15), group multiplayer (§14), web-side play (§13), format 6 (§16), main-app absorption per `ORACLE_MONETIZATION_ENGINEERING.md` §7 | — | — |

Total to App Store: **~4–6 weeks solo.** The dependency spine is M0 → M1 → M2
→ M3; M4/M5 are short tails. M0 is pure repo work with the existing toolchain
— start there today.
