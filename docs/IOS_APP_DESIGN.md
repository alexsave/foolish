# Foolish for iOS — Full Design Doc & Build Plan

*A zero-context, implementation-ready design for a proper, App-Store-approved
native iOS app: a port of foolish.cards with a cleaned-up, opinionated visual
identity. No billing in v1, but architected so client-side Infinite Oracle
billing drops in later without rework. Written to be picked up and executed by
an implementer (or LLM) with no prior knowledge of this repository. Every claim
about existing code carries a verified anchor (2026-07-13). Companion docs:
`docs/IMESSAGE_GAME_DESIGN.md` (the Messages extension that ships INSIDE this
app), `docs/ORACLE_MONETIZATION_ENGINEERING.md` (business + billing context),
`docs/WEB_RACE_BUG_HANDOFF.md` (a server-side guard this app will rely on).*

---

## Table of contents

1. [Context for the zero-context reader](#1-context)
2. [Goals / non-goals](#2-goals--non-goals)
3. [Architecture decision: native SwiftUI, one shared kit](#3-architecture)
4. [Module map](#4-module-map)
5. [The design system ("opinionated style")](#5-design-system)
6. [Screens & flows](#6-screens--flows)
7. [The engine layer (offline play, replays, future Oracle)](#7-engine-layer)
8. [The backend protocol (online play)](#8-backend-protocol)
9. [Accounts & auth](#9-accounts--auth)
10. [Billing-ready scaffolding (no billing in v1)](#10-billing-ready)
11. [App Review compliance](#11-app-review)
12. [Performance & quality bars](#12-quality-bars)
13. [Testing strategy](#13-testing)
14. [Milestones & build order](#14-milestones)
15. [Risks & open questions](#15-risks)

---

## 1. Context

**Product.** foolish.cards is a web Durak card game (Russian classic, 2–8
players): real-time multiplayer, strong bots that are "real players" in the
DB, spectating, and shareable replays (a whole game compresses into a short
URL/QR via an rANS codec). Repo layout and philosophy: `README.md`; the
architecture in one line: **all game rules live once, in C
(`cnitro/src/game.c`, `legal.c`, `view.c`, `replay.c`), compiled to WASM for
the web client, the Deno edge functions, and tests** — the TS around it is
marshaling and UI (`docs/ARCHITECTURE_AS_A_PATTERN.md`).

**Backend.** Supabase: Postgres + edge functions (`supabase/functions/`:
`create`, `action`, `meta`, `bot-heartbeat`) + Realtime broadcasts. Moves
commit via CAS on `games.version`. Clients receive **animation events** on
per-player channels (you see your own hand; others see card backs).

**Why a native app (not a wrapper).** Decided in
`ORACLE_MONETIZATION_ENGINEERING.md` §7 and hardened here: the current web UI
is explicitly considered rough in places ("vibe coded") and the owner wants an
opinionated cleanup — wrapping it would import the problem; Apple's 2025-26
review climate punishes thin wrappers (4.2 minimum functionality; the Mar/Jun
2026 low-value-app crackdowns); the iMessage extension
(`IMESSAGE_GAME_DESIGN.md`) needs a native SwiftUI board anyway; and the
future paid Infinite Oracle runs best as natively compiled C. One native
codebase serves all three.

---

## 2. Goals / non-goals

**v1 goals**

1. Feature-parity port of the website's core: play Durak online (humans +
   bots), lobby/matchmaking as the site has it, watch/share replays, tutorial,
   localization (en/ru/ko).
2. **Plus** the one thing the site can't do: fully offline play vs the bot
   roster (the C engine and bots live in the binary).
3. An opinionated, coherent visual identity (§5) — this is the "cleaned up"
   mandate, and it's a feature, not a coat of paint.
4. Approved on the App Store as a substantial native app.
5. Billing-*ready* seams (§10): the Infinite Oracle paywall can be added later
   by filling in stubs, with zero architectural rework.
6. Hosts the iMessage extension from `IMESSAGE_GAME_DESIGN.md` (same app
   record — this app **supersedes** that doc's §9.1 "minimal host app": the
   host is now this full app; the extension targets/protocol are unchanged).

**v1 non-goals**

- No IAP/StoreKit purchases, no paywalls shown to users, no Oracle UI (the
  Oracle ships later behind the §10 seams).
- No iPad-optimized layout (runs scaled; proper iPad later), no macOS.
- No push notifications (requires server work; design for it later — §15).
- No account-required play: guests play bots offline and online exactly like
  the website's low-friction flow.
- No new game features that the website doesn't have (except offline mode).

---

## 3. Architecture

**Decision: 100% SwiftUI app + a shared `FoolishKit` framework + the C engine
as a static library. No web views anywhere in the product UI.** (The only
WKWebView allowed: rendering the web replay page from a share link IF the
native replay viewer is not ready — see M5; delete it once native replay
lands.)

Rationale beyond §1: SwiftUI gives the motion/haptics quality bar cheaply;
the C engine gives offline + replays natively with the same bytes the server
runs; Supabase has a first-party Swift SDK (auth/realtime/functions) so the
online protocol is a port, not an invention.

**The one hard rule (copied from the repo's architecture and from
`IMESSAGE_GAME_DESIGN.md` §17.16):** *no game rule is ever reimplemented in
Swift.* Whose turn, legal moves, capacity checks, refills — every rules
question is answered by the C kernel through the bridge. Swift renders state
and forwards intents. Any hand-rolled "is my move legal" boolean is a bug by
policy (this exact bug class was purged from the web client — commits
`750d0b4`, `862d668`).

---

## 4. Module map

```
Foolish.xcodeproj  (bundle id cards.foolish.app; App Group group.cards.foolish)
├─ FoolishApp/            iOS app target (SwiftUI @main, routing, DI)
├─ FoolishMessages/       iMessage extension target (per IMESSAGE_GAME_DESIGN.md §9-11)
├─ FoolishKit/            shared framework, four submodules:
│   ├─ Engine/            Swift bridge over libfoolish.a (§7)
│   ├─ Net/               Supabase client, protocol codecs, realtime feed (§8)
│   ├─ DesignSystem/      tokens + components (§5)
│   └─ Boards/            game-rendering SwiftUI (table, hands, animations)
├─ Entitlements/          §10 — protocol + free stub (own module so the future
│                          StoreKit implementation swaps in cleanly)
└─ vendor/libfoolish/     Foolish.xcframework built by `make ios-lib` in cnitro/
                          (target spec: IMESSAGE_GAME_DESIGN.md §8.1; THIS app
                          adds bot strategies to the library — §7.2)
```

Dependency rules: `FoolishApp → FoolishKit → (Engine|Net|DesignSystem|Boards)`;
`FoolishMessages → FoolishKit` (Engine, DesignSystem, Boards only — never Net;
the extension is serverless by design); `Entitlements` has no dependency on
StoreKit in v1. Enforce with a lint script in CI (a plain grep on imports is
fine).

---

## 5. Design system

This section IS the "opinionated style" mandate. The current web UI grew
organically; the app does not inherit it. It inherits the *world* (procedural
materials, the Soviet-flavored ru theme, zero image assets) and formalizes it.

### 5.1 Identity: "Gosizdat Card Table"

One theme, dark-first, inspired by Soviet book-design constructivism plus a
physical card table: deep matte surfaces, bone-white cards, a single red
accent, strong condensed display type for numerals and titles. No gradients
except a subtle table vignette, no glassmorphism, no drop-shadow soup, no
more than ONE accent color on screen at a time.

### 5.2 Tokens (single Swift file, `DesignSystem/Tokens.swift`)

```
Color.table        #14231C   (deep green-black felt; light mode: #1B2E24 — the app
Color.surface      #1E2A24    is dark-first; "light" mode only lifts surfaces ~6%)
Color.card         #F4EFE6   (bone white)
Color.ink          #17140F   (card pips/type on card)
Color.accent       #C82B24   (Soviet red — actions, trump marks, destructive)
Color.textPrimary  #EDE9DF
Color.textDim      #9AA69E
Color.win          #D8B24A   (brass — victories, streaks; used sparingly)
Radius.card 10  Radius.sheet 16  Radius.chip 999
Space: 4pt grid — 4/8/12/16/24/32
Type: SF Pro Text (body 15/17), SF Pro Display Semibold (titles),
      SF Compressed Bold for BIG numerals (deck count, timers, ranks) — the
      condensed numerals are the signature; use nowhere else.
Motion: one spring — response 0.32, damping 0.82 — for ALL card movement;
        150ms ease-out for chrome; nothing animates slower than 400ms, ever.
Haptics: .light on card pick-up, .medium on legal drop, .rigid on reject,
         .success notification on round win. One map, in DesignSystem/Haptics.swift.
```

### 5.3 The materials carry over — as code, not PNGs

The web ships zero texture images (Barnsley-fern card backs, woven wool,
wood grain — `src/utils/`, README project 4). v1 ports **one** material: the
procedural card back (fern IFS, seeded) drawn into a `CGContext` once per
seed and cached — the app's card backs are identical in spirit to the web's,
which keeps cross-platform brand identity and later enables the seed-cosmetic
ideas. Reference implementation to port: the fractal helpers in `src/utils/`
(the implementer ports the math, not the WebGL — CPU → CGImage at 2x card
size is plenty). Everything else (felt, wool) is flat color + vignette in v1.

### 5.4 Component inventory (Boards/ + DesignSystem/)

`FCard` (face/back, selected, disabled, trump-badge) · `FHandFan` (drag or
tap-to-play; reachable one-handed) · `FBattleGrid` (attack/cover pairs,
covers land rotated 12°) · `FSeatBadge` (avatar-less: nickname + card count +
thinking indicator) · `FDeckWell` (deck count + flipped trump, condensed
numeral) · `FActionBar` (Pass/Pickup/Done — kernel-driven enable states,
never hand-computed) · `FToast` · `FSheet` · `FButton` (primary=red,
secondary=outline; that's the entire button family).

Accessibility floor: Dynamic Type through xxxLarge on non-board text, board
text scales with a cap; VoiceOver labels on every card ("seven of spades,
covered by queen of hearts"); Reduce Motion swaps the spring for cross-fades.

### 5.5 What "cleaned up" means concretely (the port is also an edit)

Rules the implementer applies while porting screens (each is a deviation from
the current site, on purpose): one primary action per screen; no dead-end
screens (every terminal state offers rematch/share/home); the table is
chrome-free while a round is live (status communicates through the board, not
banners); confirmation dialogs only for destructive acts (leave live game);
all text through the localization table from day one (`en/ru/ko` — port keys
from `src/localization/strings.ts`, adding new keys in all three languages).

---

## 6. Screens & flows

Six screens. Web-route anchors are for the implementer to study behavior, not
markup, and the §5.5 edit rules apply everywhere.

| # | Screen | Web anchor | Spec |
| --- | --- | --- | --- |
| 1 | **Home** | `/` + `/dashboard` (`src/app/page.tsx`, `dashboard/`) | Big PLAY (online quick-match into the site's lobby flow), OFFLINE (bot picker — roster from §7.2 with left/right cycle like the lobby's bot picker, commit `7f22749`), Join-by-code, resume-in-progress card if a live game exists, footer: Replays · Tutorial · Settings |
| 2 | **Lobby** | game page pre-start | seats as `FSeatBadge` row, add-bot (server flow) / start / share invite link (same URLs the site uses), ready states live over realtime |
| 3 | **Table** | `/​[game_id]` (`src/app/[game_id]/`, `src/components/GameDisplay/*`) | the product. Layout: opponents arced top (2–8 seats), battles center, deck well right, own fan bottom, action bar above fan. All interactions per §5.4; spectator mode = same screen, no fan |
| 4 | **Win screen** | post-game flow | result, fool reveal moment (VHS-style pause borrowed from replay transport), buttons: Rematch (server `continue` flow — mirror `resetToLobby` semantics, `src/state/clientReconcile.ts:10-40`) · Share replay (§7.3) · Home |
| 5 | **Replays** | `/​<code>` replay screen (`src/components/ReplayScreen.tsx`) | list of saved replays (local store of codes) + paste/scan a code (camera QR); native playback via engine decode (§7.3) with the transport controls the web has (play/pause/step/scrub). **The Oracle button does NOT ship in v1** — its slot is behind the §10 flag |
| 6 | **Tutorial + Settings + About** | `/tutorial`, `/about` | tutorial ported as an offline scripted game against the engine (the web tutorial already runs a mock-identity seat — `AuthContext` note in `src/contexts/AuthContext.tsx:8-9`); Settings: language override, haptics toggle, account block (§9) incl. sign-out and DELETE ACCOUNT, licenses, links |

Flow notes: cold start lands on Home in <1.5s (§12) with zero network; online
sections mount lazily. Deep links: `foolish.cards/<code>` universal links open
the native replay viewer; `foolish.cards/<game_id>` joins/spectates that game;
`/m/...` links route to the iMessage-game viewer once that ships.

---

## 7. Engine layer

### 7.1 Bridge

`vendor/libfoolish` per `IMESSAGE_GAME_DESIGN.md` §8.1 (`fio_*` C API: new
seeded game, legal moves, apply, per-viewer view, replay encode/decode, msg
envelope). `Engine/` wraps it in Swift: `struct GameState` (decoded from the
packed view bytes — same wire as `cnitro/wasm/wire.h` / the view blob format
`wasm_view_serialize`, `cnitro/wasm/wasm_api.c:487-501`), `enum Move`,
`final class LocalGame` (owns one kernel instance; serial queue; never blocks
main). Golden-vector XCTests pin the bridge to the same fixtures the e2e
suite generates (see `IMESSAGE_GAME_DESIGN.md` §8.2, §18).

### 7.2 Offline bots (the app-only feature)

Extend the `ios-lib` target with the strategy sources so offline opponents are
the real roster: `random`, `handwritten`, `espresso`, `robusta`,
`firecracker`, `gunpowder`, `blackpowder`, `octogen`/`cordite`
(`cnitro/src/*_strategy.c`; ladder documented in `README.md` project 1 and
`cnitro/CORDITE.md`). Strategy selection mirrors the seeded bot roster
personalities (`supabase/seed.sql`). Cordite-class bots deliberate — run
`choose_move` off-main with a thinking indicator, and cap deliberation with
the same env knobs the server uses (`supabase/functions/_shared/bot_strategy.ts`
shows the pattern). Battery guard: cap bot threads at 2 and pause deliberation
when `ProcessInfo.thermalState >= .serious` (same rule as the future Oracle,
`ORACLE_MONETIZATION_ENGINEERING.md` §7).

### 7.3 Replays natively

Decode: `fio_replay_decode` (v5 codec, `cnitro/src/replay.{h,c}`) → step list →
play through `Boards/` with the transport. Encode/share: finished games (online
or offline) → `fio_replay_encode` → `https://foolish.cards/<code>` + QR
(render QR natively; the web does this with `qrcode.react`, `package.json`).
Byte-parity with the server is guaranteed by the shared C; assert round-trips
in tests anyway (the server verifies round-trip before persisting — README
project 3 — match that discipline client-side).

## 8. Backend protocol

The iOS app talks to the SAME backend as the website. No server changes are in
scope for v1 except one dependency: the stale-round guard from
`WEB_RACE_BUG_HANDOFF.md` §5 should land first (the app will send
`intent_round` from day one — do not ship a second client with the old bug).

### 8.1 Protocol facts (verified anchors — study these files before coding)

- **Invoke:** edge functions via Supabase Functions. `create` (new game,
  `src/contexts/ServerContext.tsx:423`), `action` (ALL moves, **binary packed
  body**: `supabase.functions.invoke('action', { body: Blob(packed) })` —
  `ServerContext.tsx:1180`; pack format: `_shared/packed_action.ts`), `meta`
  (lobby/continue/etc. — `ServerContext.tsx:1150` generic invoke), `add-bot`
  etc. per `supabase/functions/` listing.
- **Response wire:** `[fmt | status | reject_code | u32 version]`
  (`supabase/functions/action/index.ts:17`).
- **Realtime channels:** `pv-<user_id>` (personal view feed,
  `ServerContext.tsx:349`), `game-<gameId>` (public/spectator,
  `:519,676`), `gu-<gameId>-<user_id>` (per-player animation feed,
  `src/state/RealtimeAnimationFeed.tsx:76`), `chat:<gameId>` (`:212`).
- **Ordering:** broadcasts arrive unordered; every sequence carries the
  committed `games.version`; drop stale via the version gate
  (`shouldDropStaleSequence`, `src/state/clientReconcile.ts:44-52`).
- **Views:** clients receive per-viewer masked state (`PersonalGame`,
  `_shared/player_views.ts`) — your hand is cards, others are counts.

### 8.2 Port strategy: two stages, deliberately

- **Stage C1 (v1 ships with this): server-confirmed play.** Send action →
  local "in flight" affordance on the touched cards (dim + lock, no movement)
  → animate from the authoritative broadcast. No optimistic state mutation at
  all. On reject: unlock + `.rigid` haptic + toast (incl. the stale-round
  toast). This is simple, correct, and — because the app rides the recently
  optimized move→broadcast pipeline (commits `53955c7`, `49d8448`) — feels
  fine on real networks.
- **Stage C2 (post-v1, only if feel demands): port the optimistic layer.**
  The web's optimistic system is deliberately pure and unit-tested
  (`src/state/clientReconcile.ts` top comment: "no second copy";
  `optimisticOverlay.ts`, `optimisticConflicts.ts`). Port = translate those
  pure functions to Swift **with the same test vectors** (export the TS unit
  fixtures as JSON; run them through both). Never fork the semantics.

`Net/` implements: packed-action encoder (mirror `packed_action.ts`
byte-for-byte; golden vectors), the three channel subscriptions with the
version gate, `PersonalGame` decoding, resync-on-foreground (app returns from
background → refetch snapshot via `meta`, resubscribe, drop stale), and the
bot-bump nudge the web fires (`action type:'bump'`, `ServerContext.tsx:633`).

## 9. Accounts & auth

Follows the auth program in `ORACLE_MONETIZATION_ENGINEERING.md` §4 — the app
must not invent its own scheme:

- v1 supports what the site supports at build time: username+password
  (today's hash-email scheme, `src/contexts/AuthContext.tsx:12-33`) via
  supabase-swift, stored in the Keychain. When the web's real-email upgrade
  ships (verification/reset — Oracle doc §4), the app inherits it by calling
  the same Supabase auth endpoints; design the Settings account block with
  email/verify/reset slots stubbed from day one.
- Guest-first: online quick-match may prompt for a username only, exactly as
  the site does. No wall before play.
- **Account deletion in-app is mandatory** (Guideline 5.1.1(v)): call the
  deletion edge function once it exists (Oracle doc §4 item 4 — if it hasn't
  been built yet, IT BLOCKS this app's submission; coordinate).
- No Sign in with Apple in v1 (own-account systems are exempt from 4.8 —
  verified in Oracle doc §7.3). Add SIWA only alongside the web's OAuth work.

## 10. Billing-ready

The contract: **adding the paid Oracle later must touch only (a) a new module
implementing an existing protocol and (b) flipped flags.** Concretely in v1:

1. `Entitlements/` defines:
   ```swift
   protocol EntitlementsService {
     var current: EntitlementSet { get }            // published/observable
     func refresh() async
   }
   struct EntitlementSet { let oraclePremium: Bool; let source: String? }
   final class FreeEntitlements: EntitlementsService { /* always .free */ }
   ```
   Injected at app root. v1 ships only `FreeEntitlements`.
2. **Feature flags** (`FoolishKit/Flags.swift`, compile-time + a local debug
   overlay): `oracleUI` (false), `paywallUI` (false), `webUpsellLink` (false —
   the US link-out lever from Oracle doc §5, decided later).
3. UI seams already placed: Replay screen reserves the Oracle button slot
   (hidden by flag); Settings reserves a "Foolish Premium" row (hidden).
4. The future implementation slots in as `StoreKitEntitlements`
   (StoreKit 2 + server mirror via App Store Server Notifications →
   `entitlements` table, Oracle doc §5) — none of that code in v1, but the
   module boundary, DI seam, and flags make it a leaf-node addition.
5. **Anti-goal:** no price strings, no product IDs, no StoreKit imports, no
   "coming soon" teasers anywhere in v1 (review-safe and product-honest).

## 11. App Review compliance

Checklist (details in `ORACLE_MONETIZATION_ENGINEERING.md` §7.3):

- 4.2 substance: native UI, offline bots, replays, tutorial — demonstrable in
  a 5-minute reviewer session; App Review notes include a 60-second "what to
  try" script and a test game code.
- 5.1.1(v) account deletion (§9). Privacy labels: account identifier +
  gameplay data, **no tracking, no ATT prompt** (keep Vercel-style analytics
  OUT of the app; if product analytics are added use a first-party events
  table, not an ad-attribution SDK).
- Age rating: card game, no wagering — expect 4+/9+ band under the
  July-2025 rating system; answer the gambling-themes questionnaire "no".
- Encryption export: standard HTTPS only → `ITSAppUsesNonExemptEncryption =
  NO`.
- The iMessage extension rides the same submission when ready
  (`IMESSAGE_GAME_DESIGN.md` §19 M5) — but do NOT block the app's first
  submission on it; the app stands alone.

## 12. Quality bars

- Cold start → interactive Home < 1.5s on an iPhone 12; Table at a locked
  60fps during dealing animation (Instruments trace in CI-mac optional but
  the budget is binding).
- Full offline session (bot game start→finish→replay save) with radio off.
- Memory: < 150MB in normal play (leaves the future Oracle its budget).
- Binary: < 25MB download size in v1 (no assets; the engine is tiny — the
  wasm builds measure in hundreds of KB, `public/oracle.wasm.gz` is 60KB).
- Zero rules logic in Swift (§3) — enforced by review + the golden tests.

## 13. Testing

- **Engine goldens:** shared fixtures with e2e (deal-from-seed, legal-move
  menus, view bytes, replay round-trip) asserted in XCTest — the same
  cross-engine determinism gate as `IMESSAGE_GAME_DESIGN.md` §18.
- **Protocol goldens:** packed-action encode vectors generated by the TS
  implementation (`packed_action.ts`) in e2e, committed as JSON, asserted in
  Swift. Any wire change now fails two CIs — by design.
- **Snapshot tests** for DesignSystem components (light/dark, Dynamic Type,
  ru/ko strings — long-string layouts are the classic ru bug).
- **UITests:** one happy-path per screen; offline bot game start→finish.
- **Live-backend smoke** (manual pre-release): quick-match vs a bot on prod,
  a 2-device human game, replay share both directions (app→web, web→app).
- Beta: TestFlight with the ru-speaking friends cohort before submission.

## 14. Milestones

Prerequisites: none to start A–B. The stale-round server guard
(`WEB_RACE_BUG_HANDOFF.md`) and the account-deletion endpoint (Oracle doc §4)
must land before D and F respectively — they are OTHER work-streams; track
them as external dependencies.

| M | Deliverable | Effort | Acceptance |
| --- | --- | --- | --- |
| **A** | Repo + Xcode foundation: `make ios-lib` (with bot strategies), project/targets/modules per §4, Engine bridge + golden XCTests, DesignSystem tokens + FCard/FHandFan with snapshot tests | 1–1.5 wk | goldens green; component gallery screen renders in the simulator |
| **B** | Offline vertical slice: Home(offline) + Table + Win vs bot roster, full §5 motion/haptics, tutorial port | 2–3 wk | complete offline game feels finished; radio-off test passes; 60fps dealing |
| **C** | Native replays: decode/playback/transport, save list, QR scan/share, universal links | 1–1.5 wk | web replay code plays natively; app-generated code plays on web byte-identically |
| **D** | Online play (Stage C1): Net/ protocol port, auth (guest/username), lobby, live game vs humans+bots, spectate, resync-on-foreground, stale-round toast | 3–4 wk | 2-device prod game start→finish; kill/relaunch mid-game recovers; all reject codes surfaced sanely |
| **E** | Entitlements scaffolding (§10), Settings/About complete, localization pass, accessibility pass | 1 wk | flags off = zero visible billing surface; VoiceOver plays a card |
| **F** | Hardening + review prep (§11): deletion flow, privacy labels, screenshots, TestFlight beta, submit | 1–2 wk | Approved |
| **G+** | iMessage extension integration (other doc), Stage C2 optimistic port, Oracle + StoreKit behind the seams, iPad | — | — |

Total: **~10–13 weeks solo** to approved v1. Order is deliberate: A–C prove
the app with zero backend risk (and produce the iMessage extension's
dependencies as a side effect); D is the long pole and lands on a
already-polished shell; E–F are short tails.

## 15. Risks & open questions

1. **Protocol drift risk:** the web client is the protocol's only spec. M-D
   starts by writing `docs/PROTOCOL.md` extracted from the §8.1 anchors as
   its first task — if extraction finds undocumented behaviors (reconnect
   edge cases, chat, rearrange), spec them there and get them reviewed before
   Swift lands.
2. **Bot-in-binary review risk:** none expected (native AOT C is standard),
   but cordite's deliberation must respect thermal/battery caps (§7.2) or
   field reviews will say "hot phone".
3. **Auth timing:** if the real-email auth rebuild lands mid-build, the app
   adopts it in M-D/E; if it hasn't landed by F, ship with username auth
   (site parity) — but the deletion endpoint is non-negotiable for F.
4. **Push notifications** (turn alerts, challenge invites) are the top post-v1
   ask; they need APNs + server triggers — schedule with the Oracle/server
   work, don't bolt on.
5. **Scope temptation:** the procedural-materials port beyond the card back
   (wool, wood), the Soviet ru theme, iPad, Game Center — all named here so
   they can be explicitly deferred, not rediscovered.
