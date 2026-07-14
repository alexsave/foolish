# Foolish on watchOS — Full Design Plan

*Fills out the design study `docs/WATCHOS_LAYOUT.md` (branch
`claude/ios-redesign`, commit `adb76a6`) into a buildable plan, incorporating
the owner's real-size screen sketches (July 2026, reproduced verbatim in §4).
The study's core decisions stand — token cards, circular seats, Crown + tap,
no drag, plain high-contrast design distinct from the phone's wool/wood/fern
identity. This doc adds: the App-Store structural answer (§1), the sketch-driven
screen revisions (§4–§5), architecture and connectivity (§6–§8), complications
(§9), milestones (§11), and gotchas (§12). That file stays untouched on its
branch; this is the plan of record. The iOS app this builds on is merged on
main (`ios/`, FoolishKit with Engine/Net/DesignSystem/Boards).*

---

## 1. The important question first: do we need an iOS app?

Short answers, then the reasoning:

| Question | Answer |
| --- | --- |
| Does a watch app require an iOS app? | **No.** Independent ("watch-only") watchOS apps are fully supported and get their own watch App Store listing ([Apple: creating independent watchOS apps](https://developer.apple.com/documentation/watchos-apps/creating-independent-watchos-apps)). |
| Can the watch app come later? | **Yes, cleanly.** Adding a watchOS target to an existing iOS app in a later update is the standard, penalty-free path. This is the *good* direction. |
| Should it be bundled from the start? | **Bundle it whenever it's built — and it's already "from the start," because the iOS app exists on main.** The trap direction is the reverse: shipping a *separate* watch-only app now and wanting it under the iOS app's record later means a new app (same class of one-way door as the iMessage standalone trap, `IMESSAGE_GAME_DESIGN.md` §9.1). |
| Bundled vs independent — pick one? | **Both.** A bundled watch app can be marked "runs without iOS app" (independent-capable). One caveat: independence, once shipped, **cannot be reverted** ([forums](https://developer.apple.com/forums/thread/130351)) — but we'd never want to revert it, so accept it. |

**Decision:** the watch app is a **watchOS target inside the existing
`cards.foolish.app` record** (same `ios/project.yml`, same submission), marked
independent-capable. It shares `FoolishKit`. It ships *after* the phone app's
online milestone (§11 sequencing) — there is zero cost to adding it later and
a real cost to building it before the phone's `Net/` layer is proven.

Practical consequences of "bundled + independent-capable":

- iPhone users get it automatically with the iOS app (the overwhelmingly
  common case) and auth flows from the phone (§8).
- Watch-only users (no iPhone) can install from the watch App Store; they get
  offline-vs-bot play only in v1 (no way to type credentials on a watch worth
  supporting yet — honest scoping, §8.3).
- One privacy label set, one review, one app identity.

---

## 2. Product thesis: the wrist is for *turns*, not for *sessions*

The phone/web game is a session device; the watch is a **latency device**. The
product is: *know it's your move, see the table in one glance, play the common
move in two taps* — without pulling out the phone. Durak correspondence-style
(slow online games vs friends, or bot games) is where a watch shines; nobody
plays a 6-player blitz on their wrist.

v1 scope, in priority order:

1. **"Your turn" complication + notification** — the single highest-value
   feature (§9). A wrist wedge that flips to "ход!" is the whole reason this
   app exists.
2. **Table screen** — read-mostly glance of the live game (§5.1).
3. **Action screen** — full move play via Crown + tap (§5.2). The study's
   staged plan (Pickup/Done first, full play later) is collapsed: the sketch's
   grid is simple enough to build full play in v1.
4. **Offline quick game vs a cheap bot** — makes the app real without the
   phone and gives review something standalone to see.

Non-goals (v1): replays on the watch, the Oracle (never — thermal/battery/
screen all wrong), spectating, lobby management (games are created on
phone/web; the watch *plays* them), chat, tutorial.

---

## 3. Design language: simpler than the phone, on purpose

The study's §1 call is right and the sketches confirm it: **throw out the
textures and rectangular cards entirely.** The watch design is:

- **Black canvas** (`#000` — true black; OLED and always-on both want it, and
  it makes the phone's felt identity irrelevant rather than badly imitated).
- **Token cards**: value-first, suit as color + tiny glyph. Red suits
  `#FF453A`, black suits rendered white (on black), trump gets the single
  accent treatment (brass `#D8B24A` ring or glyph — the ONE accent on screen).
- **SF Rounded Bold** for card values and counts (rounded reads better at
  token size than the phone's condensed display face — different identity,
  deliberately).
- No animation beyond: token lift on select, a 120ms slide when a battle
  updates, and haptics. The watch communicates through **haptics first**
  (turn = `.notification`, illegal = `.retry`/rigid, move sent = `.click`).
- Layout budget is exactly what the owner's real-size sketches show (§4):
  ~16 "characters" of width — everything is designed at **41mm first**
  (324×394 px; ~162×197 pt) and scales up to 45/49mm by spacing, never by
  adding elements.

---

## 4. The owner's real-size sketches (normative)

Reproduced verbatim; these are calibrated to real 41mm size (default vim font)
and set the **density budget** — if a design needs more elements than fit in
these frames, the design is wrong. Bottom line of the action sketch sits at
line 11: ~11 text rows of vertical budget.

**Sketch 1 — Action (hand) screen:**

```
┌────────────────┐
│                │
│ J   2   J   J  │
│                │
│ J   J   4   J  │
│                │
│ 7   10  6   J  │
│   /--------\   │
│   | Attack |   │
│   \--------/   │
└────────────────┘
```

**Sketch 2 — Table screen:**

```
┌────────────────┐
│#D           #Di│
│   _   6        │
│  |2|      3    │
│   ^            │
│ 6   J > 9    8 │
│                │
│   1       9    │
│       6        │
│Tr 7 7 A 9 5  Pl│
└────────────────┘
```

**Interpretation (agreed reading; deltas vs the study called out):**

- Action: a **4-column token grid**, ~3 rows visible (12 tokens), Crown
  scrolls for bigger hands; **one contextual pill button** at bottom center
  ("Attack" here). Delta vs study: 4 columns not 5 (real size says 4 is the
  max that stays tappable), and the resting state shows **one primary action**
  — the two-button case (`Cover`/`Pass` ambiguity) still exists but is the
  exception, not the layout default.
- Table, top corners: `#D` = deck count (top-left), `#Di` = discard count
  (top-right) — matches the study.
- Table, ring: opponent seats as **bare hand-count numerals** placed on the
  ring (6, 3, 2, 8, 9, 1, 6 in the sketch — up to 7 opponents fit). The
  boxed `|2|` with over/underscore marks = the **defender highlight** (drawn
  as a rounded ring around the count, brass). Delta vs study: counts are bare
  numerals, not circled chips — at real size the chip outline costs more than
  it gives; the *defender's* ring is the only enclosure.
- Table, center: battles inline as **`attack > cover`** (`J > 9` = jack
  covered by a 9 — of trump or same suit, the engine knows). Uncovered attack
  = bare token. Multiple battles lay out left-to-right on the center line;
  Crown rotates focus when they overflow (study's `1/3` rotator survives, as
  Crown-driven horizontal pan of the center strip).
- Table, bottom strip (NEW vs the study — the sketch's biggest revision):
  `Tr <trump>` at left (flipped trump card while deck lives; collapses to the
  suit glyph after), then a **mini preview of your own hand** (`7 A 9 5` —
  first ~5 values, suit-colored, unselectable), then `Pl` = the play button
  into the Action screen. The study kept your cards off the Table screen
  entirely; the sketch overrules it — a *peek* of your hand belongs on the
  glance screen (you can often decide "I'm picking up" from the glance alone).
  It stays read-only; all commits happen on the Action screen.

---

## 5. Screen specs

### 5.1 Table screen (glance, read-mostly)

- Data: the same per-viewer `PersonalGame` view the phone renders
  (`FoolishKit` `GameView`), one screen, no scrolling except the center
  battle strip (Crown).
- Elements, exactly the sketch's: deck count, discard count, opponent-count
  ring with defender ring + attacker dot + eliminated dimming (study §2 seat
  math: seats equally spaced, θᵢ = −90° + i·360°/n, R ≈ 0.42·min(w,h); you
  are implicit at 6 o'clock and not drawn — the bottom strip is "you"),
  center battle strip with `attack > cover` tokens, bottom strip
  (trump · hand peek · `Pl`).
- **Top-center back affordance** (the one addition to the sketch): a faint
  `‹` glyph, or the current opponent's short name, in the **empty gap between
  the deck (top-left) and discard (top-right) counts** — the sketch's
  `│#D           #Di│` row is wide open in the middle. Tapping it returns to
  the Games list; it also signposts the swipe-right gesture (§5.3). It lands
  in confirmed-empty space, so it costs **no corner** — which is the point,
  since all four corners are spoken for.
- Names: **dropped from v1** (the study's arced-name idea is lovely and does
  not fit the real-size budget; the ring is anonymous counts. Names live in
  the game list, §5.3). Revisit on 49mm only.
- Tap targets: whole bottom strip → Action screen; center battle → Action
  screen with that battle pre-targeted (this quietly solves the study's
  "cover targeting" gap: ambiguous covers are resolved by entering from the
  battle you mean to cover).
- Update model: re-render on every fresh snapshot from the polling probe or a
  push-triggered resync (§7); a subtle 1-second brass flash on whatever
  changed.

### 5.2 Action screen (play)

- The sketch's 4-column token grid of your full hand, sorted the same way the
  phone sorts. Crown scrolls vertically (`.digitalCrownRotation` bound to the
  ScrollView). Tap toggles selection: selected tokens lift 2pt and get a white
  ring (`[♦] Q` in the study's notation).
- Bottom pill(s) = **the kernel's legal menu for the current selection,
  nothing else** — the same hard rule as everywhere
  (`IMESSAGE_GAME_DESIGN.md` §17.16; phone `GameSession` exposes it). Single
  pill in the common case (sketch); the genuine `Cover`/`Pass` dual case
  renders two half-width pills; no selection while defending shows `Pickup`,
  attacker with covered table shows `Done`.
- Commit path: pill tap → `game.play(move)` (same `GameSession` protocol the
  phone uses — `ios/FoolishKit/Engine/GameSession.swift`) → in-flight lock on
  the played tokens → confirm/animate on the feed echo; reject → rigid haptic
  + the tokens unlock (server-confirmed model, identical to the phone's
  Stage C1; `IOS_APP_DESIGN.md` §8.2).
- Edge: hand > ~24 cards (post-pickup monsters) — grid just scrolls; no
  special case.

### 5.3 App structure & navigation — "back to Games" without a corner

The problem: the Table screen's four corners are all taken (deck count,
discard count, trump, `Pl`), so there is no corner to hang a "back to the games
list" control on. The answer is to make the return path a **gesture, not a
button** — it costs zero corner real estate.

**Structure: one horizontal three-page pager** (SwiftUI `TabView`,
`.tabViewStyle(.page)`) — no `NavigationStack`, because a nav bar would eat the
vertical budget the §4 sketches are calibrated to; no tab bar; no menus:

```
   ┌─────────┐      ┌─────────┐      ┌─────────┐
   │  Games  │  ‹───│  Table  │───›  │ Action  │
   └─────────┘ swipe└─────────┘ swipe└─────────┘
     (root)      right  §5.1    left    §5.2
```

- **Games (left page):** active games (online, from the phone's account) +
  "Quick game vs bot" (offline); each row: opponent set, turn state ("your
  move" in brass), hand count. Tapping a game points Table/Action at that game
  and swipes to Table.
- **Back to Games from Table = swipe right** — the same left/right swipe you
  already use for Table↔Action, extended one page left. This is the whole fix:
  a directional gesture needs no corner. Swiping right from **Action** goes to
  Table, then Games (two swipes), or jump straight back after a move via the
  §5.1 top-center chip.
- **Discoverability (edge-swipes don't advertise themselves on watchOS):** the
  Table screen shows a faint **top-center "‹" / opponent-name chip** in the
  confirmed-empty gap between the two count corners (§5.1) that also returns to
  Games. The gesture is the mechanism; the chip is the sign that it exists.
- **Settings** is reached from the Games page (a footer row), not the pager —
  it's rare and doesn't deserve a page or a corner.

Gesture budget (important): **horizontal swipe is reserved globally for the
pager**, so no screen may use a horizontal swipe for in-content actions. This
is already true — Table's battle-strip overflow pans by **Crown** (§5.1, not
swipe), Action's grid scrolls by **Crown** and selects by **tap** (§5.2). One
axis for navigation, the Crown for within-page — they never collide.

---

## 6. Architecture & code reuse

- **Target:** `WatchFoolish` in `ios/project.yml` (XcodeGen), platform
  watchOS 10+, bundled in the `cards.foolish.app` record, independent-capable
  flag on. New UI module `WatchUI/` (the phone's `Boards/` is not reused —
  different design language by §3; reusing it would drag the felt identity in).
- **FoolishKit compiles for watchOS — with one carve-out:** `Engine/` (the C
  bridge), `Models.swift`, `GameSession.swift`, and `Net/`'s auth +
  edge-function-invoke + packed-action code all reuse cleanly (supabase-swift
  auth/functions are URLSession-based and run on watchOS). **`Net/GameFeed`
  (Supabase Realtime, a websocket) does NOT come along** — websockets are
  banned on watchOS (§7); the watch gets a small `WatchNet/PollingFeed`
  implementing the same feed-facing interface via the §7 version probe. Add a
  watchOS destination to the FoolishKit target; CI builds both.
- **libfoolish.a gains watchOS slices** (`arm64-apple-watchos`,
  simulator slice) in the `ios-lib` Makefile target — same plain-C compile,
  minutes of work.
- **Bots on the watch:** cheap heuristics only (`espresso`/`handwritten`
  class) for the offline quick game. No cordite/octogen deliberation
  (battery), enforced by not linking the MC sources into the watch slice.
  **No Oracle, ever** (§2).
- The **no-rules-in-Swift** rule applies with extra force: the watch UI is so
  reduced that hand-rolled shortcuts will tempt ("just show Pickup when
  defending") — every button still comes from the kernel's legal menu.

## 7. Connectivity model (online play) — and the phone-free answer

**Can the watch play online without the phone nearby? Yes — by design.**
watchOS routes networking transparently over three transports: the paired
iPhone when nearby, **known Wi-Fi networks when the phone is away**, and
**LTE on cellular models**. All of our traffic is plain HTTPS (below), which
works identically on all three. Once auth has been handed off once (§8), the
watch refreshes its own Supabase tokens over HTTPS and needs the phone for
nothing. Sign-in is the only phone-required moment.

**The constraint that shapes the transport: no WebSockets on watchOS.**
Apple restricts low-level networking (including `URLSessionWebSocketTask`)
to audio-streaming apps — third-party watch apps get HTTP only
([TN3135: Low-level networking on watchOS](https://developer.apple.com/documentation/technotes/tn3135-low-level-networking-on-watchos),
[forums](https://developer.apple.com/forums/thread/714796)). So Supabase
Realtime (a websocket) **cannot run on the watch**, phone nearby or not.
Consequence: **the watch is a polling client, not a realtime client** — which
happens to fit the §2 thesis (turn-latency device, correspondence pace)
perfectly:

- **Actions (the write path):** unchanged — the same `action` edge-function
  HTTPS POST the phone uses (`docs/PROTOCOL.md`). Works phone-free as-is.
- **State freshness (the read path), while frontmost:** short-poll the
  authoritative snapshot every **~3s while it's not your move** (and stop
  polling entirely once it is — the state can only change again after *you*
  act, modulo multi-actor throw-ins, so poll at ~10s then). Implement as a
  cheap **version probe**: the client sends its last-applied `games.version`;
  a tiny edge-function path answers "unchanged" (no body) or returns the
  fresh per-viewer snapshot. The version field already exists on every game
  row and response (`action/index.ts:17` wire); the probe is a ~20-line
  addition to the `meta`/fetch path, and the phone can use it for foreground
  resync too.
- **Not foreground:** no polling, no sockets — freshness comes from APNs (§9)
  and on-activate resync. Every screen renders instantly from the last
  snapshot, then refreshes.
- **Budget check:** polls happen only while the app is frontmost (watchOS
  suspends it when the wrist drops, throttling this naturally). A 10-minute
  wrist-heavy game ≈ 100–200 probe invocations; at the §2 usage pattern this
  is noise against the Supabase quota (and Pro is assumed once money moves —
  `ORACLE_MONETIZATION_ENGINEERING.md` §14). Revisit only if watch DAU gets
  large enough to matter, in which case: longer poll interval, not sockets.
- **Optional later (W5): phone-proxy fast path.** When the iPhone *is*
  reachable and has the app open, WatchConnectivity could push feed updates
  to the watch and pause the polling. Pure optimization — do not build it
  until polling demonstrably annoys someone; the polling path must remain,
  since it is the only phone-free path.
- **Latency posture:** server-confirmed play only (no optimistic layer on the
  watch, ever — the phone's Stage C2 explicitly does not port here; the
  in-flight token lock is the entire affordance). A 3s-stale table is fine on
  a device you glance at; a wrong table is not.

## 8. Auth

1. **Primary (bundled case): token handoff from the phone.** On first watch
   launch, `WatchConnectivity` transfers the Supabase session (refresh token)
   from the iPhone app; the watch stores it in its own Keychain and is
   thereafter independent (refreshes its own tokens). Zero typing.
2. **Re-auth / drift:** if the watch session dies and the phone is
   unreachable, show "Open Foolish on your iPhone to reconnect."
3. **Watch-only users (no iPhone):** offline quick game works signed-out;
   online is honestly gated: "Online play needs the iPhone app (for now)."
   Revisit only if watch-only demand materializes (a login-code flow —
   watch shows a short code, user enters it on web — is the future answer;
   specced here so nobody invents OAuth-on-a-watch).

## 9. Complication + notifications (the actual killer feature)

- **Complication** (WidgetKit accessory families: `accessoryCircular`,
  `accessoryCorner`, `accessoryRectangular`): shows Foolish + turn state.
  Circular: the jester "Д" normally; **brass "ход!"/"your move" state** when
  any game awaits you. Rectangular adds "vs Sveta · 12 cards left".
- **Timeliness requires push.** Complication/widget refresh without push is
  best-effort background budget (many minutes to hours stale — unacceptable
  for "your turn"). The dependency is named in `IOS_APP_DESIGN.md` §15.4:
  **APNs turn notifications need server work** (an edge-function hook on
  turn-advance → APNs). The watch plan *depends on that work-stream* for its
  headline feature; sequence it first (§11 W0).
- Notification: "Ваш ход — Boris covered your 9♠" with a **Play** action that
  deep-links straight to the Action screen, and a **Pickup** quick-action
  (the one move safely committable from a notification — it needs no
  selection; still validated server-side like any action).
- Until push exists: ship the complication as a launcher (static icon +
  last-known state, clearly timestamped) — still useful, honestly stale.

## 10. Review & compliance notes

- Bundled watch apps ride the iOS app's record, privacy labels, and age
  rating; the watch binary adds its own icon/screenshots (41mm + 45mm sets).
- Independent-capable means review may test it without a phone: the offline
  quick game + signed-out states must be complete (they are, by §2 scope).
- No new data collection (nothing beyond the existing account/gameplay
  labels). No HealthKit, no location, no ATT.

## 11. Milestones

Sequencing note: start only after the phone app's online milestone (D) is
merged — the watch reuses `Net/` as-is. W0 can start immediately though,
because it's server-side.

| M | Deliverable | Effort | Acceptance |
| --- | --- | --- | --- |
| **W0** | APNs turn-notification pipeline (server): edge-function hook on turn advance → APNs to iOS + watchOS; token registration in `Net/` | ~1 wk (server + phone) | phone gets a turn push in prod; watch inherits it |
| **W1** | Target scaffolding: `WatchFoolish` in project.yml, FoolishKit watchOS destination, libfoolish watchOS slices, golden tests green on watch simulator | 2–3 d | `xcodebuild test` on watchOS simulator passes engine goldens |
| **W2** | Offline vertical slice: token components, Action screen (Crown grid + kernel pill bar), Table screen (§5.1), quick game vs cheap bot, haptics map | 1.5–2 wk | full offline game on a real 41mm watch; every element within the §4 density budget |
| **W3** | Online: auth handoff, game list, the version-probe endpoint (server) + `PollingFeed`, on-activate resync, in-flight/reject UX | 1–1.5 wk | play a live prod game phone-created, watch-played **with the iPhone powered off** (known Wi-Fi); kill/relaunch recovers |
| **W4** | Complication + notification actions (§9), always-on dimming pass, 45/49mm layout pass, screenshots, ship with the next iOS app update | ~1 wk | complication flips to "your move" within seconds of the turn (with W0 live); approved |

Total: **~5–6 weeks** solo, of which W0 is shared infrastructure the phone
wants anyway.

## 12. Gotchas

1. **Independence is one-way** (§1) — fine, intended; but never ship a
   *separate* watch-only app record.
2. **No websockets at all, ever, on watchOS** (TN3135 — restricted to audio
   apps; §7). Do not import supabase-swift's Realtime module into the watch
   target (CI lint: `import Realtime` forbidden in `WatchFoolish`/`WatchUI`);
   the polling probe is the only read path. Any design that assumes live
   updates — foreground or not — is wrong; poll + push + on-activate resync
   is the whole model.
3. **Complication budget:** without push, WidgetKit refresh is quota-limited —
   don't promise freshness the OS won't give (§9's staleness timestamp).
4. **WatchConnectivity is flaky by nature** (queued, eventual): the auth
   handoff must be retriable and idempotent; never block the UI on it.
5. **Always-on display** renders the Table screen dimmed and update-throttled
   — design it legible in the dimmed state (white-on-black tokens already
   are; the brass defender ring must not be the only turn cue — pair with the
   bottom-strip text).
6. **Crown focus:** only one view owns `.digitalCrownRotation` per screen —
   the Action grid owns it there; the Table's battle strip owns it there.
   Split ownership crashes into focus bugs; keep one owner.
7. **41mm is the design surface** — build there, scale up; the reverse always
   fails (§4's density budget is the contract).
8. **Session drift:** watch and phone refresh tokens independently after
   handoff; Supabase refresh-token rotation
   (`supabase/config.toml`: `enable_refresh_token_rotation = true`,
   `refresh_token_reuse_interval = 10`) tolerates both refreshing — but test
   the both-devices-offline-a-month case; the §8.2 re-auth path is the net.
9. **Do not port the phone's Boards/** — the design languages must not blend
   (§3); duplication of ~4 small views is cheaper than a themable board.
10. **The pager must not wrap and must open on the right page** (§5.3): Games
    is a hard left end, Action a hard right end — no carousel wrap (swiping
    right off Games or left off Action goes nowhere, not around). Opening a
    game from the list lands on **Table**, not Games; a "your move" push opens
    directly on **Action**. Horizontal swipe is reserved for the pager
    globally — no in-content horizontal swipes (§5.3 gesture budget); combine
    with gotcha #6 (Crown = within-page) for the full input map.

## 13. Open questions (owner input wanted, none blocking W0–W1)

1. Bottom-strip hand peek: first 5 cards, or 5 *strongest* (kernel-sorted)?
   Sketch reads as first-5; strongest-5 may glance better. Decide on-wrist in W2.
2. Battle strip overflow: Crown-pan (this plan) vs the study's `◀ 1/3 ▶`
   pager — decide with a real 4-battle hand on the wrist in W2.
3. Notification quick-action set: `Pickup` only (this plan) or also `Done`?
   `Done` is equally selection-free; add it if the notification UI carries
   two actions cleanly.
4. Offline bot on watch: `espresso` fixed, or expose the picker? (Plan:
   fixed; the picker is phone surface.)
