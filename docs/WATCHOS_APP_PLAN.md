# Foolish on watchOS — Full Design Plan

> **Status (2026-07-15): the screen design this plan anticipated is now
> finished.** A five-round design study (options A–H, merged in #96) settled
> the final layout — **Option H**: first-person board, vertical table list,
> crown-driven fisheye hand lane, caption verbs. For building the watch UI,
> use **`docs/WATCHOS_SPEC.md`** (implementor handoff) with
> `docs/watchos-layout.html` (interactive mockups) and `docs/WATCHOS_LAYOUT.md`
> (decision record + engine-verified rules facts). The screen sections below
> (§4–§5) are **superseded** by that study; this doc remains the plan of
> record for everything structural — App-Store bundling (§1), architecture &
> connectivity (§6–§8), complications (§9), milestones & sequencing (§11:
> after the phone app's Milestone F), and gotchas (§12).

*Fills out the original design study (branch `claude/ios-redesign`, commit
`adb76a6`) into a buildable plan, incorporating the owner's real-size screen
sketches (July 2026, reproduced verbatim in §4). This doc adds: the App-Store
structural answer (§1), the sketch-driven screen revisions (§4–§5, now
superseded — see status note), architecture and connectivity (§6–§8),
complications (§9), milestones (§11), and gotchas (§12). The iOS app this
builds on is merged on main (`ios/`, FoolishKit with
Engine/Net/DesignSystem/Boards).*

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

### 3.1 Cues taken from Apple's Weather app (the reference)

Apple Weather for watchOS is the owner-designated quality bar. Five of its
moves transfer directly; each is now a rule:

1. **One hero element per screen.** Weather's city screen is an enormous
   `85°` with everything else tiny around it — hierarchy, not density, is what
   makes a watch screen readable. Our mapping: on **Table**, the center battle
   strip is the hero (largest tokens on screen; ring counts and corners stay
   small); on **Games**, the brass "your move" state; on the **fool screen**,
   the `Д`. If two elements compete for hero on any screen, the design is
   wrong.
2. **Corner data as micro-gauges, not bare text.** Weather's bottom corners
   are tiny circular dials (wind compass, 37% ring). Ours: the **deck count
   renders inside a depleting ring** (full at 24/36 cards, empties as the
   stock drains — the ring shape *is* the "how long is this game" signal at a
   glance), and the **discard count inside a filling ring**. Same numerals as
   the sketch, one ring stroke around each; no extra space cost.
3. **Floating circular toolbar chips are the top-bar idiom.** Weather's
   translucent round `≡` (its list) and condition buttons flank the clock —
   that's the watchOS-10 toolbar (`topBarLeading`/`topBarTrailing`), layered
   above content. This *is* our §5.3 answer drawn by Apple themselves: the
   leading slot is the system Back (to Games, like Weather's back-to-cities),
   and if we ever need one contextual control it's a single trailing circular
   chip — never an in-content corner.
4. **Background tint carries state.** Weather's sky-gradient background tells
   you the conditions before you read anything. Restrained version for us on
   true black: a faint bottom-up vignette — **brass when it's your move**,
   neutral otherwise, red only on the you-picked-up beat. Never behind the
   Action grid (contrast is king there), and it must survive always-on
   dimming as "off," not as a wrong color.
5. **List rows = name left, glyph + data right-aligned.** Weather's forecast
   rows (`Tue ☀️ 62° 87°`) are the grammar for the **Games list**: opponent
   left, then right-aligned state glyph + hand count (`Sveta ● HOD! 3`).
   Numerals in the same rounded face throughout — Weather's giant rounded
   numerals confirm the §3 SF Rounded call.

(Weather's tiny precipitation bar chart has no v1 mapping — a per-game
hand-size sparkline is a stats-screen idea for later, noted and parked.)

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

## 4b. More real-size frames (same style)

Same frame as §4 — 16 columns wide, bottom border on line 11, calibrated to
41mm in a default vim font. Tokens show **values only** (suit is color, which
ASCII can't show — same as your two sketches). These render the screens/states
the specs describe so the whole app is visible at once.

**Games — the root list** (tap a row to drill into its Table; `HOD!` = your
move, in brass):

```
┌────────────────┐
│ GAMES          │
│                │
│ Sveta    HOD!  │
│   hand 3       │
│ Boris    wait  │
│   hand 6       │
│                │
│ + Bot game     │
│                │
└────────────────┘
```

**Table — heads-up (2 players)**, shown with the system band above it. That
band (clock + Back `‹`) is where "back to Games" lives — above the content, so
none of the four content corners is spent on it:

```
    9:41   ‹          <- system band (OS-drawn): clock + Back. Not our content.
┌────────────────┐
│#D           #Di│    corners: deck (TL) · discard (TR)
│      _         │
│     |6|        │    lone opponent, defending (the _ ^ box = defender ring)
│      ^         │
│                │
│    A > 10      │    the one battle: ace covered by a 10
│                │
│                │
│Tr K 8 A 9 5  Pl│    bottom strip: trump · your hand peek · Pl -> Action
└────────────────┘
```

**Action — defending, nothing selected** → the kernel offers only `Pickup`:

```
┌────────────────┐
│                │
│ 6   7   9   K  │
│                │
│ 8   J   A   3  │
│                │
│                │
│   /--------\   │
│   | Pickup |   │
│   \--------/   │
└────────────────┘
```

**Action — one card selected that can either beat or forward the attack** →
the real Durak `Cover`/`Pass` fork, two half-width pills (`[9]` = selected,
lifted + ringed):

```
┌────────────────┐
│                │
│ 6   7  [9]  K  │
│                │
│ 8   J   A   3  │
│                │
│ /-----\/-----\ │
│ |Cover||Pass | │
│ \-----/\-----/ │
│                │
└────────────────┘
```

**Action — attacker, table fully covered** → the round-closing `Done` (бито):

```
┌────────────────┐
│                │
│ 6   7   9   K  │
│                │
│ 8   J   A   3  │
│                │
│                │
│   /--------\   │
│   |  Done  |   │
│   \--------/   │
└────────────────┘
```

**Game over — the fool reveal** (Rematch re-deals via the `continue` flow):

```
┌────────────────┐
│                │
│   BORIS is     │
│   the FOOL     │
│                │
│    ( Д )       │
│                │
│  /---------\   │
│  | Rematch |   │
│  \---------/   │
└────────────────┘
```

**Notification — the "your move" push** (the killer feature, §9). `Pick up` is
the one move safe to commit straight from a notification (no selection needed);
`Play` opens the Action screen:

```
┌────────────────┐
│ FOOLISH        │
│                │
│ Your move —    │
│ Boris covered  │
│ your 9         │
│                │
│  [   Play    ] │
│  [  Pick up  ] │
│                │
└────────────────┘
```

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
- **Hierarchy (the §3.1 hero rule):** the center battle strip is the hero —
  its tokens are the largest elements on screen, sized like Weather's `85°`
  relative to its gauges; ring counts, corners, and the bottom strip stay
  deliberately small. The deck and discard corner counts render as **§3.1
  micro-gauges** (deck = depleting ring, discard = filling ring) — same
  numerals as the sketch, one ring stroke each, zero extra footprint.
- **Back to Games is the system control, not a content element:** the nav
  bar's Back chevron + left-edge swipe (§5.3) sit in the system band *above*
  the content, so the sketch's four corners — and its empty top-center gap —
  all stay free for game state. No in-content back button is drawn; the
  corners being full was never a problem for "back" on watchOS.
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

### 5.3 App structure & navigation — the standard Back, which needs no corner

The original worry ("all four corners are taken, where does *back* go?") has a
simpler answer than a pager — and swipe-between-screens was the wrong reach.
watchOS sanctions exactly two navigation models
([watchOS HIG: Navigation](https://developer.apple.com/design/human-interface-guidelines/designing-for-watchos)):
**hierarchical** (tap to drill down; return via the system **Back** button +
**left-edge swipe**) and **page-based** (swipe between *peer content
categories*, with page dots). Page-based swiping is real, but it is for peers —
**it is not the "back" mechanism**, and Games→a-game is a parent→child drill,
not a peer swipe. So:

**Use hierarchical navigation and let the system Back handle the return.** The
Back chevron and the left-edge swipe live in the **navigation/status bar above
the content**, not in the four content corners — on watchOS, "back" was never a
corner's job.

```
   Games ──tap a game──▶ Table ──tap `Pl`──▶ Action
     ▲                     │                    │
     └─ left-edge swipe / Back chevron ◀─────────┘   (system, in the nav bar)
```

- **Games (root):** a `List` in a `NavigationStack` — active games (online,
  from the phone's account) + "Quick game vs bot" (offline); rows follow the
  §3.1 Weather-forecast grammar: opponent name left, right-aligned state
  glyph + hand count (`Sveta   ● HOD! 3`), "your move" in brass. Tapping a
  game **pushes** Table.
- **Table → Action:** the sketch's `Pl` button **pushes** Action. The sketch
  already draws a drill-down *button*, not a page edge — that is itself the
  tell that this flow is hierarchical, not paged.
- **Back — to Games from Table, and to Table from Action:** the **left-edge
  swipe** and the **Back chevron** the system draws at the top-left of the nav
  bar. Zero corner cost, zero custom code, and it is the exact gesture every
  Apple Watch owner already knows. Apple's own Weather app is the proof of
  this exact shape (§3.1 cue 3): its floating top-left control returns to the
  cities list while the content below keeps every pixel — same anatomy as
  Games ◂ Table.
- **Depth** is Games ▸ Table ▸ Action = **3 levels**, the HIG's stated maximum
  ("two to three"). Acceptable; do not add a fourth.
- **Settings** hangs off the Games screen (a footer row), not the game flow.

**Vertical-budget note:** watchOS always reserves the top strip for the clock,
so every screen already begins below a system band regardless of our choices;
the Back chevron rides in that same band at negligible extra cost. The §4
sketches are idealized full-bleed — in reality line 1 (`#D … #Di`) sits just
under the system bar. Keep the nav bar minimal (no large title) to preserve the
content budget.

**Considered and rejected — page-based Table↔Action:** defensible per the HIG
(they are two peer views of one game), but it puts a horizontal page-swipe on
the same screen as the left-edge back-swipe (they overlap and get finicky) and
contradicts the sketch's `Pl` button. Pure hierarchical is more reliable and
matches what you drew.

**Gesture map (no conflicts):** left-edge swipe = system Back (the *only*
horizontal gesture in the app); Crown = within-screen (Table battle-strip pan,
Action grid scroll); tap = select / commit / drill. We add **no** custom swipe
navigation.

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
10. **Navigation is hierarchical, not swipe-paged** (§5.3): back is the system
    Back chevron + left-edge swipe, which live in the nav bar above the four
    content corners — do not invent a horizontal-swipe pager or an in-content
    back button (an earlier draft did; it's wrong for watchOS). The **only**
    horizontal gesture is the system left-edge back; Crown = within-screen
    (gotcha #6); tap = drill/select/commit. Deep-link a "your move" push
    straight to **Action**; opening a game from the list lands on **Table**.
    Keep depth at 3 levels max (Games ▸ Table ▸ Action).

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
5. **Deck ring as the screen bezel?** Weather's analog dial wraps data around
   the circular geometry (§3.1). Bolder variant of cue 2: the Table's outer
   edge itself is the depleting deck ring, and the seats sit *on* it —
   unifying the seat circle and the deck gauge into one shape. Beautiful if
   it works, noisy if it doesn't; prototype on-wrist in W2 against the plain
   corner micro-gauge before committing.
6. Fool screen hero (§3.1 cue 1): the `Д` glyph, or a Weather-style giant
   numeral (games won count / new streak)? Decide when the win/loss beat is
   built in W2.
