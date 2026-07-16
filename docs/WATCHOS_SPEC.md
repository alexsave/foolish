# Foolish watchOS — implementation spec (final: Option H layout)

Audience: a SwiftUI developer building the watchOS client. This document is
self-contained: you do not need the design history (that lives in
`docs/WATCHOS_LAYOUT.md`; the interactive mockups with live demos are in
`docs/watchos-layout.html` — **section 8 (Option H, frames H1–H5 + demo) is
the layout you are building**; section 7 (Option G) is the retained
alternative, and its G9/G10 frames still define the notifications. Open the
HTML in a browser and use the "Real size" control).

All UI dimensions are **watchOS points** on the 40 mm baseline
(324×394 px = **162×197 pt**). Build layout relative so 41–49 mm gain space
proportionally; the pt numbers below are the 40 mm reference.

---

## 0 · What exists already (do not rebuild)

| Piece | Where | What you use it for |
|---|---|---|
| C rules engine as xcframework | `c/ && make ios-lib` → `ios/vendor/Foolish.xcframework` | Add watchOS slices to the Makefile target (arm64/arm64_32 watchos + simulator). The engine is the single source of truth for legality. |
| Swift bridge | `ios/sdk/swift/EngineC.swift` | `viewFromPacked(_:viewer:) → GameView` decodes a `player_views` row; `legalFromPacked(_:seat:) → [Move]` gives the **complete legal move list** for that state. These two calls are the whole rules integration. |
| Models | `ios/sdk/swift/Models.swift` | `Card`, `Suit` (`.glyph`, `.isRed`), `GameView` (`trumpSuit`, `deckCount`, `battles`, `players`, `me`…), `Move` (`type`, `cards`, `attackCards`). |
| Networking | `ios/FoolishKit/Net/` (`OnlineGame.swift`, `GameFeed.swift`, `Backend.swift`) | `player_views` Realtime subscription + action POST. The watch reuses this layer (plain Swift + Supabase). |
| Design tokens / haptics | `ios/FoolishKit/DesignSystem/` (`Tokens.swift`, `Haptics.swift`) | Extend `Haptics.swift` with a watchOS backend (`WKInterfaceDevice.current().play(_:)`) — the current implementation is UIKit-only. |

**Rules facts your code must respect** (engine-verified, citations in
`WATCHOS_LAYOUT.md` §1): single 52-card deck at 6–8 players — duplicates
impossible; after the opening attack **any non-defender may throw in at any
time** (a race; the server serializes and losers get typed rejects); **no
timer exists** — rounds close only by unanimous explicit GOOD, and any table
change resets all GOOD votes; saying GOOD locks that player until the table
changes; the **defender can never say GOOD**; transfer (pass) is legal only
before any cover and moves the defender role to the next player; there is
**no table-slot cap** (a post-pickup defender can face 10+ pairs); the
flipped trump card is public state and is always the **last card drawn**;
`good_players` (who voted) is public state, delivered in every view.

**One deliberate capability cut:** the watch plays **one card per action**.
Multi-card leads are reachable by playing twice (the first card's rank goes
live). Multi-card *pass* cannot be sequenced (after one pass you're no longer
the defender) and is intentionally **web-only**. Never build multi-select.

---

## 1 · App structure & gesture map

```
WatchApp (NavigationStack, pushed from the game list)
└─ TableScreen                ← the only screen needed during play
   ├─ ChooserOverlay          (modal: ambiguous cover / cover-pass only)
   ├─ RejectGlow              (600 ms red edge flash)
   └─ RosterScreen            (pushed by a seat-strip tap; system chevron returns)
Notifications: category FOOLISH_TURN (see §9; long look per mockup G9,
short look per G10)
Later milestones (not v1): Smart Stack widget, corner complication.
```

**One axis, one meaning — load-bearing:**

| Gesture | Meaning | Never means |
|---|---|---|
| Crown | hand lane browsing (cards + terminal item) | page/screen navigation |
| Vertical drag | table list scroll — only when > 5 pairs | hand, navigation |
| Tap | focus (lane neighbors), commit (focused card / caption), roster (seat strip), retarget (chooser) | — |
| Horizontal swipe | **nothing** — the app has no horizontal gestures | — |

There are no pagers and no page dots anywhere. The system back chevron
(top-left, ~33 pt) is the only navigation chrome and is never fought.

---

## 2 · TableScreen layout (40 mm reference)

| Zone | Frame (pt) | Content |
|---|---|---|
| System chrome | top-left chevron ~33 ø; top-right clock | Untouched. |
| **InfoLine** | trailing-aligned **directly under the clock** (top edge ≈ 4 pt below the clock's baseline, y ≈ 32–46), trailing inset 13 | `[flip glyph] ▤{deck} ▨{discard}` — SF semibold 12 pt, secondary gray `#B8B8BE`. Flip glyph = 12 pt mini glyph-card of `GameView.flippedCard` while it exists; after it's drawn, a bare trump-suit glyph. No capacity number — it's derivable (defender's shield count minus visible open attacks). No blank band between header and strip. |
| **SeatStrip** | y ≈ 50–68, centered HStack, gap 4.5. Whole strip taps → push RosterScreen. | 8 items, seat order from your left neighbor, **you included**. Item = hand count, SF heavy 12.5 pt. Defender = count inside the gold **shield** (§6). You = gold `#E7B84A` (defending you = shield + gold). Said-GOOD = green `#30D158`. Others gray `#98989E`. Eliminated seats 30 % opacity. |
| **TableList** | left column: x 8–100, y ≈ 72–190 | One pair per row (row 22 pt, gap 3.5): `[cover glyph] ▸ [attack glyph]` — cover **left**, small arrow pointing at the covered card; open attack = attack glyph alone, full brightness, no ring/placeholder; resolved rows desaturated toward gray (`Color.secondary` luminance — never pure opacity on red). Glyphs 19 pt (rank 9 pt). **Vertically centered while ≤ 5 rows; past 5 it top-aligns and scrolls** under a vertical drag with 9 pt edge fades; newest row lands at the bottom and auto-scrolls into view only while the user is idle. Optimistic in-flight card renders at 45 % opacity in the attack column. |
| **FisheyeLane** | right column: x ≈ 104–158, anchored to the **bottom-right corner** | The hand, one representation. Focused item large (glyph card 42 pt, rank 19 pt) centered ≈ y 150; up to **three** small neighbors (17 pt, rank 8 pt) stacked above at y ≈ 76 / 98 / 120, opacity falling with distance; items beyond clip. Crown slides items through the focus point — the small chip *becomes* the big card (animate ~120 ms; respect Reduce Motion). Terminal item: bare green ✓ (attacker) or red `+n` (defender), no background, magnifies like any card. Illegal cards render dim but are browseable. |
| **Caption** | centered under the focus card, y ≈ 174–186, width ≈ 58 | SF heavy **9 pt, ALL CAPS, gray `#98989E`**, letter-spacing ~0.5. Always exactly one of: `ATTACK · GOOD · PICKUP · COVER · PASS · COVER/PASS` — or blank when the focused item offers nothing. After a GOOD vote: `VOTED`. This is the entire action UI; there are no pills or buttons. |

Colors: glyphs — black suits `#F2F2F4`, red suits `#E8352E`; rank text on
black-suit glyphs `#111`, on red-suit glyphs `#FFF`. Background pure black.
The **glyph card** is the only card rendering: ZStack of the suit glyph with
the rank string overlaid centered at ~54 % height, heavy weight.

## 3 · RosterScreen (pushed)

Rows (SF semibold 12.5 pt): name · hand count · inline gold **shield** if
defender (no role words, no GOOD column — votes live only in the strip's
green counts). Your row gold, labeled `you`. Eliminated players dimmed at the
bottom. Footer (11 pt secondary): `deck {n} · disc {n} · flip: {card} under
deck` (or `trump ♠` once drawn) and `out: {names or —}`. Header: `round {n}`.
Back = system chevron.

## 4 · Crown & focus model

- Items = `hand (server order) + [terminal]`. Attacker/bystander terminal =
  **✓ GOOD**; defender terminal = **PICKUP +n** (n = all table cards).
- `.digitalCrownRotation` snapped to indices, clamped (no wrap), `.click`
  detent haptic per step. The crown traverses **every** card — the caption
  communicates action availability, not the crown.
- Tap a small neighbor = step focus toward it. Tap the focused card (or its
  caption) = commit (§5).
- On activation / wrist raise: auto-focus the first legal card, else the
  terminal. **Focus is never stolen while the user is interacting**; if the
  focused card leaves the hand, fall back to the first legal card.
- Legality per card from `legalFromPacked(row, seat)` → per-snapshot lookups:
  `attackable: Set<Card>`, `covers: [Card:[attack]]`, `passable: Set<Card>`,
  `canPickup: Bool`, `goodEligible: Bool`.

## 5 · Caption + commit decision table

| Role | Focused item | Condition | Caption | Tap commits |
|---|---|---|---|---|
| any | card | `attackable` | `ATTACK` | optimistic throw (§7) |
| defender | card | covers exactly 1, not passable | `COVER` | cover that target immediately |
| defender | card | covers ≥ 2 | `COVER` | open ChooserOverlay |
| defender | card | covers ≥ 1 and passable | `COVER/PASS` | open ChooserOverlay |
| defender | card | passable only | `PASS` | pass immediately |
| any | card | nothing | *(blank)* | ignored |
| attacker | terminal ✓ | `goodEligible` | `GOOD` | vote; caption → `VOTED`, lane locks until the table changes |
| defender | terminal +n | table non-empty | `PICKUP` | pickup immediately |

**ChooserOverlay**: full-screen scrim (88 % black); ✕ in a 33 pt circle
top-left (same spot as the system chevron — one motor habit). Title 12.5 pt:
`{card} covers which?` or `{card} — cover or pass?`. Cover targets as
49×54 pt glyph buttons in a row; if passable, a blue `PASS ▸ {receiver}` row
below. Only ✕ dismisses; outside taps do nothing. If the world changes while
open, refresh options in place; if none remain, dismiss with the reject glow.

## 6 · Shield

The defender mark everywhere: heater-shield outline, gold `#E7B84A` stroke
~1.25 pt, transparent-dark fill, count centered. Reference path (40×48):
`M20 1 L38 8 L38 26 Q38 38 20 47 Q2 38 2 26 L2 8 Z`.

## 7 · Optimistic commit & rejection

1. Render the result immediately: thrown/passed card appears in the table
   list at 45 % opacity; covers slide onto their row; GOOD flips the caption.
2. POST the move (same endpoint/`Move` encoding as `OnlineGame`); the next
   `player_views` push is authoritative.
3. On reject (`ENGINE_REJECT_*` / `STALE_ROUND`): remove the optimistic
   card, flash the **RejectGlow** — inset red edge glow (`#FF453A`, ~9 pt
   blur, 600 ms ease-out) — and play the failure haptic. **No text, no
   banner, no dialog, ever.** The board already shows why.

## 8 · Haptic vocabulary (strict)

Derived by diffing consecutive `GameView`s:

| Event | WKHapticType | Notes |
|---|---|---|
| A rank you hold went live & capacity > 0 | `.click` | Pre-focus that card — **only from background/wrist-down**, never mid-browse. Max 1 per table change. |
| You became defender / new attack on you | `.notification` | |
| Your move confirmed | `.success` | |
| Your move rejected | `.failure` | Paired with RejectGlow. |
| You picked up / game over | `.stop` | |
| Everything else | silence | Invariant: **a buzz means you can act right now.** |

## 9 · Notifications (category `FOOLISH_TURN`)

Fire only on decision moments: you became defender · a rank you hold went
live · all-covered-awaiting-your-GOOD. Long look (mockup G9): icon row
(`♠ FOOLISH`), five-word title (**"Kat passed to you"**), one fact line
(`2 attacks · you hold 5`), actions:

- **Open Hand** — foreground; deep-link to TableScreen, best legal card
  pre-focused.
- **Take {n}** — background action, defender only (always legal; server
  validates anyway).
- Dismiss.

Short look (mockup G10) is system-rendered — icon + app name + title.

## 10 · Non-play states

- **Waiting/lobby**: "Waiting for players — manage on your phone."
- **You're out (escaped)**: TableScreen stays live (spectate); lane and
  caption hidden; strip + table keep updating.
- **Game over**: small `ДУРАК`, loser name large red, escape order one line,
  CLOSE.
- **Connection lost**: gray dot in the InfoLine; keep last snapshot; captions
  blank (no commits).

## 11 · Acceptance checklist (match mockups H1–H5 + G9/G10)

1. H1 anatomy at 40 mm; only system chrome in the top-left corner; InfoLine
   directly under the clock with no blank band; counts sum to 52 with
   hands + table.
2. Flip **card** glyph pre-draw, bare suit post-draw.
3. Table list: cover-left arrow rows; open = bright bare glyph; resolved =
   desaturated; **centered ≤ 5 rows, scrolls after** with edge fades; a
   10-pair table works; in-flight cards translucent.
4. Fisheye: focused card magnifies **in place** (single representation — no
   separate chip strip anywhere); three neighbors above; terminal ✓ / +n
   obeys the same physics; crown clamps at ends with detent haptics.
5. Caption matrix (§5) exact — gray ALL CAPS, blank states included; commit
   only via focused card / caption tap.
6. Unambiguous cover = one tap; `COVER` multi-target and `COVER/PASS` open
   the chooser; chooser survives mid-flight world changes.
7. Rejection = red edge glow + `.failure`, zero text; optimistic card
   returns.
8. Gesture map holds: no horizontal gestures anywhere; vertical drag only
   scrolls the table list; seat-strip tap pushes Roster; chevron returns.
9. GOOD: attacker-only terminal; `VOTED` lock until the table changes;
   defender terminal is PICKUP. Roster has no GOOD column; strip greens are
   the tally.
10. Haptics only per §8; notification actions work from a locked watch;
    Open Hand lands with the correct card focused.
11. Look-zones (table glyphs 19 pt, lane neighbors 17 pt, caption 9 pt) are
    deliberate floor exceptions — never tap-required; everything actionable
    is the 42 pt focused card or full-width rows.

## 12 · Known open items (not v1 blockers)

- Push latency budget for the snipe loop — measure APNs → wrist; if slow,
  prefer complication/background refresh for the "rank live" signal.
- Smart Stack widget + corner complication (glance layer).
- `make ios-lib` watchOS slices (build-system task; the C code already
  builds for iOS arm64 and wasm).

## Appendix · Option G (retained alternative)

Mockups §7. Same decision logic rendered with a horizontal table pager,
chip strip + separate focus slot, and labeled pills. Superseded in the final
owner review by H's single hand representation, all-vertical gesture map,
and caption-sized verbs — but G's frames remain the reference for the
notification layouts (G9/G10) and document the trade-offs.
