# Pick 'Em Up

A shedding game for 2 to 8: match the top card by suit or number, play your
action cards, and get rid of your hand.

**Nothing is built.** `docs/UI.html` is the surface study and it comes first -
see `docs/MOTION_BEFORE_FLOW.md` in the repo root for why.

```
open pickemup/docs/UI.html
python3 shared/tools/check_ui_doc.py pickemup/docs/UI.html
```

**Read [LEGAL.md](LEGAL.md) before touching the art.** It is the UYES
post-mortem and the list of things that get a clone removed.

## THE NAME IS A PLACEHOLDER, AND THE REASON MATTERS

This is the shape of UNO, and **UNO is a Mattel trademark** - filed 1974,
registered 1975, and enforced recently enough that a clone called UYES was
pulled from Google Play by a DMCA takedown.

What that does and does not cover:

- **Not protectable: the mechanics.** Shedding, wilds, draw-twos, reverse, skip,
  and a penalty for failing to announce your last card. This is the Crazy Eights
  family and Crazy Eights is public domain.
- **Protected: the name, the card art, the four-colour-only suits, the oval, and
  the call-out word.** None of those are here and none of them can be.

So: **suits are shape AND colour** - circle/teal, triangle/amber, square/violet,
diamond/slate. That keeps us clear of the trade dress, and it is also the only
version a colourblind player can read, which is the better reason.

`Pick 'Em Up` is a working title and **it already collides**: a shedding game of
that name exists on TheGameCrafter, plus a *Pick 'Em Up Bitch*. Neither looks
registered and neither is Mattel, so the risk is common-law and small - but it is
the same genre. Search USPTO and decide before any store listing.

## The layout is foolish's, minus three things

Measured out of the shipped Swift, not approximated. `docs/UI.html` redraws it at
the same numbers, so the port is a subtraction:

- **Delete the flipped trump** at `(18, 28)` under the deck stock, and the bare
  60pt trump glyph that replaces it when both are gone. The deck well's ink
  footprint drops from **94 to 54** - that is the only size that changes.
- **Delete the top-right discard pile.** This game has one discard and it is the
  pile you match against, so it belongs in the middle. The freed corner is where
  the direction indicator goes.
- **Replace the battle grid** (62x84 slots, three across) with **one big pile**,
  centred in the board rect exactly as the grid was.

Everything else is kept at foolish's values: board rect inset 8/8/14/4, deck well
92 wide with 66x46 landscape backs leaning 1 left and 2 up per layer, seats on a
0.42 x 0.35 ellipse, badges ~97pt tall, hand a flat row of 72pt cards with 4pt
gaps and no overlap, pills 96x40 with their trailing edge 16 from the board edge,
two 40x40 squares mirroring them on the left.

**The role row becomes the stamp slot.** foolish reserves 40pt under every badge
for a shield or a sword. This game has no roles and needs somewhere to say LAST,
so it takes that 40pt and the badge keeps its exact height.

### Two things to re-ask, which the redraw surfaced

- **A shedding hand grows when you are losing.** Durak's shrinks. At ten cards
  foolish splits the hand into two rows and the box goes 80 -> 166 - which in the
  340pt collapsed drawer is over half the screen, and the pile has to live in what
  is left. That is exactly when you most need to see the table.
- **The thin-face rule fires constantly here.** Under 40pt wide a card drops its
  centre glyph and shows the rank only. Thirteen cards gives 23.2pt. Durak almost
  never reached it; this game will.

## Why it suits a transcript

- **Hidden hands are the masking kernel's home turf**, and the deck is
  seed-derived, so the state is small.
- **2 to 8, and genuinely good at 2** - which the three parked forks were not.
- **The settlement half is unusually large.** One card can skip a player, turn
  the table around and hand somebody two cards, so far more happens at Send than
  in foolish. The channel grid in the study is where that is worked out.
- **The conflict channel is real**, unlike Ultimate Tic-Tac-Toe: turn order is
  strict, but the player before you can undo and replay, and your staged card may
  no longer be legal.

## The fork to settle first

**Does the app announce one card left, or does somebody have to catch it?**

The call-out is the best moment in the physical game and it exists because a
human can forget. An app cannot forget, so announcing it deletes the mechanic
and leaves a notification.

The version that survives asynchrony: the app says nothing, and any other player
may tap **Caught you** until the next bubble seals. One bubble is the window. It
needs no clock, it cannot be lost to a slow phone, and it is the only
simultaneous-action mechanic in the design.
