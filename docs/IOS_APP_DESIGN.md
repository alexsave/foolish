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
(`sdk/c/src/game.c`, `legal.c`, `view.c`, `replay.c`), compiled to WASM for
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
└─ vendor/libfoolish/     Foolish.xcframework built by `make ios-lib` in sdk/c/
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
packed view bytes — same wire as `sdk/c/wasm/wire.h` / the view blob format
`wasm_view_serialize`, `sdk/c/wasm/wasm_api.c:487-501`), `enum Move`,
`final class LocalGame` (owns one kernel instance; serial queue; never blocks
main). Golden-vector XCTests pin the bridge to the same fixtures the e2e
suite generates (see `IMESSAGE_GAME_DESIGN.md` §8.2, §18).

### 7.2 Offline bots (the app-only feature)

Extend the `ios-lib` target with the strategy sources so offline opponents are
the real roster: `random`, `handwritten`, `espresso`, `robusta`,
`firecracker`, `gunpowder`, `blackpowder`, `octogen`/`cordite`
(`sdk/c/src/*_strategy.c`; ladder documented in `README.md` project 1 and
`sdk/c/CORDITE.md`). Strategy selection mirrors the seeded bot roster
personalities (`supabase/seed.sql`). Cordite-class bots deliberate — run
`choose_move` off-main with a thinking indicator, and cap deliberation with
the same env knobs the server uses (`supabase/functions/_shared/common/bot_strategy.ts`
shows the pattern). Battery guard: cap bot threads at 2 and pause deliberation
when `ProcessInfo.thermalState >= .serious` (same rule as the future Oracle,
`ORACLE_MONETIZATION_ENGINEERING.md` §7).

### 7.3 Replays natively

Decode: `fio_replay_decode` (v5 codec, `sdk/c/src/replay.{h,c}`) → step list →
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
  `_shared/common/player_views.ts`) — your hand is cards, others are counts.

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

---

## 16. Milestone implementation guide (for an implementer with no iOS background)

This section turns §14's table into executable instructions. It assumes the
implementer can run shell commands on a Mac with Xcode 16+ installed, and has
never used Swift. Read §3–§13 first; this section references them constantly.

### 16.0 Conventions & ground rules (read once, apply always)

- **Toolchain:** everything about the Xcode project lives in text. Install
  [XcodeGen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`)
  and define the project in `ios/project.yml`; `xcodegen generate` produces
  `Foolish.xcodeproj`. Never hand-edit the `.xcodeproj` — it is a build
  artifact (add it to `.gitignore`). This keeps every project change
  reviewable and LLM-editable.
- **Build & test from the shell**, not the IDE:
  ```bash
  cd ios && xcodegen generate
  xcodebuild -project Foolish.xcodeproj -scheme Foolish \
    -destination 'platform=iOS Simulator,name=iPhone 16' build
  xcodebuild ... test        # runs XCTest bundles
  xcrun simctl boot "iPhone 16" && open -a Simulator   # to look at it
  ```
- **The JSON bridge rule.** Swift never parses the kernel's packed binary
  formats. For every piece of state Swift needs, add a C function to
  `sdk/c/ios/ios_api.c` that emits **JSON into a caller-provided buffer**,
  and decode it in Swift with `Codable`. (Precedent: the kernel already emits
  JSON for the oracle explain dump — `OG_EXPLAIN` in the wasm oracle build.)
  Binary crosses the language boundary in exactly two places: golden-vector
  fixtures (compared as opaque bytes) and the packed action encoder (§16.D3,
  where byte-exactness against the TS implementation is the whole point).
- **No rules in Swift** (§3). If you are writing an `if` about Durak rules in
  Swift, stop and add a C accessor instead.
- **Commit cadence:** one commit per numbered task below, message prefixed
  `ios(<milestone>):`. Run the relevant tests before each commit.
- **Don't touch:** existing `sdk/c/src/*.c` game logic, `supabase/`,
  `src/` web client — except the explicitly listed additive files
  (`sdk/c/ios/*`, `sdk/c/Makefile` new targets, `scripts/gen_ios_goldens.mjs`,
  `public/.well-known/` in §16.C5).
- **When stuck on an Apple-ism** (signing, entitlements, provisioning): the
  fix is almost always in `ios/project.yml` settings or the Developer portal
  (developer.apple.com → Certificates, Identifiers & Profiles). Simulator
  builds need NO signing; only device/TestFlight work does. Do all
  development against the simulator until §16.F.

---

### 16.A Milestone A — foundation

**A1. `make ios-lib` (the C engine as an xcframework).**
Add to `sdk/c/Makefile` (pattern-match the existing native targets for the
source list — do NOT hardcode a list in this doc's spirit; the authoritative
inventory is the Makefile's own native OBJECTS):

```make
IOS_SRC = src/game.c src/legal.c src/view.c src/replay.c src/deal_rng.c \
          $(STRATEGY_SRC)            # reuse the same var the native build uses
IOS_API = ios/ios_api.c
IOS_MIN = -miphoneos-version-min=15.0

ios-lib:
	mkdir -p build/ios/device build/ios/sim
	xcrun -sdk iphoneos clang -target arm64-apple-ios15.0 $(IOS_MIN) -O2 -c \
	    $(IOS_SRC) $(IOS_API) && libtool -static -o build/ios/device/libfoolish.a *.o
	xcrun -sdk iphonesimulator clang -target arm64-apple-ios15.0-simulator -O2 -c \
	    $(IOS_SRC) $(IOS_API) && libtool -static -o build/ios/sim/libfoolish.a *.o
	xcodebuild -create-xcframework \
	    -library build/ios/device/libfoolish.a -headers ios/include \
	    -library build/ios/sim/libfoolish.a    -headers ios/include \
	    -output ../ios/vendor/Foolish.xcframework
```

(Adapt: object files per-directory, add x86_64 sim slice via a `lipo` merge
if CI Macs are Intel; the flags are plain — the wasm build's freestanding
constraints do NOT apply natively.)

**A2. `sdk/c/ios/ios_api.{h,c}` — the `fio_*` shim.** One static `Game`, no
threads inside (the Swift wrapper serializes). Implement, in this order:

```c
// ios/include/ios_api.h  (this header IS the Swift-visible API)
int  fio_new_game(const uint8_t *seed, int seed_len, int n_players); // deal_rng path, game.h:139-151
int  fio_legal_moves_json(int seat, char *out, int cap);   // [{type,cards:[{s,v}]},...]
int  fio_apply_json(const char *move_json);                // one move, validated
int  fio_state_json(int viewer_seat, char *out, int cap);  // per-viewer masked view (view.c)
int  fio_public_state_json(char *out, int cap);            // spectator view
int  fio_actor_mask(void);                                 // bitmask of seats with legal moves
int  fio_game_over(void);                                  // fool seat, or -1
int  fio_bot_choose_json(int strategy_id, int seat, char *out, int cap); // M-B2
int  fio_replay_encode_b32(char *out, int cap);            // M-C
int  fio_replay_decode_b32(const char *code, char *out, int cap); // M-C, steps as JSON
```

The JSON shapes mirror the shared TS types (`@shared/types.ts` `Card`,
`PersonalGame`) — copy field names from there so Swift models and web models
read the same. Write a tiny C unit test (`sdk/c/tests/`) exercising
new→legal→apply→state round-trips before any Swift exists.

**A3. Golden vectors.** Add `scripts/gen_ios_goldens.mjs` (Node, runs the
kernel wasm via the e2e bridge): emits `ios/Fixtures/goldens.json` —
`{seed, n_players, deal_fingerprint, legal_moves_at_step[], state_json_at_step[]}`
for ~20 seeded games. The Swift test in A6 replays the same seeds through
`libfoolish.a` and asserts equality. **This one test is the keystone of the
whole port** — it proves the native build is the same engine.

**A4. `ios/project.yml`** (XcodeGen). Starter:

```yaml
name: Foolish
options: { bundleIdPrefix: cards.foolish, deploymentTarget: { iOS: "15.0" } }
packages:
  SnapshotTesting: { url: https://github.com/pointfreeco/swift-snapshot-testing, from: 1.17.0 }
targets:
  Foolish:
    type: application
    platform: iOS
    sources: [FoolishApp]
    dependencies: [{ target: FoolishKit }]
    settings: { PRODUCT_BUNDLE_IDENTIFIER: cards.foolish.app }
  FoolishKit:
    type: framework
    platform: iOS
    sources: [FoolishKit, Entitlements]
    dependencies: [{ framework: vendor/Foolish.xcframework, embed: false }]
  FoolishTests:
    type: bundle.unit-test
    platform: iOS
    sources: [FoolishTests, Fixtures]
    dependencies: [{ target: Foolish }, { package: SnapshotTesting }]
```

App Group + Associated Domains are added in §16.C5/§16.F (they require the
Developer portal; not needed for simulator work).

**A5. Engine bridge (`sdk/swift/`).** Three files:

```swift
// EngineC.swift — the ONLY file that touches the C API
final class EngineC {
  private let q = DispatchQueue(label: "engine")   // serializes the static Game
  func newGame(seed: Data, players: Int) throws { ... fio_new_game ... }
  func stateJSON(viewer: Int) throws -> Data { /* 64KB buffer, fio_state_json */ }
  // every fio_* gets one throwing Swift method; negative return -> EngineError(code)
}
// Models.swift — Codable structs matching the JSON (Card{s,v}, GameView, Move)
// LocalGame.swift — ObservableObject: published GameView, funcs play(Move),
//   botTurnLoopIfNeeded(); all mutations via EngineC on q, published on main.
```

**A6. Tests + DesignSystem.** `EngineGoldenTests.swift` (A3 fixtures);
`Tokens.swift` + `FCard` + `FHandFan` per §5.2/§5.4; snapshot tests for both
components in light/dark; a `#if DEBUG` `GalleryView` screen listing every
DesignSystem component (this is the reviewer-of-record for §5 — keep it
current forever).

**Definition of done (A):** `make ios-lib` green from clean checkout;
`xcodebuild test` green including golden + snapshot suites; GalleryView
renders in simulator; no signing configured anywhere.

---

### 16.B Milestone B — offline vertical slice

**B1. Bot strategies in the lib.** Extend `IOS_SRC` with the strategy sources
(same roster as `supabase/seed.sql` personalities; the C files follow
`*_strategy.c` naming — take the exact set from the Makefile's bots build).
`fio_bot_choose_json(strategy_id, seat, ...)` calls the same `choose_move`
entry the server bridge uses (`_shared/common/bot_strategy.ts:37` shows the call
shape). Map `strategy_id` ↔ roster names in one C table; expose
`fio_strategy_count/name`.

**B2. Bot pacing & thermal guard.** ✅ DONE, and the pacing half moved to C
(`docs/C_CORE_CONSOLIDATION.md` F2/F3, July 2026). `LocalGame.runBots` is now
one `EngineC.botDrive` call per cycle: the KERNEL picks fairly among
simultaneously-eligible bots, applies the cycle (bundling silent actions), and
returns `delayMs` from the one pacing table. The old plan here — a 600–1200ms
randomized delay, "the server does the same deliberately" — was wrong on both
counts: the server paces at 3000ms with a human watching, and the phone drove
the FIRST eligible seat where the server shuffles. Do NOT reintroduce a Swift
delay constant; `BotDrive.delayMs` is the answer. Note bots now throw in while
the player deliberates, exactly as they do online.

What stays Swift-side is the host's job only: timers, and the thermal guard —
show the thinking indicator immediately for cordite-class strategies; skip
deliberation entirely when `ProcessInfo.processInfo.thermalState >= .serious`
(fall back to `espresso` for that move — never freeze the game).

**B3. Table screen composition.** One `TableView` driven by a `GameView`
value + an `AnimationPlan` (see B4). Zones as §5.4 components; layout in a
`GeometryReader` with fixed proportions: opponents strip 22% height, battles
40%, deck well trailing, hand fan 26%, action bar between. Interaction flow
(the ONLY pattern in the app):

```
tap card → EngineC.legalMoves → is this card in a legal move?
  no  → .rigid haptic, 80ms shake, done
  yes → LocalGame.play(move) → new GameView → diff → AnimationPlan → render
```

**B4. Animation = kernel events.** ⚠️ **`BoardDiff.swift` is CANCELLED — do not
write it** (`docs/C_CORE_CONSOLIDATION.md` F4/A3, owner decision July 2026).
The animation plan is not something a client derives: the kernel already emits
it as the evwire stream, and the website only decodes and plays it. A Swift
diff engine would be a third implementation of "which card flies where" — the
web's `buildEvents` twin and the replay projection being the others — and it
would be legacy the day it was written.

Instead: `fio_apply_json` / `bot_drive` gain an events output (A3), and
SwiftUI renders THAT stream. Everything below still holds — only the source of
the moves changes: `matchedGeometryEffect` ids = card identity (`"\(s)-\(v)"`,
back cards use synthetic slot ids), animated with the single §5.2 spring; deal
choreography staggers cards 40ms apart. Replays (C) and online (D) then play
the same events rather than reusing a diff engine.

Each event carries **`state`** — the board as of that step, already masked for
the viewer (`GameEvent.state: GameView?`, July 2026). This is the same per-step
snapshot the web's evwire has always carried (`snap_len`, `evwire.h`) and
commits as each event's animation lands, and it is what makes the rule above
enforceable: a `bot_drive` cycle applies several actions, so without it the
board could only be drawn at the cycle's FINAL state and the only route back to
the intermediate boards would be a client-side diff. Play an event, commit its
`state`, play the next. `state` is absent (nil) only for an event the kernel
emitted no snapshot for — never a cue to derive one.

Redaction stays the kernel's call: `cards` entries arrive `null` where a card
was dealt/drawn into a hand that is not the viewer's (`mask_cards`). Render a
back — the identity never crossed the bridge.

**B5. Home (offline path) + Win screen + bot picker** per §6 specs. Bot
picker cycles the roster with left/right arrows (web precedent: commit
`7f22749`).

**B6. Tutorial.** Port the script from the web tutorial route
(`src/app/tutorial/`) into a `TutorialScript.json` (steps: forced hands via a
fixed seed chosen to reproduce the web tutorial's deal, highlight target,
copy key). Runs as a `LocalGame` with input restricted to the scripted move.
(If the web tutorial's exact deal can't be reproduced by seed, pick a new
seed and adjust copy — the lesson content is what's being ported, not bytes.)

**DoD (B):** complete bot game start→finish→rematch with radio off; 60fps
during deal on an iPhone 12 (Instruments or on-device FPS overlay); tutorial
completes; UITest drives one full offline game by accessibility ids.

---

### 16.C Milestone C — replays

**C1. Decode.** `fio_replay_decode_b32(code, out)` → JSON array of steps
mirroring the TS `DecodedReplay` (`@shared/replay/core.ts`) — reuse
`replay_decode` (`sdk/c/src/replay.h:100`) + a JSON emitter. Swift:
`ReplayPlayer: ObservableObject` — an index into steps + the B4 diff engine
for rendering; transport = play/pause/step/scrub (VHS feel per web
`ReplayScreen.tsx`).

**C2. Encode/share.** On Win screen and finished offline games:
`fio_replay_encode_b32` → `https://foolish.cards/<code>`; share sheet
(`ShareLink`) + QR (CoreImage `CIQRCodeGenerator`, quiet zone 4 modules,
render at 512px). Assert in tests: encode→decode round-trip equals final
state; and one **cross-platform fixture** — a committed web-generated code
decodes natively to the fixture's expected JSON (generated by
`scripts/gen_ios_goldens.mjs` via the wasm codec).

**C3. Storage.** `ReplayStore`: one JSON file in Application Support
(`[{code, date, fool, players, myResult}]`), newest-first list on the
Replays screen. No database framework — do not add SwiftData/CoreData.

**C4. Scan/paste.** Paste field validating via decode; camera scan with
`AVCaptureMetadataOutput` (QR) behind a camera-permission string
(`NSCameraUsageDescription` — write it now, review requires it).

**C5. Universal links.** Two-sided task: (web) serve
`public/.well-known/apple-app-site-association` (JSON, `applinks` for
`/<code>` and `/m/*` paths — mind that `/<code>` collides with live-game
URLs; include both, the app routes by decode success: replay code → replay
viewer, else game id → §16.D join/spectate, else open Safari). (app)
`com.apple.developer.associated-domains: applinks:foolish.cards` in
project.yml entitlements + URL routing in `FoolishApp`. Device-only feature —
simulator testing via `xcrun simctl openurl booted <url>`.

**DoD (C):** web code plays natively; native code plays on web (manual);
round-trip + cross-platform tests green; list/save/scan all work.

---

### 16.D Milestone D — online play (Stage C1: server-confirmed)

**D0. FIRST TASK: write `docs/PROTOCOL.md`** by reading, in order:
`src/contexts/ServerContext.tsx` (invokes at `:423,633,1150,1180`; channels
at `:212,349,519,676`), `src/state/RealtimeAnimationFeed.tsx:76`,
`src/state/clientReconcile.ts` (version gate `:44-52`; sequences carry full
resulting state `:47-49`), `supabase/functions/_shared/packed_action.ts`,
`player_views.ts`, `action/index.ts:17`. The doc must contain: every edge
function's request/response shape, every channel name + event payload shape,
the version gate rule, and the auth flow. Get it reviewed (PR) before writing
`Net/` — it is the contract, and any surprise found later goes into it first.

**D1. Dependencies & config.** Add `supabase-swift` to project.yml packages.
Config via `ios/Config/{Debug,Release}.xcconfig`: `SUPABASE_URL`,
`SUPABASE_KEY` (the same two the web client uses, README "Quick start" — the
anon key is public by design; still keep it in xcconfig, not source).

**D2. Auth port.** Mirror `AuthContext` exactly (`src/contexts/AuthContext.tsx`):
`nameToEmail` = SHA-256 of uppercased username → first 16 hex chars →
`<hex>@foolish.cards` (`:12-33`, `WEBSITE_DOMAIN` from
`src/constants/constants.ts`); signUp carries
`user_metadata.username` (uppercased) and must locally enforce the
bot-reserved-prefix rejection (`usernameUsesReservedPrefix`,
`src/common/botName.ts` — port the check). supabase-swift persists the
session in Keychain by default. When the real-email auth rebuild lands
(`ORACLE_MONETIZATION_ENGINEERING.md` §4), this module gains
verify/reset flows — leave TODO seams, ship without them if the site has.

**D3. Packed action encoder.** Port `_shared/packed_action.ts` to
`Net/PackedAction.swift` **byte-for-byte**, validated by golden vectors:
extend `scripts/gen_ios_goldens.mjs` to emit `action_goldens.json`
(move JSON → expected bytes hex, ~50 cases covering every action type,
multi-card moves, and — once the web race fix lands — the `intent_round`
field per `WEB_RACE_BUG_HANDOFF.md` §5). POST via
`supabase.functions.invoke("action", body: bytes)`; decode the
`[fmt|status|reject_code|u32 version]` response (`action/index.ts:17`) into a
Swift enum incl. `.staleRound` when the server ships it.

**D4. Realtime feed.** `Net/GameFeed.swift`: subscribe `pv-<user_id>`,
`gu-<gameId>-<user_id>`, `game-<gameId>` (spectate) — payload shapes from
PROTOCOL.md. Apply the version gate (port `shouldDropStaleSequence` — 5
lines). **Stage C1 rendering rule:** take each sequence's full resulting
`PersonalGame`, map to `GameView`, feed the B4 diff engine. Use the
sequence's event list only for ordering/timing hints, not as a second source
of truth. In-flight UX: on POST, mark the played cards `.inFlight` (dimmed,
locked, unmoved); confirm/animate on the broadcast; on reject unlock +
`.rigid` + toast (reject-code strings localized, incl. stale-round copy from
the handoff doc).

**D5. Screens.** Home(online): quick-match = `create` then route by returned
game id; join-by-code = the same game-URL parser as C5. Lobby per §6 (add-bot
via the server flow, ready states over the feed). Table: identical `TableView`
— `LocalGame` and `OnlineGame` both expose the same `GameSession` protocol
(`view`, `play(move)`, `actorMask`); build that protocol NOW, in this
milestone, by refactoring `LocalGame` to it. Win screen: Rematch = the
`meta`/continue flow, optimistically resetting to lobby EXACTLY per
`resetToLobby` (`clientReconcile.ts:10-40` — port its field list; the comment
warns the server reset must match byte-for-byte or the UI snaps).

**D6. Lifecycle & resync.** On foreground/reconnect: refetch authoritative
state (the web pulls via `meta`/game fetch — PROTOCOL.md will pin the exact
call), resubscribe channels, drop stale sequences via the gate, rebuild view.
On websocket drop mid-game: passive banner ("reconnecting…"), never block
input on the banner (the POST path is independent of the feed). Also send the
web's bot-bump nudge where the web does (`ServerContext.tsx:633`).

**D7. Spectate.** `game-<gameId>` public channel + public view JSON — same
TableView, no fan, no action bar.

**DoD (D):** two physical devices (or device+web) complete a prod game;
mid-game force-kill + relaunch recovers to the live table; every reject code
has a user-visible, localized surface; airplane-mode toggling mid-game
recovers; UITest covers quick-match vs bot on a staging project (a dedicated
Supabase project for CI — do NOT point tests at prod).

---

### 16.E Milestone E — entitlement seams, settings, localization, a11y

**E1. Entitlements module** exactly per §10 (protocol, `FreeEntitlements`,
`EntitlementSet`), injected via SwiftUI `Environment`. Unit test: with the
free stub, `oraclePremium == false` and — the real assertion — **no view in
the app reads StoreKit** (grep-based CI lint: `import StoreKit` is forbidden
outside `Entitlements/` and fails the build if found; the v1 `Entitlements/`
itself must not import it either).

**E2. Flags.** `Flags.swift` (`oracleUI=false, paywallUI=false,
webUpsellLink=false`) + a DEBUG-only settings row listing flag states.
Reserved slots: Replay screen right-bar Oracle button position; Settings
"Foolish Premium" row — both `if Flags.oracleUI` (i.e., compiled, invisible).

**E3. Settings & account.** Language override (system/en/ru/ko), haptics
toggle, account block (username, sign-out, DELETE ACCOUNT → the deletion
edge function; if it doesn't exist yet this milestone BLOCKS §16.F — escalate,
don't fake it with a mailto), licenses (single static screen), links
(privacy policy + terms URLs on foolish.cards — coordinate with web).

**E4. Localization.** Write `scripts/gen_ios_strings.mjs`: parse
`src/localization/strings.ts` → merge into `ios/FoolishKit/Localizable.xcstrings`
(String Catalog JSON; en/ru/ko values per key; keys keep the web's names,
new iOS-only keys get an `ios.` prefix and MUST be added in all three
languages in the same commit — CI check: the three language columns have
identical key sets).

**E5. Accessibility pass.** Checklist to execute, not aspiration: VoiceOver
labels on every interactive element (card labels per §5.4 wording); Dynamic
Type xxxLarge on all non-board screens without truncation (snapshot tests at
that size); Reduce Motion honored (diff engine cross-fades instead of moving);
minimum 44pt hit targets (the fan uses expanded hit slop); color-only
information nowhere (trump marked by badge shape + color).

**DoD (E):** flags off → zero billing surface (verified by UITest walking all
screens); strings CI green; a11y checklist committed with each item checked.

---

### 16.F Milestone F — hardening & App Store submission

**F1. Apple accounts & identifiers** (one-time, human-in-the-loop —
the implementer prepares everything and the owner clicks): Apple Developer
Program enrollment ($99/yr); in the Developer portal create bundle id
`cards.foolish.app` (+ `.MessagesExtension` for later), App Group
`group.cards.foolish`, Associated Domains capability; in App Store Connect
create the app record — **decision locked in `IMESSAGE_GAME_DESIGN.md` §9.1:
one record, this app** — name "Foolish — Durak" (check availability;
fallbacks listed in review notes), primary language en, category Games/Card.
Enable automatic signing in project.yml
(`DEVELOPMENT_TEAM: <teamid>`, `CODE_SIGN_STYLE: Automatic`).

**F2. App icon + launch.** Procedural-fern icon: render the §5.3 card-back
fern (one blessed seed — commit it as `ICON_SEED`) at 1024px on
`Color.table`, via a small `swift run` tool committed under `ios/Tools/`;
Xcode 16 single-size icon slot. Launch screen: plain `Color.table` with
centered wordmark — nothing dynamic (launch screens are static by platform
rule).

**F3. Privacy & compliance metadata** (all in App Store Connect + committed
mirror in `ios/Compliance.md`): privacy labels — Data Used to Track You:
none; Data Linked to You: identifiers (user id), user content (game history);
Data Not Linked: none beyond diagnostics-off. `ITSAppUsesNonExemptEncryption
= NO` in Info.plist. Age rating questionnaire: no gambling, no user-generated
content beyond fixed-emoji chat (check what chat ships; if free-text chat is
in scope, moderation answers change — prefer shipping emoji-only chat in v1
to keep the questionnaire clean). Account deletion URL + in-app path.

**F4. Screenshots & store page.** `xcrun simctl` scripted captures on
iPhone 16 Pro Max + iPhone SE sizes: (1) table mid-battle, (2) offline bot
roster, (3) replay with QR, (4) win screen, ru variants for the ru locale
page. Keywords: durak, дурак, card game, подкидной — the store page is the
"durak" search play (`MONETIZATION_ROADMAP.md` phase 1).

**F5. TestFlight → review.** Internal TestFlight build → the owner + ru
cohort for ≥1 week; fix crashes (Xcode Organizer). Submission review notes:
60-second reviewer script ("Play → Offline → beat Espresso; Replays → paste
code `<committed demo code>`; Settings → delete account works"), demo
account credentials, note that the app is fully usable without an account.
First submission WILL likely bounce once — respond within 24h, fix, resubmit;
budget for two cycles in the §14 estimate.

**DoD (F):** approved and live. Post-launch: tag the release, archive dSYMs,
turn on App Store Connect crash reports review as a weekly habit.

---

### 16.G Milestone G+ — pointers only (each is its own work order)

- **iMessage extension:** execute `IMESSAGE_GAME_DESIGN.md` §19 M1–M5 inside
  this project (`FoolishMessages` target in project.yml; FoolishKit already
  provides Boards/Engine/DesignSystem — its M2 is mostly done by this app).
- **Stage C2 optimistic play:** export the web's optimistic unit fixtures
  (`src/state/optimistic*.ts` tests) as JSON; port the pure functions to
  `Net/Optimistic/`; run both implementations against the same vectors in
  both CIs; only then wire into `OnlineGame`.
- **Oracle + billing:** implement `StoreKitEntitlements` in `Entitlements/`
  (StoreKit 2 `Transaction.currentEntitlements` + server mirror per
  `ORACLE_MONETIZATION_ENGINEERING.md` §5/§7); flip `oracleUI`; native oracle
  = new `wasm-oracle`-equivalent native target (the C already exists —
  `docs/INFINITE_ORACLE_DESIGN.md`; threading per Oracle doc §7.1).
  Blocked on: auth rebuild + entitlements backend.
- **iPad:** revisit `TableView` proportions + pointer hover; no new logic.
- **Push notifications:** requires APNs certs + server-side send on
  turn events — design doc first (server work; coordinate with the
  Supabase/edge-function owners).

---

## 17. Implementation status & handoff (2026-07-14 · updated 2026-07-15)

*What has actually been built vs. what remains, so a fresh implementer (with a
Mac + Apple account) can finish. Written after a from-scratch build pass that had
no Mac/Xcode and no live backend — so the C engine is proven on Linux, and the
Swift is written-to-compile but has NOT yet been through `xcodebuild`.*

### 17.1 Branches — all merged; work from `main`

Both branches this section used to track are on `main`: the account-deletion
endpoint merged (`4108a5e`), and the app itself (`ios/`, `sdk/c/ios/`) is on
`main` with subsequent D-milestone commits on top (kernel packed-view decode,
realtime feed, online session/service — see 17.5). There is no in-flight iOS
branch; start new work from `main`. Live milestone summary:
`ios/README.md` § "Milestone status".

### 17.2 Prerequisites (both handled)

1. **Stale-round guard** (`WEB_RACE_BUG_HANDOFF.md`) — **landed** (migration
   `20260713120000_round_epoch_stale_guard.sql`, `e2e/race_conditions.test.ts`,
   `intent_version` in the wire, `REJECT_STALE_ROUND`, localized strings). The
   iOS `PackedAction` already sends `intent_version` and decodes stale-round,
   so the app never reintroduces the bug. Nothing to do.
2. **Account deletion** (`ORACLE_MONETIZATION_ENGINEERING.md` §4) — **merged to
   `main`** (`4108a5e`: migration `20260714120000_account_deletion.sql`,
   `supabase/functions/delete-account`, web page). Deploys with `main`; still
   verify once against a live DB before the F submission relies on it.

### 17.3 What is DONE and verified (Linux, no Mac)

- **C engine bridge** (`sdk/c/ios/`, `make ios-lib`/`ios-smoke`/`ios-goldens`/
  `ios-view-test`): all green. `ios-smoke` drives a full game + replay round-trip
  through the `fio_*` API; `ios-view-test` proves the server packed masked-view
  decodes through the SAME kernel as offline (view + legal moves match). The
  bridge is the same `game.c`/`legal.c`/`view.c`/`replay.c` as the wasm build.
- **Native replay codec** (encode+decode, base32 + shared `replay.c`) —
  round-trip proven; byte-parity with the server by construction.
- **Verified wire primitives** (unit-tested in isolation): `PackedAction` (awire,
  matches `awire.h`), `Auth.nameToEmail` (golden vectors), `VersionGate`.

### 17.4 What is BUILT but needs the first `xcodebuild` (Mac)

All Swift under `ios/`. Two independent compile-review passes were run and their
findings fixed, but Swift was never compiled. First Mac session:
`cd sdk/c && make ios-lib && cd ../ios && xcodegen generate && xcodebuild ...
build test`, then fix any surfaced nits (most likely: supabase-swift 2.x exact
API shapes — see 17.6). Offline play, replays, tutorial, settings, entitlements,
localization, and the DEBUG gallery are all implemented.

### 17.5 Online layer — wire RESOLVED (implemented from the web source)

The three items previously flagged as `TODO(D0)` were extracted from the web
client and implemented — `docs/PROTOCOL.md` is now the resolved contract, not a
skeleton:

1. **`create` → game id + seat**: the response is the enveloped packed game;
   `Net/PackedGame.decode` yields `{gameId, seat, version, view}` (native mirror
   of `decodePackedGame`). No seam.
2. **Local seat**: read from the packed envelope (byte `[2]`) — `OnlineGame`
   learns it from the feed. No seam.
3. **Realtime transport**: the authoritative masked view arrives as
   **`player_views` Postgres-change rows** (not a broadcast); `GameFeed`
   subscribes those and decodes `row.view` via `PackedGame`. The
   `animation_events` broadcast is Stage-C2 animation polish, deferred.

New verified C: `fio_view_from_packed_json` + `fio_legal_from_packed_json` decode
the server's masked-view wire through the SAME kernel as offline
(`make ios-view-test`, green). So online renders and computes legal moves with
zero rules in Swift.

The supabase-swift 2.x API shapes used in `Net/` were **verified against the SDK
source** (Functions invoke overloads + `FunctionInvokeOptions(body: some
Encodable)` raw-Data special-case; realtime `postgresChange(_:schema:table:
filter: RealtimePostgresFilter)` + `await channel.subscribe()` + `decodeRecord`;
auth `session`/`signIn`/`signUp`/`signOut`; `AnyJSON.stringValue`). One real bug
was found and fixed there (the Postgres-change `filter:` is a
`RealtimePostgresFilter`, not a `String`).

**What actually remains for online is runtime testing, not wire or API** (§17.6
step 5): the first `xcodebuild` (surface any residual nits), then end-to-end
against a staging Supabase project. `docs/PROTOCOL.md` §9 lists it.

### 17.6 First-Mac-session checklist

1. `cd sdk/c && make ios-lib` (needs Xcode CLT), then `make ios-smoke`,
   `make ios-view-test`, `make ios-goldens` — confirm all green.
2. `brew install xcodegen`; `cd ios && xcodegen generate`.
3. `xcodebuild -scheme Foolish -destination 'platform=iOS Simulator,name=iPhone 16' build test` — fix compile nits (Engine/DesignSystem/Boards first, then Net/ supabase-swift shapes).
4. Record snapshot references (`ComponentSnapshotTests`, set `record=true` once, commit `__Snapshots__`, set back).
5. Smoke-test online end-to-end vs a staging Supabase project (never prod, §16.D DoD). The wire is resolved (§17.5, `docs/PROTOCOL.md`) — this step is runtime verification, not protocol work.
6. Set `SUPABASE_URL`/`KEY` in `ios/Config/*.xcconfig` (account-deletion endpoint is already merged/deployed with `main`; verify it once against a live DB).
7. Milestone F (needs Apple account): signing (`DEVELOPMENT_TEAM` in project.yml), render the app icon (`swift run --package-path ios/Tools/IconGen icongen …`), screenshots, privacy labels (`ios/Compliance.md`), TestFlight, submit.

### 17.7 Known gaps / deferred (not blockers)

- Replay **board playback** renders the decoded event stream under a transport;
  projecting each step onto the full board via the diff engine is a follow-up.
- Cross-zone card-flight animation — today the board springs on state change.
  Blocked on A3 (kernel events), NOT on a Swift diff engine: `BoardDiff.swift`
  is cancelled (§16.B4).
- Camera QR **scan** (paste works); full a11y checklist pass; snapshot references.
- `game_snapshots.extras` replay-name blob is not anonymized on deletion
  (documented in the migration) — needs replay re-encoding.

### 17.8 Phone layout, naming, and consolidation studies (2026-07-15)

Companion docs landed after this section was last updated (each merged from
two independent design passes on 2026-07-15); where they conflict with the
body above, they win:

- **`docs/IOS_PHONE_LAYOUT.md` + `docs/ios-phone-layout.html`** — the SE
  8-player table study. Confirms §16.B3's banded skeleton, fixes its numbers
  (opponent *arc* with zigzag at 6+ seats, one-tap roster sheet, fan scrub
  for big hands), and reverses one §5 decision: the table wears the
  website's wool/wood/fern materials, not flat felt.
- **`docs/IOS_BOT_NAMING.md`** — iOS-only display mapping of the explosive
  bot names to Russian cities: the 10-rung km-to-Moscow ladder
  (octogen → Moscow), localized (en/ru/ko now, verified exonym table for
  later locales), `BotNames.swift` spec. Also fixes the raw-`%` name bug on
  the online table in passing.
- **`docs/IMESSAGE_IMPLEMENTATION_HANDOFF.md`** — the extension's canonical
  work order (most of the old plan's M1–M2 fell out of this app's build;
  seed is 32 bytes; actions are seat-prefixed awire frames; format 6
  shipped as the *other* design). `IMESSAGE_GAME_DESIGN.md` stays the
  protocol spec; its §20 points here.
- **`docs/C_CORE_CONSOLIDATION.md`** — before writing `BoardDiff.swift`
  (§16.B4), read this: the plan is to consume kernel-emitted events instead,
  and to move the bot roster/knobs + drive cycle + pacing into C (it also
  documents a live divergence: offline cordite currently runs un-knobbed at
  arena budget).

### 17.9 watchOS (designed, parked)

The watch client's design study is complete and merged (#96): final layout is
Option H, implementor handoff in `docs/WATCHOS_SPEC.md`, interactive mockups in
`docs/watchos-layout.html`, decision record in `docs/WATCHOS_LAYOUT.md`. The
structural plan (`docs/WATCHOS_APP_PLAN.md`) still governs App-Store bundling,
connectivity, and sequencing: a watchOS target inside this app's record,
shipped **after Milestone F**. Only build-system prerequisite: watchOS slices
in `make ios-lib`. Do not start it before the phone app ships.
