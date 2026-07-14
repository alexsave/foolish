# Foolish on the wrist — 40 mm watchOS layout study

Deliverables:

- **`docs/watchos-layout.html`** — the canonical mockups: every screen state as
  HTML/CSS/JS at native watch pixels, viewable at true physical size (with a
  monitor-calibration control), with a 10 mm-finger overlay, a 2×3 tap-zone
  overlay, and live crown/tap interaction demos. Four design options, each
  with per-element rationale.
- **This doc** — the verified rules the layout depends on (with citations),
  the Apple HIG constraints that picked the winner (with quotes), the
  corrections to the brief, and ASCII reference frames of the recommended
  option.

Scope notes: the brief moved from 44 mm to **40 mm** mid-flight; all numbers
are for the 40 mm panel. A first-pass design ("Focus flow", Option B in the
HTML) was **rejected against the HIG type floor** — half its text sat below
Apple's 12 pt minimum — and replaced by Option D, which is built from the
HIG's own architecture rather than a shrunken board.

---

## 1 · Rules facts the layout depends on

Everything below was read out of the engine — the C kernel is the single
source of truth (the TS server and the browser both run it compiled to wasm).
Where the brief's assumption was wrong, the design follows the code.

| # | Question | Answer from the code | Citations |
|---|----------|----------------------|-----------|
| 1 | Deck composition | 2–5 players: 36 cards (values 5–13 → ranks 6–A). **6–8 players: full 52** (ranks 2–A). One of each suit/rank; hands deal & refill to 6. An 8-seat game deals 48 and flips one, leaving a stock of **3 + the flip** — the stock dies in about a round. | `cnitro/src/card.h:15-29`, `supabase/functions/_shared/constants.ts:33-39`, `card.h:25` |
| 2 | Duplicate cards in one hand | **Impossible.** The deck is generated one-of-each (`refill_deck`); a move containing duplicates is rejected (`ENGINE_REJECT_DUPLICATES`); the hand-rearrange endpoint validates a permutation to prevent duplicate-minting. Opponents' *masked* cards are wire placeholders `{0,1}` — rendered by count, never identity. | `game.c:305-316`, `game.c:524-528,545`, `rearrange.ts:5-11,23-32`, `view.c:61-64`, `src/wasm/clientGuards.ts:15-20` |
| 3 | Max table slots / throw-in cap | **No fixed slot count.** The only rule cap: the defender's current hand must cover the load — reject when `defender_cards < uncovered + n`. Buffers are build params (`MAX_BATTLES` 32 native / **64 wasm+iOS**); one submitted move carries ≤ 28 cards. Covering is capacity-neutral (hand −1, uncovered −1), so a bout vs a fresh 6-card hand tops out at 6 pairs — but a **post-pickup defender can legally face dozens**. | `game.c:557-559`, `game.h:12-19`, `cnitro/Makefile:127,372`, `awire.h:26-27` |
| 4 | Who may throw in, when | After the opening attack: **any non-defender, any time** — no adjacency, no turn order. Thrown values must already be on the table, and **defense cards count** — a cover opens its value for new throw-ins. Opening attack: first-attacker only, single value, 1–n cards. | `game.c:538,551-555`, `game.c:507-513`, `game.c:547-550` |
| 5 | How a round closes | Three ways, **no timer** (the old 60 s auto-discard is disabled): ① all attacks covered **and** every in-play attacker explicitly says GOOD — and any attack/cover/pass **resets everyone's GOOD**; ② the defender's covering play empties their hand → instant close; ③ defender picks up → round over, defender skipped for the next lead. The opening attacker cannot GOOD before attacking. Saying GOOD **locks you out of acting** until the table changes. `good_timestamp` is recorded but nothing consumes it to close a round. | `actions/good.ts:7-8`, `game.c:840-869`, `game.c:572,696-697,747-748`, `game.c:657-694`, `game.c:799-802`, `game.c:845`, `legal.c:377-384`, `types.ts:150` |
| 6 | Transfer (perevod) | **Legal.** `handle_pass`: defender only; before any cover exists; every table attack shares the passed value (pre-cover the table is always single-valued, so in practice: hold a matching card); receiver = next in-play player, who must hold `≥ passed + battles`; the defender role moves on. Chains are legal. No "show trump to pass" variant. | `game.c:716-772` (esp. `:731,:732,:734-735,:759`) |
| 7 | Legal-moves function | Both exist. Full enumerator kernel-side (`calculate_legal_moves`, capped 65,536 moves — bots/server). The **client ships a 23 KB validate-only kernel** (guards.wasm): synchronous `canAttack / canCover / canPass / canPickup / canCoverPair` gates plus `validateActionWire`. A watch client can legality-shade each card cheaply without enumerating. | `legal.c:358-386`, `bot_strategy.ts:143-151`, `clientGuards.ts:1-27,219-285` |
| 8 | Events or snapshots | **Both.** Per action: an evwire event sequence, per-viewer masked, every event carrying a full masked state snapshot, plus a final-state trailer. Standing: per-player **pre-masked packed snapshots** in the `player_views` table, pushed via Realtime. A watch can ignore events entirely and render trailers/rows. | `evwire.h:1-27`, `player_views.ts:1-13`, `game.h:166-190` |
| 9 | Nitro's observation encoding | Bot decisions run wholly inside bots.wasm. Per decision the bridge marshals: full game state, per-seat strategy keys, an FNV game key, the session log (hidden cards = `0xFE`), env knobs, and a server-secret-derived strategy RNG. Honest bots build hidden-card beliefs from the public log; only `novichok` reads real hands. The distilled policy scores a **55-feature vector** per candidate move. Nothing here needs wrist UI. | `bots.ts:126-162,244-289`, `strategy.h:41`, `distill_feat.h:15` |

### Where the brief was wrong

1. **"Max table slots"** — no slot grid exists; the cap is the defender's live
   hand (`game.c:557-559`). The table must scroll (crown), and `cap n` is the
   number players race against — it rides the top overlay of every screen.
2. **"Timer / passes / defender beats last pair"** — no timer (`good.ts:7-8`).
   The everyday close is *unanimous explicit GOOD*, reset by any table change;
   "beats the last pair" closes only when the play empties the defender's
   hand (`game.c:657`). GOOD is therefore a first-class destination — and its
   lock-in (`legal.c:377-384`) is its own screen state.
3. **"Duplicate card in hand"** — impossible; state cut. The real atomic-move
   needs are multi-card leads (`game.c:549`) and multi-card passes
   (`game.c:759`), which become **adjacent crown pages**, not popup sheets.
4. **"It's a race"** — between attackers only. Covers can't be sniped
   (defender-only, exact-card targets, `game.c:621-629`); what can: attack
   capacity, and the transfer window (`game.c:734-735`). Server serializes
   under a version fence (`utils.ts:63,829-837`); losers get typed rejects
   (`rejectMessages.ts`). Design response: optimistic send + banner, never a
   blocking confirm.

---

## 2 · Physical budget (40 mm)

- Panel **324 × 394 px** @ 326 ppi → **25.3 × 30.7 mm**, corner radius
  ≈ 4.7 mm = **60 px**. Points are px/2: a **162 × 197 pt** canvas.
- A 10 mm fingertip = **128 px = 40 % of panel width** → at most 2 tap targets
  per row, 3 per column — and ideally **one**.
- The Digital Crown is the only precise pointer; watchOS draws the clock
  top-right, always.

## 3 · What Apple says (fetched from the HIG, not remembered)

- *"People glance at the Always On display many times throughout the day,
  performing concise app interactions that can last for **less than a minute**
  each. People frequently use a watchOS app's related experiences — like
  complications, notifications, and Siri interactions — **more than they use
  the app itself**."* — Designing for watchOS
- *"Support quick, glanceable, **single-screen interactions** … targeted
  actions with a **simple gesture or two**. Minimize the depth of hierarchy …
  use the Digital Crown to provide vertical navigation for scrolling or
  **switching between screens**."* — Designing for watchOS
- *"Starting with watchOS 10, the Digital Crown takes on an elevated role as
  **the primary input for navigation** … people turn the Digital Crown to
  switch between **vertically paginated tabs**."* and *"Anchor your app's
  navigation to the Digital Crown … back them up with corresponding touch
  screen interactions."* — Digital Crown
- Typography: watchOS **default 16 pt, minimum 12 pt** (= 32 / 24 px here;
  SF Compact). Layout: *"The Apple Watch bezel provides a **natural visual
  padding** around your content"* — design full-bleed.
- Apps never respond to crown **presses** (system-reserved); haptic detents
  come free with paging.

**The audit that killed the first design.** Option B ("Focus flow") packed
status + context + carousel + dots + hint + buttons into 394 px. Held to the
12 pt floor: context band 13.5 px ≈ **7 pt**, hint 13 px ≈ **6.5 pt**, card
corner ranks ≈ **10 pt**, button subtext/badges ≈ **5 pt** — all illegal. A
phone information hierarchy, miniaturized; exactly what the HIG warns about.

## 4 · Option D — "One thing at a time" (recommended)

The watch is not a board renderer; it is a **decision surface**. Every choice
is a full-bleed, crown-paginated screen (watchOS 10 vertical TabView):

- **Attacker strip:** [TABLE] → [your cards, legal-first] → [GOOD]. The app
  opens on your best legal card, so the common race play is **zero detents +
  one tap**. Tap anywhere commits the visible page — one tap zone, the whole
  screen. Illegal cards stay in the strip, dim, with the rule as the bottom
  strip text; taps on them do nothing but tick.
- **Defender strip:** [TABLE] → [open attack pages (red-ringed)] → [TAKE] →
  [PASS if legal]. Tapping an attack opens a sub-strip of *legal covers only*
  (+ back). Full-screen TAKE/PASS pages can't be fat-fingered — crown distance
  is the confirmation.
- **Atomic multi-card moves are adjacent pages** — "9♣" then "9♣ + 9♥ lead
  both"; "PASS 7♠" then "PASS 7♠ + 7♥". Sheets deleted.
- **Top overlay (24–25 px):** defender + capacity — the two live numbers — on
  a scrim under the system clock. **Bottom strip (24–27 px):** hint and button
  are the same element ("TAP TO PLAY J♦" / "9 NOT ON THE TABLE").
- **Layer 0 is half the product** (per the HIG quote): a notification with
  safe inline actions (Take is always defender-legal, `game.c:776-780`;
  card-choice actions deep-link in), a Smart Stack widget ("Your move · J♦ 7♠
  playable · cap 2"), and a corner complication badge. In an 8-player game,
  ~90 % of wall-clock isn't your moment — the app is mostly *closed*.
- Every string ≥ 24 px (12 pt); headline voice 27–58 px; pips 170 px.
  Smallest tap target: the screen.

State inventory carries over one-for-one from the brief: nothing legal,
table capped, sniped (banner + failure haptic), GOOD-said lock with the vote
board, escaped-and-watching, round-close interstitial, game over.

## 5 · ASCII reference frames (Option D)

Scale: **1 char ≈ 8 × 16 px** — the 40 × 25 grid maps to 324 × 394 at
8.1 px/char horizontal, 15.76 px/char vertical. Corner radius 60 px =
7.4 chars / 3.8 rows, drawn to scale. `#` = the action strip (full-bleed to
the bottom curve — the corners clip it, by design); right-edge `o/*` = the
vertical page dots; ASCII card codes (`Jd` = J♦, `Jh` = J♥).

Best-card page (the app opens here; whole screen = the tap target):

```
    .______________________________.
  / KAT . cap 2                12:41 \
 |                                    |
|  Jd   <- rank 58px                   |
|                                    o |
|                                    o |
|                 /\                 * |
|                /  \                o |
|                \  /                o |
|                 \/                 o |
|            (pip 170px)             o |
|                                    o |
|                                      |
|  card face fills the panel           |
|  (bezel = the card edge)             |
|                                      |
|                                      |
|######################################|
|###         TAP TO PLAY Jd         ###|
|######################################|
|######################################|
|######################################|
 |####################################|
  \##################################/
    '------------------------------'
```

Open-attack page while defending 3 open pairs (crown stop 2 of 6:
3 attacks + TAKE + PASS + table; `!` = the red table-card ring):

```
    .______________________________.
  / DEFEND 2/3                 12:41 \
 |                                    |
|! Jh   <- rank 58px                  !|
|!                                   o!|
|!                                   *!|
|!                /\                 o!|
|!               /  \                o!|
|!               \  /                o!|
|!                \/                 o!|
|!           (pip 170px)             o!|
|!                                   o!|
|!                                    !|
|! red ring (!) = table card,         !|
|! not yours                          !|
|!                                    !|
|!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!|
|######################################|
|###       TAP - 3 COVERS FIT       ###|
|######################################|
|######################################|
|######################################|
 |####################################|
  \##################################/
    '------------------------------'
```

## 6 · The other options (kept in the HTML for comparison)

- **B · Focus flow** — carousel + three fat zones. Right interaction grammar,
  illegal type sizes; disqualified at 40 mm. Its 13 screen states map 1:1
  onto D's pages.
- **A · Dense board** — persistent table strip; chips at half a fingertip;
  44/46 mm only, and even there D reads better at arm's length.
- **C · Action list** — every move a row; bulletproof and joyless; rows
  reorder mid-race. Its anatomy is D's VoiceOver projection.

## 7 · Cut list

- 8-player avatar ring (defender + pass receiver are named in the overlay;
  roster = hand counts on the table page).
- Discard pile & counting aids (Oracle-tier, wrong device). Deck count stays.
- Animations / evwire playback — snapshot trailers only (`evwire.h:24-27`).
- Chat, emotes, replay, Oracle.
- Hand rearrange (`rearrange.ts` is web-only; legal-first auto-ordering wins).
- Multi-card cover batching (pairs commit one at a time, `game.c:638-655`,
  including the hand-empty instant win).
- Popup sheets of any kind — replaced by adjacent crown pages.

## 8 · Client stack notes

- **State in:** subscribe the player's `player_views` row (pre-masked
  server-side) — `player_views.ts:1-13`.
- **Legality:** embed guards.wasm (~23 KB, fixed memory); per-card gates paint
  the page strip; one awire buffer per move for gate + POST
  (`clientGuards.ts:268-285`, `awire.h:1-13`).
- **Rejects:** map `ENGINE_REJECT_*` / `STALE_ROUND` to banner copy
  (`rejectMessages.ts`), revert optimistic state, re-render the pushed row.
- **Notifications:** fire only on decision moments (you defend / you can
  throw in / all-covered-say-GOOD); the engine emits everything needed to
  detect them in the pushed snapshot.
- **No timers to render** (`good.ts:7-8`).
