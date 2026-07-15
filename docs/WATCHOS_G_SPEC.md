# Foolish watchOS — Option G implementation spec

Audience: a SwiftUI developer building the watchOS client. This document is
self-contained: you do not need the design history (that lives in
`docs/WATCHOS_LAYOUT.md`; the interactive mockups with live demos are
`docs/watchos-layout.html`, section 7 — open it in a browser and use the
"Real size" control; every screen state referenced below as G1…G10 is
mocked there).

All UI dimensions in this spec are **watchOS points** on the 40 mm baseline
(324×394 px = **162×197 pt**). Build layout relative (percent/flex) so 41–49
mm gain space proportionally; the pt numbers below are the 40 mm reference.

---

## 0 · What exists already (do not rebuild)

| Piece | Where | What you use it for |
|---|---|---|
| C rules engine as xcframework | `cnitro/ && make ios-lib` → `ios/vendor/Foolish.xcframework` | Add watchOS slices to the Makefile target (arm64/arm64_32 watchos + simulator). The engine is the single source of truth for legality. |
| Swift bridge | `ios/FoolishKit/Engine/EngineC.swift` | `viewFromPacked(_:viewer:) → GameView` decodes a `player_views` row; `legalFromPacked(_:seat:) → [Move]` gives the **complete legal move list** for that state. These two calls are the whole rules integration. |
| Models | `ios/FoolishKit/Engine/Models.swift` | `Card`, `Suit` (`.glyph`, `.isRed`), `GameView` (`trumpSuit`, `deckCount`, `battles`, `players`, `me`…), `Move` (`type`, `cards`, `attackCards`). |
| Networking | `ios/FoolishKit/Net/` (`OnlineGame.swift`, `GameFeed.swift`, `Backend.swift`) | `player_views` Realtime subscription + action POST. The watch reuses this layer (it is plain Swift + Supabase). |
| Design tokens / haptics | `ios/FoolishKit/DesignSystem/` (`Tokens.swift`, `Haptics.swift`) | Extend `Haptics.swift` with a watchOS backend (`WKInterfaceDevice.current().play(_:)`) — the current implementation is UIKit-only. |

**Rules facts your code must respect** (engine-verified, citations in
`WATCHOS_LAYOUT.md` §1): single 52-card deck at 6–8 players — duplicates
impossible; after the opening attack **any non-defender may throw in at any
time** (it is a race; the server serializes and losers get typed rejects);
**no timer exists** — rounds close only by unanimous explicit GOOD, and any
table change resets all GOOD votes; saying GOOD locks that player until the
table changes; the **defender can never say GOOD**; transfer (pass) is legal
only before any cover and moves the defender role to the next player;
there is **no table-slot cap** (a post-pickup defender can face 10+ pairs);
the flipped trump card is public state and is always the **last card drawn**;
`good_players` (who voted) is public state, delivered in every view.

**One deliberate capability cut:** the watch plays **one card per action**.
Multi-card leads are reachable by playing twice (the first card's rank goes
live). Multi-card *pass* cannot be sequenced (after one pass you're no longer
the defender) and is intentionally **web-only**. Never build multi-select.

---

## 1 · App structure

```
WatchApp
└─ GameRootView            — VERTICAL TabView (.verticalPage), 2 pages
   ├─ TableScreen  (page 1)   ← the only screen needed during play
   │    ├─ ChooserOverlay     (modal, ambiguous cover/pass only)
   │    └─ RejectGlow         (600 ms red edge flash)
   └─ RosterScreen (page 2, below — swipe up; system vertical dots, right edge)
Notifications: category FOOLISH_TURN (long look per G9, actions below)
Later milestones (not v1): Smart Stack widget, corner complication.
```

**The gesture map is one-axis-one-meaning — this is load-bearing:**

| Gesture | Meaning | Never means |
|---|---|---|
| Crown | hand browsing (cards + terminal item) | page navigation |
| Horizontal swipe | table pair pages — **only** | hand, roster |
| Vertical swipe | Table ↔ Roster (root vertical TabView) | table pairs |
| Tap | focus (chips, seat strip) or commit (FocusSlot, Pill) | — |

Consequences: the root TabView is **vertical** (`.verticalPage` /
`.tabViewStyle(.verticalPage)`), so the table pager is the only horizontal
pager — no nested same-axis conflict. The **chip strip must not be
horizontally draggable**: it is a crown-bound viewport (offset driven by the
focus index), chips accept taps only. Tapping the **seat strip** also opens
the Roster (redundant, discoverable path). There are **no bottom page
dots** — the system's vertical-page indicator (right edge) replaces them.

Navigation: the app is pushed from a game list; the **system back chevron
exists and is not fought** — the top-left ~33 pt circle belongs to it. All
layout below assumes it is present.

---

## 2 · TableScreen layout (40 mm reference, top → bottom)

| Zone | Frame (pt) | Content |
|---|---|---|
| System chrome | top-left chevron ~33 ø; top-right clock | Untouched. |
| **InfoLine** | trailing-aligned **directly under the clock**: top edge ≈ 4 pt below the clock's baseline (y ≈ 32–46), trailing inset 13 — same right edge as the clock | `[flip glyph] ▤{deck} ▨{discard}` — SF semibold 12 pt, secondary gray `#B8B8BE`. Flip glyph = 12 pt mini glyph-card of `GameView.flippedCard` while it exists; after it's drawn, render the bare trump suit glyph instead. **This is not its own centered row** — it shares the header band with the clock; there must be no blank band between the header and the strip. |
| **SeatStrip** | y ≈ 50–68, centered HStack, gap 4.5. Whole strip is a tap target → opens Roster. | 8 items, seat order starting from your left neighbor, **you included**. Item = hand count, SF heavy 12.5 pt. Defender = count inside the gold **shield** (22×26.5 outline shape, see §6). You = gold `#E7B84A` (if you defend: shield + gold count). Said-GOOD = green `#30D158`. Others gray `#98989E`. Eliminated seats render dimmed at 30 % opacity. |
| **TablePager** | y ≈ 72–132, horizontal `TabView(.page)` — the app's **only** horizontal pager; index dots ~4 pt at y ≈ 134 | **Two pairs per page**, centered, gap 15. Pair = `[cover glyph] ▸ [attack glyph]` — **cover on the left**, 4 pt arrow `▸` in `#5A5A5E` pointing at the covered card. Open attack = the attack glyph **alone**, full opacity — no ring, no placeholder slot. Resolved pair: dim by **desaturating toward gray** (`#6E6E73`-ish), not by pure opacity — a red glyph at 40 % opacity on black is illegible; target roughly `Color.secondary` luminance for both suits. Optimistic in-flight card = 45 % opacity (it is transient, legibility matters less). Glyph size 31 pt (rank 14.5 pt). Dots appear only when pages > 1. Auto-advance to the page containing the newest open attack **only when the user is not interacting** (see focus invariant, §4). |
| **FocusSlot** | leading 8, y ≈ 132–194 | The crown-focused item, large: glyph card 56×62 (rank 23 pt); dim (32 % opacity + desaturate) when the item has no action. Terminal items render as a bare green ✓ (48 pt, **no background**) or red `+n` (30 pt) — see §4. |
| **Pill** | trailing 7, y ≈ 146–174, height 28, **min-width 86**, corner radius 14, horizontal padding 12 | SF heavy 13 pt. The single action control; see the decision table (§5). When the focused item offers nothing, the pill is **absent** (not disabled). |
| **ChipStrip** | x 71 → trailing 4, y ≈ 170–195 | All hand cards as 22×25 glyph chips (rank 10 pt), gap 4.5, in server hand order, followed by the **terminal item** (§4). Focused item: 1.25 pt white outline, 1 pt offset. Illegal-now cards: 32 % opacity. When the strip overflows, it becomes a crown-followed viewport with a 15 pt fade mask on the **right edge only**; keep the focused chip visible. Chips are **display + focus targets only**: tapping a chip focuses it, never commits. |
| *(no bottom dots)* | — | Table↔Roster paging is vertical; the system draws its vertical indicator on the right edge. |

Colors: card face glyphs — black suits `#F2F2F4`, red suits `#E8352E`; rank
text on black-suit glyph `#111`, on red-suit glyph `#FFF`. Pill variants —
ATTACK/COVER gold `#E7B84A` on `#241A02`; TAKE red `#FF453A` on `#2B0300`;
PASS blue `#0A84FF` on `#001B38`; GOOD green `#30D158` on `#03210E`; voted/
disabled gray `#2C2C2E` on `#98989E`. Background pure black.

The **glyph card** component (see `FCard.swift` for the iOS analog): a
ZStack of the suit glyph (SF text, sized per zone above) with the rank
string overlaid centered at ~54 % height, heavy weight. This is the only
card rendering in the app — there are no card faces.

---

## 3 · RosterScreen (page 2)

Rows (SF semibold 12.5 pt, 16.5 pt row height): player name · hand count ·
shield icon inline if defender (no role words, no GOOD column). Your row in
gold, labeled `you`. Eliminated players struck through / dimmed at the list
bottom. Footer (11 pt secondary): `deck {n} · disc {n} · flip: {card} under
deck` (or `trump ♠` once drawn) and `out: {names or —}`. Header left:
`round {n}` (12 pt); clock top-right as always.

---

## 4 · Crown & focus model

- `ScrollableFocusModel`: an ordered list of **items** =
  `hand.map(CardItem) + [TerminalItem]`.
  - Attacker / bystander: terminal = **✓ GOOD** (bare green check).
  - Defender: terminal = **TAKE n** (red `+n`, where n = all cards currently
    on the table).
- Drive with `.digitalCrownRotation` bound to a continuous value snapped to
  item indices; **clamp at the ends (no wrap)**; play `.click` haptic per
  detent. The crown traverses **every** card, including illegal ones — the
  pill communicates action availability, not the crown.
- Tap on a chip = focus that item (never commits). Tap on the FocusSlot or
  the Pill = commit (§5).
- On app activation / wrist raise: auto-focus the **first legal** card if
  any, else the terminal item. **Focus is never stolen while the user is
  interacting** — remote updates re-render counts/table but keep the focused
  identity; if the focused card left the hand (picked up/played), fall back
  to the first legal card.
- On GOOD-voted lock (see §5): all card items render dim; terminal shows
  gray `✓`; crown still browses (planning is allowed, acting is not).

Legality per card comes from `legalFromPacked(row, seat)` — build three
lookups per snapshot:
`attackable: Set<Card>`, `covers: [Card: [attackCard]]`,
`passable: Set<Card>` (plus `canTake: Bool`, `goodEligible: Bool` — good is
in the move list only when the engine allows it: not defender, table
non-empty, not already voted, opener-must-attack satisfied).

## 5 · The pill + tap decision table

For the focused item, exactly one of:

| Role | Focused item | Condition | Pill | Tap (pill or FocusSlot) |
|---|---|---|---|---|
| any | card | `attackable` | **ATTACK** (gold) | optimistic throw (§7) |
| defender | card | covers exactly 1 target, not passable | **COVER** (gold) | cover that target immediately |
| defender | card | covers ≥ 2 targets, or (covers ≥ 1 **and** passable) | **COVER** (gold) | open **ChooserOverlay** |
| defender | card | passable only (no cover) | **PASS ▸ {receiver}** (blue) | pass immediately |
| any | card | none of the above | *(no pill)* | nothing (ignore tap) |
| attacker | terminal ✓ | `goodEligible` | **GOOD** (green) | vote; then pill = gray `✓ voted`, lock until table changes |
| attacker | terminal ✓ | already voted | gray **✓ voted** | nothing |
| defender | terminal +n | table non-empty | **TAKE n** (red) | pickup immediately |

**ChooserOverlay** (G2b): full-screen scrim (88 % black). Top-left ✕ in a
33 pt circle (same position as the system chevron — one motor habit). Title
(12.5 pt): `{card} covers which?` or, when pass is also legal,
`{card} — cover or pass?`. Cover targets as 49×54 pt glyph buttons in a
row; if passable, a blue pill row `PASS ▸ {receiver}` below. Only ✕
dismisses; outside taps do nothing (race-safe). If the world changes while
open (a target gets covered / pass dies), refresh the options in place; if
none remain, dismiss with the reject glow.

Receiver for PASS = next in-play player clockwise from you; show their
name; their hand count is why pass can be illegal — the engine's move list
already accounts for it.

## 6 · Shield

The defender mark everywhere (SeatStrip, Roster): a heater-shield outline,
gold `#E7B84A` stroke ~1.25 pt, transparent-dark fill, count centered.
Reference path (40×48 viewBox):
`M20 1 L38 8 L38 26 Q38 38 20 47 Q2 38 2 26 L2 8 Z`.

## 7 · Optimistic commit & rejection

On ATTACK / COVER / PASS / TAKE / GOOD:

1. Render the optimistic result immediately: thrown card appears on the
   table pager at 45 % opacity (new open pair); cover slides onto its
   target; GOOD greys the pill; TAKE can simply await the snapshot.
2. POST the move (same endpoint/`Move` encoding as `OnlineGame`); the next
   `player_views` push is authoritative and replaces optimistic state.
3. On reject (`ENGINE_REJECT_*` or `STALE_ROUND`): remove the optimistic
   card, flash the **RejectGlow** — an inset red border glow around the
   screen edge (inset shadow, `#FF453A`, ~9 pt blur, 600 ms ease-out) —
   and play the failure haptic. **No text, no banner, no dialog.** The
   board already shows why (capacity gone, round closed).

Never show a confirmation dialog anywhere in the app.

## 8 · Haptic vocabulary (strict)

Extend `Haptics.swift` with a watch backend. Signals are derived by
diffing consecutive `GameView`s:

| Event (derived) | WKHapticType | Notes |
|---|---|---|
| A rank you hold went live & capacity > 0 | `.click` | Also pre-focus that card — **only if the app is backgrounded/wrist-down**, never mid-browse. Throttle: max 1 per table-change. |
| You became defender / new attack landed on you | `.notification` | |
| Your move confirmed | `.success` | |
| Your move rejected | `.failure` | Paired with RejectGlow. |
| You picked up / game over | `.stop` | |
| Everything else | silence | Invariant: **a buzz means you can act right now.** |

## 9 · Notifications (category `FOOLISH_TURN`)

Fire only on decision moments: you became defender · a rank you hold went
live · all-covered-awaiting-your-GOOD. Long look (G9): app icon row
(`♠ FOOLISH`), title = five words max (e.g. **"Kat passed to you"**), one
fact line (`2 attacks · you hold 5`), then actions:

- **Open Hand** — foreground, deep-links to TableScreen with best-legal
  card focused.
- **Take {n}** — background action, defender only (always legal for the
  defender; server validates anyway).
- Dismiss.

Short look is system-rendered (icon + app name + title) — G10 shows the
expected appearance; no custom code needed beyond a good title.

## 10 · Screens for non-play states

- **Waiting/lobby game**: single message screen — "Waiting for players —
  manage on your phone." No lobby management on the watch.
- **You're out (escaped)**: TableScreen stays live (spectate); FocusSlot
  and Pill hidden, chips gone; SeatStrip + pager keep updating.
- **Game over**: full-screen: small `ДУРАК` label, loser name large red,
  escape order one line, CLOSE.
- **Connection lost**: gray dot in the InfoLine; keep last snapshot; block
  commits (no pill).

## 11 · Acceptance checklist

Match against the mockups (`watchos-layout.html` §7, G1–G10 + live demo):

1. G1 anatomy at 40 mm; nothing but system chrome in the top-left corner.
2. InfoLine shows flip **card** pre-draw, bare suit post-draw; deck and
   discard counts always correct (they must sum with hands+table to 52).
3. Crown traverses all cards + terminal; detent haptics; no wrap; chip tap
   focuses only.
4. Pill matrix (§5) exact — including *absent* pill states; pill min-width
   86 pt so ATTACK/COVER/TAKE 12 render at one size.
5. Unambiguous cover commits in one tap; ambiguous (multi-target **or**
   cover+pass) opens the chooser; chooser handles mid-flight world changes.
6. Two pairs per table page, cover-left arrow rendering, resolved dimmed,
   in-flight translucent; dots only when > 1 page; a 10-pair table works.
7. Chip strip crown-scrolls past ~7 items with right-edge fade only.
8. Rejection = red glow + `.failure`, zero text; optimistic card returns.
8b. Gesture map holds: horizontal swipe pages table pairs only; vertical
    swipe (or seat-strip tap) reaches the Roster; the chip strip cannot be
    dragged; nothing else responds to horizontal drags.
9. GOOD: attacker-only terminal ✓ (bare, no background); vote → gray lock →
   auto-unlock on table change. Defender terminal is TAKE n.
10. Roster per §3 (shield icon, no GOOD column, flip line).
11. Haptics only per §8 table; notification actions work from a locked
    watch; "Open Hand" lands with the correct card focused.
12. Every actionable string ≥ 12 pt; chip/pair ranks are the only smaller
    text (look-zones, re-readable in the FocusSlot).

## 12 · Known open items (not v1 blockers)

- Push latency budget for the snipe loop — measure APNs → wrist; if slow,
  prefer complication/background refresh for the "rank live" signal.
- Smart Stack widget + corner complication (glance layer).
- `make ios-lib` watchOS slices (build-system task; the C code is portable —
  it already builds for iOS arm64 and wasm).
