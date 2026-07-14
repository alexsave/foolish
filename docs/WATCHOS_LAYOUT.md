# Foolish on the wrist — 40 mm watchOS layout study

Deliverables:

- **`docs/watchos-layout.html`** — the canonical mockups: every screen state as
  HTML/CSS/JS at native watch pixels, viewable at true physical size (with a
  monitor-calibration control), with a 10 mm-finger overlay, a 2×3 tap-zone
  overlay, and a live crown/tap interaction demo. Three design options, each
  with per-element rationale.
- **This doc** — the verified rules the layout depends on (with citations),
  the corrections to the brief, the physical math, the interaction model, and
  two ASCII reference frames of the recommended option.

Scope note: the brief moved from 44 mm to **40 mm** mid-flight; all numbers
below are for the 40 mm panel. The 44 mm variant is strictly easier — Option A
in the HTML becomes viable there.

---

## 1 · Rules facts the layout depends on

Everything below was read out of the engine — the C kernel is the single
source of truth (the TS server and the browser both run it compiled to wasm).
Where the brief's assumption was wrong, the design follows the code, not the
brief.

| # | Question | Answer from the code | Citations |
|---|----------|----------------------|-----------|
| 1 | Deck composition | 2–5 players: 36 cards (values 5–13 → ranks 6–A). **6–8 players: full 52** (ranks 2–A). One of each suit/rank; hands deal & refill to 6. An 8-seat game deals 48 and flips one, leaving a stock of **3 + the flip** — the stock dies in about a round. | `cnitro/src/card.h:15-29`, `supabase/functions/_shared/constants.ts:33-39`, `card.h:25` |
| 2 | Duplicate cards in one hand | **Impossible.** The deck is generated one-of-each (`refill_deck`); a move containing duplicates is rejected (`ENGINE_REJECT_DUPLICATES`); the hand-rearrange endpoint explicitly validates a permutation to prevent duplicate-minting. Opponents' *masked* cards are wire placeholders `{0,1}` — rendered by count, never identity. | `game.c:305-316`, `game.c:524-528,545`, `rearrange.ts:5-11,23-32`, `view.c:61-64`, `src/wasm/clientGuards.ts:15-20` |
| 3 | Max table slots / throw-in cap | **No fixed slot count.** The only rule cap: the defender's current hand must cover the load — reject when `defender_cards < uncovered + n`. Buffers are build params (`MAX_BATTLES` 32 native / **64 wasm+iOS**); one submitted move carries ≤ 28 cards. Covering is capacity-neutral (hand −1, uncovered −1), so a bout vs a fresh 6-card hand tops out at 6 pairs — but a **post-pickup defender can legally face dozens**. | `game.c:557-559`, `game.h:12-19`, `cnitro/Makefile:127,372`, `awire.h:26-27` |
| 4 | Who may throw in, when | After the opening attack: **any non-defender, any time** — no adjacency, no turn order, no "waiting for you" state. Thrown values must already be on the table, and **defense cards count** — a cover opens its value for new throw-ins. Opening attack: first-attacker only, single value, 1–n cards. | `game.c:538,551-555`, `game.c:507-513`, `game.c:547-550` |
| 5 | How a round closes | Three ways, **no timer** (the old 60 s auto-discard is disabled): ① all attacks covered **and** every in-play attacker explicitly says GOOD — and any attack/cover/pass **resets everyone's GOOD**; ② the defender's covering play empties their hand → instant close; ③ defender picks up → round over, defender skipped for the next lead. The opening attacker cannot GOOD before attacking. Saying GOOD **locks you out of acting** until the table changes. `good_timestamp` is recorded but nothing consumes it to close a round. | `supabase/functions/_shared/actions/good.ts:7-8`, `game.c:840-869`, `game.c:572,696-697,747-748`, `game.c:657-694`, `game.c:799-802`, `game.c:845`, `legal.c:377-384`, `types.ts:150` |
| 6 | Transfer (perevod) | **Legal.** `handle_pass`: defender only; before any cover exists; every table attack shares the passed value (pre-cover the table is always single-valued, so in practice: hold a matching card); receiver = next in-play player, who must hold `≥ passed + battles`; the defender role moves on. Chains are legal. No "show trump to pass" variant. A post-mutation `PASS_OVERFLOW` guard aborts pathological states. | `game.c:716-772` (esp. `:731,:732,:734-735,:759,:763-770`) |
| 7 | Legal-moves function | Both exist. Full enumerator kernel-side (`calculate_legal_moves`, capped 65,536 moves — used by bots/server). The **client ships a 23 KB validate-only kernel** (guards.wasm): synchronous `canAttack / canCover / canPass / canPickup / canCoverPair` gates plus `validateActionWire` — same C code the server applies. So a watch client can legality-shade each card cheaply without enumerating. | `legal.c:358-386`, `bot_strategy.ts:143-151`, `clientGuards.ts:1-27,219-285` |
| 8 | Events or snapshots | **Both.** Per action: an evwire event sequence, per-viewer masked, every event carrying a full masked state snapshot, plus a final-state trailer. Standing: per-player **pre-masked packed snapshots** in the `player_views` table, pushed via Realtime. A watch can ignore events entirely and render trailers/rows — animation is optional by design. | `evwire.h:1-27`, `player_views.ts:1-13`, `game.h:166-190` |
| 9 | Nitro's observation encoding | Bot decisions run wholly inside bots.wasm. Per decision the bridge marshals: full game state, per-seat strategy keys, an FNV game key, the session log (u16 count; per record: type, seat, defender, card pairs; hidden cards travel as `0xFE`), env knobs, and a server-secret-derived strategy RNG. Honest bots build hidden-card beliefs from the public log; only `novichok` reads real hands (research). The distilled policy scores a **55-feature vector** per candidate move. | `bots.ts:126-162,244-289`, `strategy.h:41`, `distill_feat.h:15` |

### Where the brief was wrong (and what changed)

1. **"Max table slots"** — there is no slot grid. The cap is the defender's
   live hand (`game.c:557-559`). Any fixed 6-slot mat design is wrong; the
   table view must scroll (crown), and the capacity number (`cap n`) is the
   thing players actually race against — it rides in the always-visible
   context band.
2. **"Round closes by timer / passes / defender beats the last pair"** — no
   timer exists (`good.ts:7-8`). The everyday close is *unanimous explicit
   GOOD*, and it resets on any table change. "Beats the last pair" closes a
   round only when the play empties the defender's hand (`game.c:657`).
   Consequence: **GOOD is a first-class, always-reachable button** — it is the
   mechanism of progress, not chrome. It also *locks* you until the table
   changes (`legal.c:377-384`), which becomes its own screen state (B9).
3. **"A duplicate card in hand"** — impossible by construction; the screen
   state is cut. What replaces it: the only two moves that must be **atomic**
   are multi-card leads (`game.c:549`) and multi-card passes (after one pass
   you are no longer the defender, `game.c:759`) — each gets a two-button
   count sheet. Ordinary multi-card throw-ins don't need batching: sequential
   single throws reach the same states (a landed card's value is on the table
   for the next one).
4. **"It's a race"** — true, but only between attackers. Covers cannot be
   sniped (only the defender covers, targeting exact cards,
   `game.c:621-629`). What a rival's throw-in *can* snipe: your attack's
   capacity (`DEFENDER_CAPACITY`), and the defender's transfer window
   (receiver capacity, `game.c:734-735`). The server serializes actions
   (`utils.ts:63`, version fence `utils.ts:829-837`) and losers get typed
   rejects incl. `STALE_ROUND` (`rejectMessages.ts:30-39`). Design response:
   optimistic send + toast, never a blocking confirmation.

---

## 2 · Physical budget (40 mm)

- Panel: **324 × 394 px** @ 326 ppi → **25.3 × 30.7 mm**, corner radius
  ≈ 4.7 mm = **60 px**.
- A 10 mm fingertip = **128 px = 40 % of panel width** → at most **2 tap
  targets per row, 3 per column**. This single number designs the screen.
- Strict no-corner-clip inset: r·(1−1/√2) ≈ 18 px per corner; conservative
  flat content rect ≈ 21 × 27 mm.
- The Digital Crown is the only precise pointer; watchOS always draws the
  clock top-right — the status row is designed around it.
- Input grammar used everywhere: **crown = select, tap = commit**, and the
  only destructive tap target is the screen's center.

## 3 · The three options

Full mockups with per-element rationale are in `docs/watchos-layout.html`.

- **A · Dense board** — persistent table strip + hand carousel + actions on
  one screen (the web layout miniaturized). Best glanceability; breaks at
  40 mm because 66 px pair chips are half a fingertip and two scrollable
  strips fight over one crown. Keep as the 44/46 mm variant.
- **B · Focus flow** *(recommended)* — one decision per screen. Attacker:
  crown scrolls the hand carousel (illegal cards dim with a reason), tap the
  center card to throw, GOOD across the bottom; table detail lives one tap
  away (context band → crown-scrolled pair peek). Defender: two-step wizard —
  crown picks the open attack, tap; crown picks among *legal covers only*,
  tap; TAKE/PASS ride the bottom. Scales unchanged from 2 pairs to 20.
- **C · Action list** — every legal move is a full-width row in a
  crown-scrolled list. Bulletproof targets, free accessibility; but rows
  reorder under your finger as rivals act, and pair×cover rows explode for
  defenders. Keep the row anatomy as the VoiceOver projection of B.

Screen states covered in the HTML (Option B): attacker happy path · nothing
legal · table capped · sniped (reject toast) · lead-count sheet · defender
with 3 open pairs · cover picker · transfer window · GOOD-said lock ("out of
the round") · out of the game (escaped, watching) · table peek · round-close
interstitial · game over.

## 4 · ASCII reference frames (Option B)

Scale: **1 char ≈ 8 × 16 px** — precisely, the 40 × 25 grid maps to the
324 × 394 panel at **8.1 px/char horizontally, 15.76 px/char vertically**
(0.6 % / 1.5 % off nominal). Corner radius 60 px = 7.4 chars / 3.8 rows; the
frame rows inset 4 / 2 / 1 / 0 chars to draw it to scale. ASCII-only card
codes (`Jd` = J♦, `7s` = 7♠) keep the grid monospace-exact.

Attacker, legal throw-in focused (the race screen):

```
    .______________________________.
  / S trump . deck 3           12:41 \
 | vs KAT(4) . open 2 . cap 2   [TBL] |
|                                      |
|------+    +==============+    +------|
|      |    | J            |    |      |
|  8d  |    |              |    |  7s  |
| (dim)|    |      Jd      |    |      |
|      |    |              |    |      |
|      |    |              |    |      |
|      |    |              |    |      |
|      |    |            J |    |      |
|------+    |--------------|    +------|
|           | TAP = THROW  |           |
|           +==============+           |
|        o * o o o o   hand 2/6        |
|                                      |
|      LEGAL - J is on the table       |
|  +--------------------------------+  |
|  |              GOOD              |  |
|  |            2/6 said            |  |
|  +--------------------------------+  |
 |                                    |
  \                                  /
    '------------------------------'
```

Element rationale (one line each): **status** = trump + stock, the two facts
that reprice every card (clock corner is system-owned); **context band** =
defender · open · cap — the whole race state, and the full-width tap target
for the table peek; **carousel** = crown-driven, neighbors peek, dim =
`canAttack([card])` false; **focused card** = the one destructive target, one
finger wide, footer names the verb; **GOOD** = load-bearing round-closer with
the vote count.

Defending with 3 open pairs (crown cycles open attacks; center card is the
*attack*, not yours):

```
    .______________________________.
  / S trump . deck 0           12:44 \
 | DEFEND . 3 open . 1 covered  [TBL] |
|                                      |
|------+    +==============+    +------|
|      |    | J            |    |      |
|  Jd  |    |              |    |  8h  |
| 1cov |    |      Jh      |    | 3cov |
|      |    |              |    |      |
|      |    |  (3 covers)  |    |      |
|      |    |              |    |      |
|      |    |            J |    |      |
|------+    |--------------|    +------|
|           |  PICK COVER  |           |
|           +==============+           |
|            open pair 2/3             |
|                                      |
|       crown: next open attack        |
|  +--------------------------------+  |
|  |             TAKE 5             |  |
|  |  pass locked - cover is down   |  |
|  +--------------------------------+  |
 |                                    |
  \                                  /
    '------------------------------'
```

Element rationale: **badges** = per-pair legal-cover counts
(`canCoverPair` sweeps) — triage before committing; **TAKE 5** = surrender
with its price (all table cards, `game.c:787-795`); **pass subline** = the
rule for its own absence (`game.c:731`) instead of a hidden button; tapping
the center attack enters the cover picker, whose carousel holds *only legal
covers*.

## 5 · Cut list

- 8-player avatar ring (seat identity matters twice: defender, pass receiver
  — both named in the band; roster lives in the peek footer as hand counts).
- Discard pile & counting aids (Oracle-tier, wrong device). Deck count stays.
- Animations / evwire playback — snapshot trailers only (`evwire.h:24-27`).
- Chat, emotes, replay, Oracle.
- Hand rearrange (crown carousel auto-groups by rank instead).
- Multi-card cover batching (engine takes covers pair-at-a-time,
  `game.c:638-655`; sequential taps reach identical states, including the
  hand-empty instant win).
- Any "slots" graphic — capacity is a number (`cap n`) in the band.

## 6 · Client stack notes

- **State in:** subscribe the player's `player_views` row (pre-masked
  server-side, shared packed-game codec) — `player_views.ts:1-13`.
- **Legality:** embed guards.wasm (~23 KB, fixed memory) and gate per-card;
  build one awire buffer per move and use it for gate + POST —
  `clientGuards.ts:268-285`, `awire.h:1-13`.
- **Rejects:** map `ENGINE_REJECT_*` / `STALE_ROUND` to toast copy
  (`rejectMessages.ts`), revert optimistic state, re-render the pushed row.
- **No timers to render** (`good.ts:7-8`); the round-close interstitial's 2 s
  is pure client pacing.
