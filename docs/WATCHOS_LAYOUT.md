# Foolish on the wrist — 40 mm watchOS layout study

Deliverables:

- **`docs/watchos-layout.html`** — the canonical mockups: every screen state as
  HTML/CSS/JS at native watch pixels, viewable at true physical size (with a
  monitor-calibration control), a 10 mm-finger overlay, a 2×3 tap-zone
  overlay, and five live crown/tap demos. Eight design options (A–H) with
  per-element rationale; **Option H (§8) is the final layout**.
- **`docs/WATCHOS_SPEC.md`** — the implementation handoff: the final Option H
  layout as a self-contained SwiftUI spec (points, decision tables,
  FoolishKit integration, acceptance checklist). **Give the implementor that
  file.**
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
4. **Owner review of F → Option G** ("First person, refined") — pills,
   swipe-paged table, terse verbs. A/B/C were confirmed illegible at real
   size; D's notifications survive.
5. **Owner reviews of G against the running simulator → Option H** ("All
   vertical") — **the final layout.** Single fisheye hand representation
   anchored bottom-right (three small neighbors stacked above), table as a
   vertically-centered list (scrolls past five rows), gray ALL-CAPS caption
   verbs instead of pills (ATTACK · GOOD · PICKUP · COVER · PASS ·
   COVER/PASS), zero horizontal gestures, roster behind a seat-strip tap.
   Handoff: `docs/WATCHOS_SPEC.md`.

---

## 1 · Rules facts the layout depends on

Everything below was read out of the engine — the C kernel is the single
source of truth (the TS server and the browser both run it compiled to wasm).

| # | Question | Answer from the code | Citations |
|---|----------|----------------------|-----------|
| 1 | Deck composition | 2–5 players: 36 cards (ranks 6–A). **6–8 players: full 52** (ranks 2–A). One of each suit/rank; hands deal & refill to 6. An 8-seat game deals 48 and flips one — the stock is **3 + the flip** and dies in about a round. | `c/src/card.h:15-29`, `constants.ts:33-39`, `card.h:25` |
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

## 4.6 · Option H — "All vertical" (alternative, under evaluation)

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
