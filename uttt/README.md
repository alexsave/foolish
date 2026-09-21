# Ultimate Tic-Tac-Toe

Nine boards in a 3x3 grid. Your move inside a small board decides which small
board your opponent must play in next. Win three small boards in a line.

**Nothing is built.** `docs/UI.html` is the surface study and it comes first, on
purpose - see `docs/MOTION_BEFORE_FLOW.md` in the repo root for why.

```
open uttt/docs/UI.html
python3 shared/tools/check_ui_doc.py uttt/docs/UI.html
```

## Where it stands on rights

Clean. No inventor is recorded for the game - not even on its Wikipedia page -
and it has no patent or trademark I could find. It spread as a folk game on the
internet.

**One thing to stay away from:** `Tic-Tac-Ku`, by Mark Asperheim and Cris Van
Oosterum, is a commercial variant where a player wins by taking **five** small
boards rather than three in a line. Use the three-in-a-line meta win, which is
the version everybody already plays.

The name "Ultimate Tic-Tac-Toe" is descriptive and used generically, so it is
low risk - but a distinct app name would still be better branding.

## Why it suits a transcript

- **~25 bytes of state.** 81 cells at 2 bits, plus the forced board, the side to
  move, and nine board verdicts. The body budget is roughly 625, so every bubble
  carries the whole game with room spare - no chain to walk, nothing a late tap
  cannot rebuild.
- **40 to 60 moves**, which is one bubble each.
- **The caption writes itself.** Messages gives one truncating line, and this
  game's line is never "Alex moved" - it is *"Alex sent you to the bottom-middle
  board,"* which is the entire mechanic.
- **No conflict channel.** Strict alternation means two moves can never race.

## The open question

The board tops out near 354px at 390pt wide, which leaves ~280pt of empty paper
in the expanded view. Either that space earns its keep - the move trail in the
study is the only candidate worth defending - or **this game never expands at
all**, since the board is already legible at 214px in the collapsed drawer. If
that is the answer, the auto-collapse and every anchor decision above it stop
existing.
