# Foolish on watchOS — design notes

*Working note, July 2026. This is a **separate future effort**, not a dependency
of the iPhone app. The watch is not a shrunk phone: we throw out the wool / wood /
fern textures and the rectangular playing cards (those are the web + iOS identity)
and lean into a **plain, clean, high‑contrast watch design**. What carries over is
only the kernel — `EngineC`, `Models`, `GameView`, the packed‑view decode, and the
online `GameFeed` are all UI‑agnostic and already live in `FoolishKit`.*

Target: smallest current Apple Watch, **324 × 394 px** (~41 mm, ~162 × 197 pt @2x).

Two screens: a **Table** screen (glance state) and an **Action** screen (play).

---

## 1. Cards become tokens, not rectangles

A 62×88 rectangle with corner indices is unreadable at 41 mm. On the watch a card
is just its **suit glyph with the value laid over it in white** — a poker‑chip
token. Red for ♥/♦, near‑black for ♠/♣; the white numeral reads on both.

```
   rectangular (web/iOS)        token (watch)
   ┌────────┐                     ╭─────╮
   │5       │                     │  ♥  │   ← suit fills the tile
   │   ♥    │        ───▶         │  7  │   ← value overlaid, white, bold
   │      5 │                     ╰─────╯
   └────────┘                     ~36–44 pt square
```

Tokens tile into a grid beautifully and stay legible tiny. Trump is *not* marked
on the token (same call as the phone — the flipped card / trump glyph tells you).

---

## 2. Screen A — Table (the glance screen)

Everyone equally spaced **in a circle**; the four corners carry the counts; the
centre shows the live attacks; one button drops into the Action screen.

```
        ┌───────────────────────────────────┐
        │ ⛁ 12          P3    P4         6 ⛃ │   TL: deck remaining (12)
        │           P2    ·  ·    P5         │   TR: discard count (6)
        │                                    │
        │      P1      ┌─────────┐      P6    │   centre: current attack(s)
        │              │  A♠     │            │     attack token + its cover
        │              │   ╲K♠   │            │     (if any) shown together,
        │      P8      └─────────┘      P7    │     rotated behind/over like
        │                ◀ 1 / 3 ▶           │     the felt.  "1/3" = swipe or
        │                                    │     dial to rotate through them.
        │  ♦            (you)          ▶ play │   BL: flipped trump card, or just
        └───────────────────────────────────┘       the ♦ glyph once it's drawn
                                                  BR: → Action screen button
```

- **Players in a circle**: equally spaced on the rim; you're implicit at 6 o'clock
  and never drawn as a seat (your cards live on the Action screen). Each opponent
  is a **small circle with the hand‑count number inside it** — that's the primary,
  always‑on datum. The **defender** gets a distinct **coloured ring** around their
  circle (the single most important "who's on the spot" cue); the attacker a
  subtler mark; an eliminated seat dims out.

  ```
  seat i of n:  θᵢ = -90° + i·(360°/n)        (start at top, clockwise)
  x = cx + R·cos θᵢ ,  y = cy + R·sin θᵢ      R ≈ 0.42·min(w,h)

     normal        defender            attacker
      ╭───╮        ╔═══╗ ← gold        ╭───╮
      │ 6 │        ║ 4 ║   ring        │ 5 │·  small dot mark
      ╰───╯        ╚═══╝               ╰───╯
  ```

- **Names arc around the circle.** The count lives *inside* the pip; the username
  (only if there's room) is set on a **curved baseline that follows the table
  circle** — letters rotated tangent to the rim so it reads as a clean ring of
  names rather than flat labels colliding with neighbours. Outside seats (near the
  rim) arc their text outward; it degrades to no‑name (count only) when tight.

  ```
          P2 ‿ ‿ P3 ‿ ‿ P4          ← baseline curves with the circle;
        ‿                   ‿          each name's glyphs tilt to the tangent
      P1                       P5
  ```

- **Corners** (fixed, glanceable):
  - **top‑left** — cards left in the deck (`⛁ 12`)
  - **top‑right** — discard pile count (`6 ⛃`)
  - **bottom‑left** — the flipped trump *card* while it's still in the deck; once
    the deck empties, collapse to just the **trump suit glyph** (`♦`)
  - **bottom‑right** — **▶ play** → the Action screen
- **Centre — the attacks.** This is the one I'm least settled on (and so are you).
  Proposal: show **two battles at a time**, big; a `◀ 1 / 3 ▶` control rotates
  through the rest via **swipe or Digital Crown**. A **cover is always drawn on
  its attack** (attack token with the defence token laid across it), so you never
  lose the pairing:

  ```
     uncovered        covered
      ╭─────╮        ╭─────╮
      │ A♠  │        │ A♠ ╲│      defence token laid across the attack,
      ╰─────╯        ╰──╲──╯      slight rotate — reads as "beaten"
                        ╲K♠╲
  ```

  Open question: 2‑at‑a‑time vs. a single big battle with the crown cycling. Decide
  with a real hand of clubs on the wrist; 2 is the bet for now.

The Table screen is **read‑mostly** — it's the "what's happening / whose turn"
glance. All *playing* happens on the Action screen.

---

## 3. Screen B — Action (where you play)

Your whole hand as a **grid of tokens**, **crown‑scrollable**, with a
context‑sensitive action bar pinned to the bottom.

```
        ┌───────────────────────────────────┐
        │   ♠     ♥     ♦     ♣     ♠         │   your hand, tokens in a grid.
        │   6     7     9    10     A         │   Digital Crown scrolls the grid;
        │                                    │   tap a token to (de)select it —
        │  [♦]   ♠     ♥     ♦     ♣         │   selected tokens lift + ring.
        │   Q     K     3     8     J         │   (here ♦Q is selected)
        │                                    │
        │  ═══════════════════════════════   │
        │        [  Cover  ] [  Pass  ]       │   bottom bar: only the moves the
        └───────────────────────────────────┘   kernel says are legal right now
```

### The bottom bar is the kernel's legal menu, nothing more

Same rule as the phone: **never hand‑compute a move** — read `humanLegal` and show
only what's offered for the current selection.

```
   SELECTION / ROLE                       BUTTONS SHOWN
   ─────────────────────────────────────────────────────────
   nothing selected, you're defending  →  [ Pickup ]         (take the cards)
   attack card(s) selected, attacking  →  [ Attack ]
   a card that beats an attack, def.   →  [ Cover ]
   a same-rank card, pass is legal     →  [ Pass ]
   selection that can do either        →  [ Cover ] [ Pass ] ← the "cover+pass" case
   attacker, table covered             →  [ Done ]           (бито)
```

The **"cover + pass"** case is real Durak: a single selected card can sometimes
*either* beat the attack *or* be thrown to pass it to the next seat — so we surface
both and let you choose. Tapping the button commits via the same `game.play(move)`
path the phone uses.

### Cover targeting (the one honest gap)

Covering needs a *target* attack. Two ways to resolve it without a table on this
screen:

1. **Unambiguous auto** — if the selected card legally covers exactly one attack,
   `Cover` just does it (the phone already has `findUnambiguousCover` logic).
2. **Ambiguous** — enter from the **Table** screen by tapping the specific attack
   first (that pre‑targets it), *or* after tapping `Cover` here, the centre shows
   the candidate attacks and the crown picks one. Lean on (1); most covers are
   unambiguous.

---

## 4. Interaction model — Crown + tap, no drag

Dragging a token around a 162 pt screen with a fingertip is hopeless, so drag is
out. The **Digital Crown** is the watch's super‑power — precise, detented,
eyes‑optional selection you can *feel*.

```
   Table screen                    Action screen
   ────────────                    ─────────────
   crown / swipe  → rotate         crown          → scroll the hand grid
                    through the    tap token      → select / deselect
                    centre battles bottom button  → commit the move
   tap ▶ play     → Action         swipe left / ◀ → back to Table
```

Reuse the phone's haptics: a soft tap when it becomes your turn, a rigid buzz on an
illegal attempt (`Haptics.reject`).

---

## 5. Why this is its own effort (and what it needs)

- **New**: two SwiftUI scenes for watchOS, the token card, the circle‑seat layout,
  the crown‑driven grid + battle rotator, a WatchConnectivity or direct‑Supabase
  path for online, and a "your turn" **complication** (the real wrist wedge —
  flick to see if it's on you instead of pulling out the phone).
- **Free**: the whole rules/engine/wire stack in `FoolishKit`. No new Durak logic,
  no new protocol.
- **Build order**: (1) read‑only Table screen off the `player_views` feed +
  complication → all glance value, zero interaction risk; (2) Pickup / Done swipes
  (most defensive turns are exactly these two); (3) full crown‑select play on the
  Action screen.

---

## 6. Footnote — the one thing it did teach the phone

Even as a separate effort, the watch's **circle of equally‑spaced seats** is the
right answer to "8 players on an iPhone SE": a ring has 360° of perimeter, a flat
top strip has one width and overflows at ~7 seats. The phone `TableView` already
moved to a top **arc**; the natural next step is the full ellipse. That's the only
cross‑pollination — the rest of the watch design stands on its own.
