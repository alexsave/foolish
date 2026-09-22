#!/usr/bin/env python3
"""Plot the win-probability strip cnitro_winprob writes.

    make -C c winprob
    ./c/build/cnitro_winprob --code=<link> --threads=8 --bin=strip.bin
    python3 c/tools/winprob/plot_winprob.py strip.bin out.png

The input is the packed file, read through winprob_bin.py - the same layout
c/src/winprob.h documents and winprob_packed writes. Two panels over one x
axis (never two y scales on one panel):

  TRUTH   the position as it really was - every hidden hand and the real
          remaining stock are in the rebuilt board, so a playout from it is
          the true position handed to bots. Exactly one seat is the fool in
          each playout, so these curves are commensurable.
  BELIEF  the same number computed inside each seat's own information set:
          what that seat could honestly know at that moment. Eight seats hold
          eight different pictures of one board, and the gap between a seat's
          two curves is what it could not see.

Colours are the dataviz reference palette's dark categorical slots, assigned
to seats in fixed slot order and never cycled. Identity is never colour-alone:
every line is direct-labelled in the right margin, in finishing order.
"""

import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, str(Path(__file__).resolve().parent))
from winprob_bin import read          # noqa: E402

# dataviz reference palette, dark steps (references/palette.md)
SERIES = ["#3987e5", "#d95926", "#199e70", "#c98500",
          "#d55181", "#008300", "#9085e9", "#e66767"]
SURFACE, PLANE = "#1a1a19", "#0d0d0d"
INK, INK2, MUTED = "#ffffff", "#c3c2b7", "#898781"
GRID, BASELINE = "#2c2c2a", "#383835"
CRITICAL = "#d03b3b"
ORD = {1: "1st", 2: "2nd", 3: "3rd", 4: "4th", 5: "5th",
       6: "6th", 7: "7th", 8: "8th", 0: "-"}


def smooth(ys, k=3):
    """A k-wide centred mean - the sampling noise of a finite world count is
    not signal, and the neighbouring positions are the same game."""
    if k <= 1 or len(ys) < k:
        return ys
    out = []
    for i in range(len(ys)):
        lo, hi = max(0, i - k // 2), min(len(ys), i + k // 2 + 1)
        out.append(sum(ys[lo:hi]) / (hi - lo))
    return out


def place_labels(ys, gap):
    """Nudge label y's apart, keeping their order, so eight names at the right
    edge stay readable when the lines they belong to end at the same value."""
    order = sorted(range(len(ys)), key=lambda i: -ys[i])
    out = list(ys)
    for k in range(1, len(order)):
        a, b = order[k - 1], order[k]
        if out[a] - out[b] < gap:
            out[b] = out[a] - gap
    low = min(out[i] for i in order)
    if low < 0:
        for i in order:
            out[i] -= low
    return out


def exit_index(strip, seat):
    """The first step at which the seat is out, or None."""
    for st in strip.steps:
        if st.hand[seat] is None:
            return st.index
    return None


def main(binpath, png, engine="the bot"):
    strip = read(binpath)
    n_pos = len(strip.steps)
    np_ = strip.n_players
    fool = strip.fool
    names = [st.name or f"seat {st.index}" for st in strip.seats]
    place = {st.index: (st.place or 0) for st in strip.seats}
    exits = {s: exit_index(strip, s) for s in range(np_)}

    have_belief = any(any(p is not None for p in st.belief_fool) for st in strip.steps)
    fig, axes = plt.subplots(2 if have_belief else 1, 1, figsize=(15, 9.0),
                             sharex=True, height_ratios=[3, 2] if have_belief else None)
    axes = list(axes) if have_belief else [axes]
    fig.patch.set_facecolor(PLANE)

    panels = [("truth", axes[0],
               "The table, as it really stood",
               "every hand and the stock are known here - the seats' odds add to one"),
              ("belief", axes[1] if have_belief else None,
               "The table, as each seat could see it",
               "public information only: own hand, table, discard, flip, watched pickups")]

    for view, ax, title, sub in panels:
        if ax is None:
            continue
        ax.set_facecolor(SURFACE)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            ax.spines[side].set_color(BASELINE)
        ax.tick_params(colors=MUTED, labelsize=9)
        ax.grid(True, axis="y", color=GRID, lw=0.8)
        ax.set_axisbelow(True)
        ax.set_ylim(-3, 103)
        ax.set_yticks([0, 25, 50, 75, 100])
        ax.set_yticklabels(["0%", "25", "50", "75", "100%"])
        ax.set_xlim(-1, n_pos + 1.5)
        ax.set_title(title, color=INK, fontsize=15, loc="left", pad=30, weight="bold")
        ax.text(0, 1.008, sub, transform=ax.transAxes, color=MUTED, fontsize=9.5,
                va="bottom", ha="left")

        ends = {}
        for seat in range(np_):
            pts = strip.win(view, seat)
            if not pts:
                continue
            xs = [x for x, _ in pts]
            ys = [y for _, y in pts]
            ys = smooth(ys, 3 if view == "truth" else 5)
            colour = SERIES[seat % len(SERIES)]
            lead = seat == fool or seat == 0
            ax.plot(xs, ys, color=colour, lw=2.6 if lead else 1.7,
                    alpha=1.0 if lead else 0.78, solid_capstyle="round",
                    zorder=5 if lead else 3)
            # The moment the seat left the game, marked on its own line.
            gone = exits[seat]
            if gone is not None and gone <= xs[-1]:
                gy = ys[min(range(len(xs)), key=lambda i: abs(xs[i] - gone))]
                ax.plot([gone], [gy], "o", ms=6.5, color=colour, mec=SURFACE, mew=2, zorder=6)
            # A seat that has left the game is safe, and the belief panel
            # stops measuring it (there is nothing left for it to decide).
            # Carry it to the right edge dashed, at the certainty it earned.
            tail_x, tail_y = xs[-1], ys[-1]
            if gone is not None and xs[-1] < n_pos - 1:
                ax.plot([xs[-1], gone, n_pos - 1], [ys[-1], 100.0, 100.0],
                        color=colour, lw=1.4, alpha=0.5, ls=(0, (3, 3)), zorder=2)
                tail_x, tail_y = n_pos - 1, 100.0
            ends[seat] = (tail_x, tail_y)

        # One ordered label column in the right margin: it is the legend AND
        # the direct labels, so identity never rests on colour alone, and the
        # reading order is the order the seats finished in.
        seats = sorted(ends, key=lambda s: place.get(s, 99))
        lys = place_labels([ends[s][1] for s in seats], gap=7.0)
        for s, ly in zip(seats, lys):
            ex, ey = ends[s]
            colour = SERIES[s % len(SERIES)]
            ax.plot([ex, n_pos + 1.5], [ey, ly], color=colour, lw=0.9, alpha=0.45,
                    zorder=2, clip_on=False)
            ax.annotate(f"{ORD[place.get(s, 0)]}  {names[s]}", xy=(n_pos + 2.5, ly),
                        xytext=(0, 0), textcoords="offset points",
                        color=CRITICAL if s == fool else colour,
                        fontsize=10, va="center",
                        weight="bold" if s in (0, fool) else "normal",
                        zorder=7, annotation_clip=False)

    # The point of no return: the last position at which the seat that lost
    # was still even money, and the move that ended that. One annotation, on
    # the panel that can carry it - never a label per point.
    pts = strip.win("truth", fool)
    xs = [x for x, _ in pts]
    ys = smooth([y for _, y in pts], 3)
    even = [i for i in range(len(xs)) if ys[i] >= 50.0]
    if even and even[-1] + 1 < len(xs):
        i = even[-1] + 1
        di, dy = xs[i], ys[i]
        mv = strip.steps[di]
        who = names[mv.seat] if mv.seat is not None else "the deal"
        right = di > 0.62 * n_pos
        axes[0].annotate(
            f"move {mv.move}  ·  {who} plays {mv.label()}\n"
            f"{names[fool]}'s last even-money position",
            xy=(di, dy), xytext=(-16 if right else 16, 38),
            textcoords="offset points", color=INK2, fontsize=10,
            ha="right" if right else "left", zorder=8,
            arrowprops=dict(arrowstyle="-", color=MUTED, lw=1, shrinkA=0, shrinkB=6))

    # The eliminations, as one recessive rule per panel.
    for ax in axes:
        for seat, x in exits.items():
            if x is None:
                continue
            ax.axvline(x, color=BASELINE, lw=0.8, ls=(0, (2, 3)), zorder=1)

    ax = axes[-1]
    ax.set_xlabel("move of the game", color=MUTED, fontsize=10, labelpad=8)

    title = (f"Chance of not being the fool  ·  {np_} seats, trump {strip.trump}, "
             f"{names[fool]} lost")
    fig.suptitle(title, color=INK, fontsize=19, x=0.045, ha="left", y=0.982, weight="bold")
    fig.text(0.045, 0.938,
             f"every position played to the end by {engine} at every seat  ·  "
             f"{strip.worlds} true worlds and {strip.belief_worlds} belief worlds "
             f"per seat per move  ·  {strip.playouts:,} playouts",
             color=MUTED, fontsize=10.5, ha="left")

    if strip.belief_failed:
        print("note: a seat's belief broke conservation somewhere; "
              "those seats carry no point there")

    fig.tight_layout(rect=(0.03, 0.02, 0.875, 0.925))
    fig.savefig(png, dpi=160, facecolor=PLANE)
    print(f"wrote {png}  ({n_pos} positions, {np_} seats)")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    # The engine is a roster INDEX in the file; the roster itself is C, so the
    # name is passed in rather than guessed here.
    main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "the bot")
