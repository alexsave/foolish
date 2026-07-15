# Foolish on the wrist — 40 mm watchOS layout study

Deliverables:

> **Status: Option H ships.** It is built in `ios/WatchFoolish` — start at
> §4.6 and §4.6.1 (as-built), which override the G-era text below wherever they
> disagree. The rest of this doc (rules facts, HIG budget, reviews) still
> stands, and G remains the source for everything H inherits unchanged.

Deliverables:

- **`docs/watchos-layout.html`** — the mockups: every screen state as HTML/CSS/JS
  at native watch pixels, viewable at true physical size (with a
  monitor-calibration control), a 10 mm-finger overlay, a 2×3 tap-zone
  overlay, and live crown/tap demos. Eight design options (A–H) with
  per-element rationale. Useful for the states, but **it is no longer canonical
  for H** — §4.6.1 lists where the built screen departs from these frames.
- **`docs/WATCHOS_G_SPEC.md`** — the Option G handoff. Superseded in part: H
  replaced its root pager, table pager, focus slot, chip strip and pill. Still
  the reference for the rules facts, action decision table, haptics,
  notifications and data layer.
- **This doc** — the verified rules (with citations), the Apple HIG
  constraints (with quotes), the design reviews and their engine verdicts,
  and ASCII reference frames.

How this converged, in four rounds:

1. **Option B** ("Focus flow") — first 40 mm attempt; killed by the HIG type
   audit (half its text below Apple's 12 pt floor).
2. **Option D** ("One thing at a time") — HIG-purist: full-bleed crown pages,
   one tap zone, everything ≥ 24 px. Correct but board-blind.
3. **Foolish40 prototype + second design review → Option F** ("First person")
   — the synthesis chassis.
4. **Owner review of F → Option G** ("First person, refined") — **the build
   spec.** A/B/C were confirmed illegible at real size; D's full-screen card
   and notifications survive inside G.

---

## 1 · Rules facts the layout depends on

Everything below was read out of the engine — the C kernel is the single
source of truth (the TS server and the browser both run it compiled to wasm).

| # | Question | Answer from the code | Citations |
|---|----------|----------------------|-----------|
| 1 | Deck composition | 2–5 players: 36 cards (ranks 6–A). **6–8 players: full 52** (ranks 2–A). One of each suit/rank; hands deal & refill to 6. An 8-seat game deals 48 and flips one — the stock is **3 + the flip** and dies in about a round. | `cnitro/src/card.h:15-29`, `constants.ts:33-39`, `card.h:25` |
| 2 | Duplicate cards in one hand | **Impossible.** Deck generated one-of-each; duplicate-carrying moves rejected; the rearrange endpoint validates a permutation to prevent duplicate-minting. Masked opponent cards are wire placeholders — rendered by count, never identity. | `game.c:305-316`, `game.c:524-528,545`, `rearrange.ts:5-11,23-32`, `view.c:61-64`, `clientGuards.ts:15-20` |
| 3 | Max table slots / throw-in cap | **No fixed slot count.** The only cap: defender's current hand ≥ `uncovered + n`. Buffers are build params (32 native / 64 wasm+iOS); ≤ 28 cards per submit. Covering is capacity-neutral, so a fresh-hand bout tops out at 6 pairs — but a **post-pickup defender can legally face dozens**. | `game.c:557-559`, `game.h:12-19`, `Makefile:127,372`, `awire.h:26-27` |
| 4 | Who may throw in, when | After the opening attack: **any non-defender, any time** — no adjacency, no turn order. Thrown values must be on the table, and **defense cards count** (a cover opens its value). Opening attack: first-attacker only, single value, 1–n cards. | `game.c:538,551-555`, `game.c:507-513`, `game.c:547-550` |
| 5 | How a round closes | Three ways, **no timer** (the 60 s auto-discard is disabled): ① all covered **and** every in-play attacker explicitly says GOOD — any attack/cover/pass **resets all GOODs**; ② defender's covering play empties their hand → instant close; ③ pickup → round over, defender skipped. The opener can't GOOD before attacking. Saying GOOD **locks you** until the table changes. | `good.ts:7-8`, `game.c:840-869`, `game.c:572,696-697,747-748`, `game.c:657-694`, `game.c:799-802`, `game.c:845`, `legal.c:377-384` |
| 6 | Transfer (perevod) | **Legal.** Defender only; before any cover; table attacks share the passed value; receiver = next in-play player with `hand ≥ passed + battles`; defense moves on. Chains legal. No show-trump variant. | `game.c:716-772` (esp. `:731,:732,:734-735,:759`) |
| 7 | Legal-moves function | Kernel enumerator exists (bots/server, capped 65,536). The **client ships a 23 KB validate-only kernel** (guards.wasm): sync `canAttack/canCover/canPass/canPickup/canCoverPair` + `validateActionWire`. Per-card legality shading is cheap — this is what lets the crown be a legality oracle. | `legal.c:358-386`, `bot_strategy.ts:143-151`, `clientGuards.ts:1-27,219-285` |
| 8 | Events or snapshots | **Both.** Per action: evwire per-viewer masked event stream + final-state trailer. Standing: per-player pre-masked snapshots (`player_views`) via Realtime. A watch can render snapshots only. | `evwire.h:1-27`, `player_views.ts:1-13`, `game.h:166-190` |
| 9 | Nitro's observation encoding | Bots run in-kernel: full state + per-seat strategy keys + game key + session log (hidden cards `0xFE`) + secret-derived RNG. Belief bots reconstruct hidden cards from the public log; distilled policy = 55-feature vector/move. Bitmask belief tracking is correct (single deck — see review verdicts below). | `bots.ts:126-162,244-289`, `strategy.h:41`, `distill_feat.h:15` |
| 10 | Flipped trump = last draw | **True** — when the deck empties, the flipped card is drawn last. Real endgame info; lives on the roster page. | `game.c:321-333` |

## 2 · Physical budget (40 mm) & Apple's rules

- Panel **324 × 394 px** @ 326 ppi → 25.3 × 30.7 mm, corner r ≈ 60 px; a
  162 × 197 pt canvas. A 10 mm fingertip = **128 px = 40 % of panel width**.
- HIG (fetched, quoted in the HTML §3): interactions "less than a minute";
  related experiences (notifications, complications) used "**more than the app
  itself**"; "single-screen interactions … a simple gesture or two"; crown =
  "**the primary input for navigation**" (watchOS 10, vertically paginated
  tabs), always backed by touch; text **default 16 pt / minimum 12 pt**
  (= 32 / 24 px here); "the bezel provides natural visual padding" — design
  full-bleed; crown presses are system-reserved.

## 3 · The two design reviews, held against the engine

**Review 1 — the Foolish40 prototype (screenshots).** Verdicts: the
**rank-inside-suit glyph** (card = one shape) and the **shield/sword role
language** are keepers; the radial ring proved names-as-ambient works, but it
paginated the table and spent center pixels on 7 small numbers. Missing,
per the rules: any GOOD affordance (the only normal round-close, fact #5),
TAKE/PASS reachability, and the capacity number.

**Review 2 — the "first person" critique.** Its design moves are adopted
wholesale: you are not on the board (the watch is a first-person view);
opponents = a top strip of counts (identity one page away); the **full table
grid replaces pagination** (open attacks are the one thing you must see
whole); **small things you look at, big things you touch** (chips read-only,
crown browses, focus card is the one big tap target); crown detents =
**[legal cards…, terminal action]**; the **snipe loop** with a strict haptic
vocabulary and wrist-down-only pre-focus; optimistic in-flight rendering,
no confirmation dialogs. Its four factual assumptions, checked:

| Review claim | Engine verdict |
|---|---|
| "Neighbors only, or anyone?" | **Anyone, any time** (`game.c:538`) — the race/vulture reframe is correct; no waiting-on indicator exists to draw. |
| Round end via `bito 0:04` last-call countdown? | **No timer exists** (`good.ts:7-8`). Rounds close by unanimous explicit ✓ (`game.c:852`) — the countdown is replaced by the goods tally (strip counts turn green as votes land). |
| "Foolish is a double 36-deck, right? 42+8+14=64" | **No — single 52** (`card.h:15-29`); duplicates impossible (`game.c:305-316`). The deduction was sound; the premise was my earlier mockups' invented counts, which violated 52-card conservation. Fixed: every mock now sums to 52. No ×2 chips, no twin auto-focus; nitro's bitmask belief tracker stays. |
| Fixed six-slot grid | **No slot cap** (`game.c:557-559`) — the 2×3 grid gets a "+N more" overflow cell for post-pickup defenders. |

## 4 · Option F — "First person" (the chassis, superseded by G)

Two screens total. **Table** (the only screen needed in play): status line
(trump + deck), opponents' counts across the top (gold shield = defender,
green = said GOOD), the full 2×3 pair grid (resolved dimmed, open red-ringed,
empty dashed, "+N" overflow), the focus card (~96 px) with a two-line label
column (verb, then `cap · disc` or the last haptic event), the read-only chip
strip with the ✓ vote as its terminal chip, and page dots. **Roster** (one
page right): 25 px rows — name, count, role, vote — plus deck/disc/trump,
"last draw" (fact #10), and who's out.

Interaction grammar:

- **Crown detents = [legal cards…, terminal]** — legality from guards.wasm
  collapses six cards to two or three detents. Terminal = ✓ (attacker) or
  TAKE with a live price (defender; "+2 → +5" climbing is the pressure UI).
- **Tap the focus card to commit.** Optimistic: the card renders on the grid
  translucent, solidifies on confirm, or slides home with an ✗ tick if
  sniped. Never a confirmation dialog.
- **Defender:** open pairs ring red; single open pair auto-targets; several →
  tap a pair to retarget (mis-tap changes selection, never commits). Crown
  cycles legal beats; the ⇄ pass line appears exactly when the focused card
  both beats and matches the table value (receiver named with hand count).
  New pairs queue in arrival order and never steal focus.
- **Haptics (the app is mostly closed — HIG):** ⌁ a rank you hold went live &
  a slot is open (pre-focuses that card, wrist-down only) · ⌁⌁ you became
  defender / new attack on you · ✓/✗ your throw confirmed/bounced · long =
  you took the pile or game over. Everything else silent — a buzz always
  means you can act right now.
- **Stated floor exceptions:** grid ranks (~17 px) and chip ranks are
  look-zones — never touch-zones, always re-readable in the focus card.
  Every actionable string ≥ 24 px.

ASCII reference (grid: 40 × 25 chars over 324 × 394 px = 8.1 × 15.76 px/char;
corner radius 60 px drawn to scale; `s h d c` suits, `[v]` = GOOD vote chip,
`[_]` = open slot, `----` = empty grid cells, `.` prefix = illegal/dim):

```
    .______________________________.
  / S trump . deck 8           12:41 \
 |      4   2  [6]  5   3   6   2    |
|                                      |
|   (Qs > Ks)         Kc > [_]         |
|     ----              ----           |
|     ----              ----           |
|                                      |
|  +------+    > throw in Kh           |
|  |      |    cap 2 . disc 7          |
|  |  Kh  |                            |
|  |      |    crown: 2 legal + [v]    |
|  +------+                            |
|                                      |
|   .6h .Ah .6s .Tc .Jd [Kh] (v)       |
|                                      |
|           * o   (table|roster)       |
|                                      |
|                                      |
|                                      |
|                                      |
|                                      |
 |                                    |
  \                                  /
    '------------------------------'
```

Zones top-to-bottom: status (24 px) · opponent counts (25 px, shield boxed) ·
pair grid (2×3, look-zone) · focus card + label column (the touch zone) ·
chip strip + ✓ (look-zone + terminal detent) · page dots.

## 4.5 · Option G — "First person, refined" (ships)

F's architecture, twice refined against the owner's running 40 mm simulator
build (measurements taken from its screenshot):

- **The back chevron is immovable** — ~66 px circle, top-left; nothing else
  lives in that corner. The clock is large, top-right.
- **The line under the time** — the empty band below the clock carries
  flip-card glyph · deck · discard (`6♠ ▤8 ▨7`), right-aligned as the
  clock's column. **No capacity number**: cap is derivable — the defender's
  shield count minus the open attacks in view. The flip glyph decays to a
  bare suit icon once drawn (`view.c:19-26`, `game.c:321-333`).
- **One context pill** — the only action chrome. ATTACK (gold) on a legal
  focused card; COVER while defending; TAKE n when the focused card can't
  cover; GOOD (green) otherwise. The crown browses the **whole hand** — the
  pill simply doesn't offer ATTACK on dead cards. No terminal ✓ chip, no
  boxed checkmark anywhere.
- **Covers commit immediately when unambiguous** — if the focused card beats
  exactly one open attack, tap = cover. Only a genuine tie opens a chooser
  overlay: giant target cards + ✕ (top-left, same spot as the system
  chevron). No selection state on the table itself — no gold rings, and open
  attacks are **bare glyphs** (no red ring, no dashed slot).
- **Table = two big pairs per swipe page** — 62 px glyphs, dots below;
  six pairs = three dots, nine = five. No text to clip, ever.
- **Rejection = red edge-glow + ✗ haptic** — ~600 ms, zero words; the card
  is already back in the strip.
- **Goods** — green counts in the strip only; the roster shows the shield
  icon (not the word "defending") and carries names, counts, the flip, and
  who's out.
- **Notifications** — D0's anatomy with five-word copy ("Kat passed to you" ·
  one fact line · Open Hand / Take 2 / Dismiss), plus the **short-look
  glance** (icon · FOOLISH · title) for the wrist-raise moment.
- **One card per action** — multi-card pass stays web-only (`game.c:759`).

## 4.6 · Option H — "All vertical" (**ships** — built in `ios/WatchFoolish`)

Owner-directed variant of G: every horizontally-movable thing rotated
vertical, and the double hand display removed.

- **Table = vertical list** (left column): one pair per row, cover column ▸
  attack column; open attacks alone in the attack column, full bright;
  resolved rows desaturated. Up to 4 pairs visible at once — no paging;
  beyond that the list scrolls under a vertical drag (the app's only drag),
  with edge fades and idle-only auto-scroll to the newest attack.
- **Hand = fisheye lane** hugging the crown edge (right): one representation —
  the focused chip IS the big card (42 pt), neighbors shrink above/below
  (±1 at 18 pt, ±2 at 12 pt, faded). The terminal ✓ / TAKE +n is the lane's
  last stop and magnifies like any card. No chip strip, no focus slot.
- **Roster** = seat-strip tap, pushed page, system chevron returns. No root
  pager, no page dots, zero horizontal gestures.
- **No pills** — the verb is a complication-sized caption (≈10.5 pt) under
  the fisheye: always one of **attack · good · pickup · cover · pass ·
  cover/pass**, blank on a dead card. Commit = tap the big focused card (or
  the caption). cover / cover-pass ambiguity still routes to the chooser
  screen. Note the vocabulary shift: the take action is labeled **pickup**
  (engine term).
- Everything else identical to G: the same action decision table (rendered
  as captions), chooser overlay, red-glow rejection, InfoLine, one card per
  action. The pill's old bottom-left footprint returns to the table list —
  five rows visible instead of four.

Trade-offs vs G: H shows the whole table at once (≤4 pairs) and has a single
hand representation with a physical crown-to-lane mapping; G has a bigger
focused card (56 vs 42 pt) and bigger table glyphs (31 vs 19 pt), and is
simpler to build (no fisheye, no region-scoped scroll). Mocked as H1–H5 +
live demo in the HTML (§8).

### 4.6.1 · As built — where the shipped code departs from the mock

H is now the watch client (`ios/WatchFoolish`, replacing G's TablePager /
FocusSlot / ChipStrip / pill). Four owner calls made during the build, each of
which the mocks do **not** show:

1. **The fisheye window is ±2, the focus is ~36 pt, and ±2 is drawn at the ±1
   size** — the ring is graded by *opacity alone* (1.0 / 0.9 / 0.45), not by
   size. The mock shrank each ring (v1: 18 pt then 12 pt; v2: dropped ±2
   entirely), which made the outer items read as debris. A 42 pt focus cannot
   fit a ±2 window on a 197 pt face at all — v1 tried and its +2-below fell off
   the screen. At ~36 pt the window fits with only the bottom ±2 clipping a few
   pt on the face's edge, which doubles as the lane's "more below" cue.
   *(The §4.6 bullet above still describes the v1 ±2 window; v2 shrank it to ±1
   in the HTML but never updated the prose. ±2 is the shipped answer either
   way.)*
2. **The verb is gray, uppercase, plain system SF, and 7 pt** — the same size
   as the header's column labels; `WFont.caption` is the single token for every
   chrome word in the app, table and chooser alike. The mock colour-coded each
   verb (gold/blue/green/red) in a custom rounded face at ~10.5 pt. Gray +
   system type matches the platform and lets the lane, not the caption, carry
   colour. It sits **directly under the focused card** (y ≈ 158 pt), which is
   why the lane's down-offsets are larger than its up-offsets: the caption
   lives in that gap.
3. **The table list is centred** in its column until it overflows, then it
   scrolls (with the edge fades). The mock always top-aligns; a one-pair table
   clinging to the top of the column reads as a bug. Its glyphs and the ▸ are
   **larger** than the mock's (17 pt / 13 pt), which costs a row: **four** are
   visible, not v2's five — i.e. back to what §4.6 asked for originally.
4. **The chooser is a row of captioned choices** — no tiles, no prompt string,
   no receiver name. Each item is a bare icon that IS its own button with one
   word under it: a cover target renders as its glyph captioned `COVER`; pass
   renders as a blue **↑** captioned `PASS`. (Icons shrink from 30→21 pt past
   three choices — a trump can cover several same-rank attacks.) It only ever
   opens for genuinely ambiguous actions: ≥2 cover targets, or cover+pass.
5. **Arrows carry the two "where do the cards go" actions.** The defender's
   lane terminal is a red **↓** (pickup — they come to you), pairing with the
   chooser's blue **↑** (pass — it goes onward). The mock's `+n` count next to
   pickup is gone; the table list already shows what you'd be taking.
6. **Dimming desaturates; it does not fade.** `Glyph(dim:)` used to drop the
   whole card to 32 % opacity, which pulled the suit AND the rank knocked out
   of it toward black together — measured on the sim, the suit fell to
   luminance 78 against ink at ~5 and **the card lost its value**. It now holds
   the suit at a mid gray (`WColor.suitDim` #6E6E73) with dark ink: separation
   111 at the focus, 100 at ±1. This is the failure §2 already warned about for
   the table pager ("desaturate toward gray, not pure opacity"); the shared
   glyph component simply never did it. The lane's ±2 ring also went 0.45 →
   0.6 opacity, since a dim card at 0.45 crushed the rank away again (50).
7. **The InfoLine is two rows, not one**: a 7 pt label line
   (`FLIP|TRUMP · DECK · DISC`) over a values row that is therefore just
   *card icon · number · number*. The mock inlined SF-symbol deck/discard icons
   next to each number; naming the columns once, in the header, retires them.
   The first label switches FLIP → TRUMP when the flipped card is drawn.
8. **Elimination is shown, not stated.** The roster's `out: …` footer line is
   gone and the strikethrough with it; an escaped player's row simply goes dark
   gray (`WColor.out`) at the bottom of the list.
9. **"Zero horizontal gestures" did not survive contact.** The owner wants the
   roster on a swipe as well as the strip tap, so the root is a horizontal
   pager again (Table page 0, Roster page 1): drag right→left for the roster,
   tap the seat strip for the same. **Still no page dots** — the face has no
   room and the strip is the discoverable door. The roster is *not* a pushed
   page. Opening on the table is load-bearing: it keeps watchOS's left-edge
   back-swipe (pop to the games list) unshadowed, which putting the roster on
   the left would have cost. So H's remaining gesture claims are: the crown is
   never navigation, and the table list is the only drag.

The seat-strip cell caps at 18 pt (G said 22) — that, plus the four-row table,
is what pays for the two-row header without pushing the lane off the bottom.
Everything on this face is within ~3 pt of overflowing; re-check the whole
column before growing any of it.

### 4.6.2 · Colour means state, and only state

The strip and the roster share one colour function. **There is no "you" colour** —
gold used to mean "you", which made it the one hue that said nothing about the
game. You are marked by *weight* instead, which frees colour entirely:

| colour | meaning |
|---|---|
| **red** | the opener, *while the bout is still waiting on their opening attack* |
| **orange** | defender (and the shield) |
| **green** | said GOOD — these counts are the vote tally |
| **dark gray** | escaped |
| **white** | everyone else |

Red is **transient**: it clears to white the instant they attack, so it reads as
"we're waiting on them", not as a permanent role. It is derived from public state
(`firstAttacker` + an empty table) because the bridge deliberately masks
`awaitingAttack` to the viewer's own seat (`ios_api.c:172`).

Precedence: out ▸ defender ▸ good ▸ opening ▸ white. Two of those can never
actually collide — the defender can never say GOOD (`game.c:844`), and a seat
that still owes the opening attack cannot have voted (`legal.c:377-384`).

**Your seat is the digit's weight**: semibold, against regular for everyone else.
That is deliberately a *short* step on SF's weight axis (2 stops); heavy-vs-light
is 5 stops and reads as two different typefaces. Two dead ends got here, both
worth not repeating:

- *An underline.* When you are also the defender the shield wraps your seat and
  its bottom tip lands exactly where the underline goes; at 8 seats that hid it
  completely.
- *An outline (solid you / hollow them).* **SwiftUI cannot stroke `Text`.** There
  is no stroke modifier, and `AttributedString`'s `strokeWidth` is a UIKit
  attribute that SwiftUI's `Text` ignores. The SwiftUI route is
  [`textRenderer(_:)`](https://developer.apple.com/documentation/swiftui/view/textrenderer(_:)),
  **watchOS 11+** — we target 10.0. (Note Apple's own docs disagree here: the
  [`TextRenderer` protocol page](https://developer.apple.com/documentation/swiftui/textrenderer)
  claims watchOS 10 / iOS 17, one major version early across every platform;
  the modifier page's watchOS 11 is the one to trust, and it is the binding
  constraint anyway.) CoreText *can* do it at watchOS 10 —
  `CTFontCreatePathForGlyph` vends real glyph paths — and it was built and it
  worked; it was dropped because a hairline outline is simply less legible than
  weight at 12.5 pt, not because it was impossible.

Nothing forces a deployment bump: the App Store's 28 Apr 2026 rule is about the
**build SDK** (watchOS 26), not the minimum OS. Bumping to 11 would cost Series
4, Series 5 and 1st-gen SE. Apple does not publish watchOS adoption figures.

### 4.6.2 · Tuning — in Xcode, not in the app

Every number above is a constant in `HTuning.swift`; no layout view holds a
literal. `Previews.swift` renders the real screens on **static fixtures** — a
fixed `GameView` plus a fixed legal menu, no kernel, no bots, no deal. Edit a
number in `HTuning.swift` (or a fixture in `Previews.swift`), hit save, and the
canvas redraws. That is the whole loop: **there is no tuning UI in the app**, and
there should never be one.

Why static: booting a real game in a preview is slow, non-deterministic, and
fights the single-global C kernel — you get whatever the deal gives you instead
of the state you wanted to look at. `WatchGame.init(preview:legal:)` is the seam
(no LocalGame behind it); `Deal` in Previews.swift is a small builder — hands are
strings like `"6h Ac 10s"`. Previews cover the 8-player table (attacker, all-dim,
defending/pickup, empty, 7-pair scroll, 11-card hand, all-voted), heads-up, both
chooser shapes, the roster (incl. escapees), the games list, and game over.

### 4.6.3 · The bot, and pacing

The watch plays **octogen** (`WatchGame.defaultStrategy`) — it was already in the
offline roster (`cnitro/ios/ios_api.c` ROSTER, built `-DCD_LEAFBOOK` so its
endgame oracle is live); the watch simply never asked for it. LocalGame's
thermal guard still downgrades seats to espresso on a hot device (§7.2), which
is what makes an 8-seat table of a heavy MC solver safe to ask for.

**Bots move asynchronously to you**, as they do on the server. `LocalGame`'s
drive loop used to stop the moment the human was an eligible actor — but Durak
has no turn order for throw-ins (any non-defender, any time — `game.c:538`), so
you and the bots are routinely eligible together. The loop therefore froze every
bot until you played, which made GOOD feel like a "let the bots move" button. It
now only stops when the human is the *sole* eligible actor (nothing can change
until you move anyway). This lives in shared FoolishKit, so **the phone app gets
the same fix**.

Inspection flags (simulator has no Crown): `-table`/`-table4`/`-table8` deal a
real offline game, `-focus N` parks the lane on item N, `-pairs`/`-pairs7`
inject table rows (`-pairs7` overflows the five-row list), `-roster` pushes the
roster, `-chooser` opens the overlay.

## 5 · The other options (all kept in the HTML)

- **F · First person** — G's chassis; superseded by G's refinements.
- **D · One thing at a time** — full-bleed crown pages, one tap zone,
  strictly ≥ 12 pt everywhere. The purist fallback. Its layer 0 carries into
  G unchanged (with the trimmed notification).
- **E · Foolish40 × D** — the first merge (ring + glyphs + pill); superseded
  by F/G, which deleted self-rendering, pagination, and curved names.
- **B · Focus flow** — killed by the type audit (documented in HTML §3).
- **A · Dense board** — 44/46 mm only. **C · Action list** — the VoiceOver
  projection of F's detent list.

## 6 · Cut list (cumulative)

- Curved ring names (→ roster page rows), "waiting on X" indicators (no turn
  order exists, fact #4), any round countdown (no timer, fact #5), popup
  sheets (→ adjacent crown detents/pages), ×2 duplicate chips (single deck,
  fact #2), avatar ring, discard-pile card list (count only; exact list is
  Oracle-tier), animations/evwire playback (snapshot trailers suffice),
  chat/emotes/replay, hand rearrange (legal-first auto-order wins),
  multi-select / count sheets (G: one card per action; double-pass web-only),
  goods on the roster page (green strip counts only), icon glyphs in pills
  (⚔ rendered as ✕ in mono fonts — words won).

## 7 · Client stack notes

- **State in:** subscribe the player's `player_views` row (pre-masked
  server-side); render snapshots, ignore the event stream.
- **Legality:** guards.wasm (~23 KB) per-card gates paint chips and build the
  detent list; one awire buffer per move for gate + POST.
- **Rejects:** `ENGINE_REJECT_*`/`STALE_ROUND` → ✗ tick + label-line copy
  (`rejectMessages.ts`); revert the in-flight card; re-render the pushed row.
- **Haptic triggers:** derivable client-side from consecutive snapshots
  (rank-liveness set, defender index, goods mask) — no new server surface.
- **No timers to render** (`good.ts:7-8`).
