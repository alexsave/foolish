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
