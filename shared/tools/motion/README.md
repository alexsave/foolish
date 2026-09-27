# motion - film a take, find the ruler, score the ride

One palette, one finder, one scorer, for every product in this repo.
The ruler's colours and sizes live once in `shared/c/motion_ruler/motion_ruler.h`.
Swift paints from it through the `CMotionRuler` module, and `motion.c` looks for exactly those colours, so what is drawn and what is found cannot drift apart.

## What a product paints (DEBUG only, behind `dev.ruler`)

- A RED bar on the moving container's top edge and a GREEN bar on its bottom edge, `MR_EDGE_PT` thick.
- A banded strip down the left edge, `MR_STRIP_PT` wide, counted from the red bar: `MR_BAND_PT` bands, red first, yellow every tenth, cyan and magenta between (`mr_band_ink`).
- A clock under the red bar, `MR_STRIP_PT + MR_CLOCK_GAP_PT` from the left: `MR_CLOCK_BITS` white/black cells of `MR_CLOCK_CELL_PT`, milliseconds modulo 16384, most significant first.
  A frame whose clock repeats while the geometry moved is a frame the app did not draw.
- A `MR_SIDE_PT` square on every element that moves, in one of the eight square inks (`mr_square_ink`): magenta, cyan, yellow, orange, blue, violet, lime, pink.
  Red and green are the bars' and never a square.
  When an ink appears more than once in a frame, the finder splits it by quadrant around the magenta square.

Two painters use this palette today: `shared/swift/MotionRuler.swift` (UIKit and Core Animation layers) and a product's own SwiftUI ruler that reads the same constants from `CMotionRuler`.
A new painter must use only these inks and this geometry, or the finder will not see it.

## Filming a take

On a simulator, with the product's rig (`rig.sh` sourced with the product's `rig.env`):

```bash
rig.sh ruler on                 # touches dev.ruler in the App Group
rig.sh film NAME [SECONDS] ...  # records every composited frame to take.mp4
```

On a USB iPhone (a DEBUG build, so the ruler is compiled in):

```bash
shared/tools/devcap/devcap.sh <product>/ship.env install
shared/tools/devcap/devcap.sh <product>/ship.env ruler on
shared/tools/devcap/devcap.sh <product>/ship.env film NAME   # records until ^C, then tracks, scores and charts
```

Always film at normal speed: the recorder keeps every composited frame with its real presentation time.

## Scoring a take

```bash
make -C shared/tools/motion                                    # build/motion
shared/tools/motion/motion_take.sh take.mp4 take.tbl           # every frame through `motion find`
shared/tools/motion/build/motion score --span 1.5 take.tbl     # every mark against its anchor
python3 shared/rig/lib/motionplot.py take.tbl chart.png --span 1.5 --title NAME
```

- `take.tbl` is a fixed-layout text table: one row per composited frame with its time, clock, red and green bar, and every mark's x and y in points.
- `motion score` reports, per mark, how far its offset from its anchor moved: the largest one-frame step, snaps above `--snap`, and a jerk figure against the host spring's floor.
  Several takes on one command line are averaged.
- The default anchors (`mt_default_opts` in `motion.c`) suit a board with a header on the red bar and doors on the green one.
  A product whose squares mean something else passes `--anchor MARK=red|green|mid|none` for every ink it paints, for example `--anchor cyan=mid --anchor lime=mid --anchor blue=green`.
- `--bottom first` scores against the screen's bottom instead of the painted green bar, for a drawer whose bottom is the screen's.
- `motionplot.py` draws four panels against milliseconds from the first move: every square's y with the bars, every square's x, each square's y minus its anchor's (a mark that rides its anchor is a flat line), and the board's width and height from its corner squares beside the drawer's height.
- `motion pace` measures how often a box's pixels changed; `motion_grid.sh` reads a take with no ruler at all (a Release build's) off the board's own grid lines.

Run `make -C shared/tools/motion test` after touching `motion.c` or the palette.
