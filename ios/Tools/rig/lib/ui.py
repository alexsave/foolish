"""Locate Foolish UI elements from a simulator screenshot, in device points.

Our own extension is a separate process and reports NOTHING to the
accessibility tree - `idb ui describe-all` sees Messages' chrome and, where a
drawer is presented, a single zero-height "Activate to dismiss pop-up window".
So everything inside the board is found by COLOUR. That is not a fallback: the
layout shifts between compact and expanded, between player counts, between
locales and between devices, and a hard-coded y is what kept sending taps into
the Settings gear (which silently switches the app's language).

Colour tests are appearance-agnostic on purpose. The table surface is strongly
tinted in both light and dark mode, while a card face is near-ACHROMATIC (white
in light mode, near-black in dark). Testing "is this pixel grey" rather than "is
this pixel dark" is what lets one rig drive a light-mode iPhone SE and a
dark-mode iPhone 17 Pro Max.
"""
import os
import subprocess
import sys

import numpy as np
from PIL import Image

SIM = os.environ.get("FOOLISH_SIM", "")
SHOT = os.path.join(os.environ.get("FOOLISH_WORK", "/tmp/foolishrig"), "_ui.png")


def grab():
    os.makedirs(os.path.dirname(SHOT), exist_ok=True)
    subprocess.run(["xcrun", "simctl", "io", SIM, "screenshot", SHOT],
                   capture_output=True)
    a = np.asarray(Image.open(SHOT).convert("RGB")).astype(int)
    # Points, not pixels: idb injects touches in points. Every 3x iPhone is at
    # least 1000px wide, every 2x one is under it.
    return a, (3 if a.shape[1] >= 1000 else 2)


def wood(a):
    """Wooden button pixels: warm, mid-to-bright, and clearly not the table."""
    # `r > 45`, not the 120 this started at: the plank is (62, 18, 0) in dark
    # appearance and (171, 120, 84) in light, and a threshold picked in light
    # mode finds NO BUTTONS AT ALL in dark - which reads as "the lobby has no
    # controls" rather than as "the test was calibrated on one appearance".
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    return (r > 45) & (r > b * 1.7) & (g > b) & (g < r * 0.85)


def card(a):
    """Card-face pixels: near-achromatic, either end of the luminance range."""
    hi, lo = a.max(axis=2), a.min(axis=2)
    return (hi - lo < 34) & ((hi < 80) | (lo > 150))


def surface(a):
    """Table-surface pixels, whichever table is on: the red plaid wool is warm
    (red clearly over blue) and the felt is green (green clearly over both).

    Testing for BOTH is not belt-and-braces - it is the difference between a
    rig that works and one that silently reports nothing. This used to test
    warmth alone, which was correct while wool was the default and became wrong
    the day felt became it; a `None` from `drawer_top` then reads as "no
    drawer" rather than as "the test is looking for the wrong colour"."""
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    wool = (r - b) > 25
    # `g < 150` is load-bearing, and it is there because of the transcript:
    # an SMS bubble is (49, 213, 90) and the felt is (19, 55, 39). Both are
    # "green over red and blue", so without a brightness ceiling every text
    # bubble in the chat reads as table, and `drawer_top` answers with the top
    # of the CONVERSATION instead of the top of the drawer.
    felt = (g > r + 10) & (g > b + 10) & (g < 150)
    return wool | felt


def runs(idx, gap):
    """Split a sorted index array into contiguous runs."""
    if not len(idx):
        return []
    return np.split(idx, np.nonzero(np.diff(idx) > gap)[0] + 1)


def wood_bars(a, s, min_width_pt=120):
    """Wide wooden buttons -> [(y_pt, width_pt)], top to bottom."""
    counts = wood(a).sum(axis=1)
    out = []
    for run in runs(np.nonzero(counts > min_width_pt * s)[0], 12 * s):
        if len(run) < 20 * s // 3:            # ignore thin strips
            continue
        out.append((int(np.mean(run) / s), int(counts[run].max() / s)))
    return out


def drawer_top(a, s):
    """Top edge of the presented drawer (points), or None if none is up.

    Defined as the row after the LAST LONG GAP in table-surface coverage.
    Three earlier definitions were wrong, each plausibly:

      * the topmost surface row - which a STAGED Foolish bubble sitting up in
        the transcript satisfies, hundreds of points too high;
      * the contiguous surface run reaching the bottom - which an EXPANDED
        board breaks, because a row of cards is hundreds of pixels of
        not-surface and the run restarts under it (486 for a real edge of 68);
      * a suffix MEAN over a threshold - which crosses that threshold well up
        inside the transcript, because the drawer's own high coverage drags the
        average of everything above it up with it (281 for a real edge of 590,
        and the expand drag then scrolled the chat instead).

    A gap is what actually separates the two: above the drawer there is a long
    stretch of screen with no table in it at all, and inside the drawer the
    longest table-free stretch is one row of cards."""
    h, w = a.shape[0], a.shape[1]
    band = surface(a)[:, int(w * 0.10):int(w * 0.90)].mean(axis=1) > 0.15
    gaps = runs(np.nonzero(~band)[0], 1)
    # A gap has to be taller than a row of cards to count as "not the drawer".
    long_gaps = [g for g in gaps if len(g) >= 26 * s]
    top = 0 if not long_gaps else int(long_gaps[-1][-1]) + 1
    if top >= h - 40 * s:            # the gap runs to the bottom: no drawer
        return None
    return int(top / s)


def hand_band(a, s):
    """(top, bottom) pixel rows of the hand strip - the lowest TALL run of
    card-coloured rows inside the drawer.

    Anchored to the drawer, not to a fraction of the screen. The old form
    scanned a fixed `0.88 * height` band, which was true of one phone with one
    table and became silently wrong when either changed."""
    top = drawer_top(a, s)
    if top is None:
        return None
    lo = top * s
    # 0.30 separates a row of CARDS from a row that merely contains a suit
    # glyph or a seat badge: the table's battle rows and the hand both clear
    # it, everything else on the board sits under 0.25.
    m = card(a)[lo:, :].mean(axis=1)
    h = a.shape[0]
    bands = [r for r in runs(np.nonzero(m > 0.30)[0], 8 * s)
             if len(r) >= 10 * s and lo + int(r[-1]) < h - 10 * s]
    # The last TALL one that does NOT run to the bottom edge. Both extra
    # conditions were paid for: the drawer's own bottom edge and the home
    # indicator clear the colour test for ~30pt, and taking that as the hand
    # put every card tap 70pt below the cards.
    if not bands:
        return None
    b = bands[-1]
    return lo + int(b[0]), lo + int(b[-1])


def hand_cards(a, s):
    """Face-up hand cards -> [x_pt] centres, left to right."""
    band = hand_band(a, s)
    if band is None:
        return []
    strip = card(a)[band[0]:band[1] + 1, :]
    cols = np.nonzero(strip.mean(axis=0) > 0.35)[0]
    w = a.shape[1]
    out = [int(np.mean(gp) / s) for gp in runs(cols, 6 * s) if len(gp) > 8 * s]
    # Drop the drawer's own left and right edges, which are card-pale for a few
    # columns and would otherwise be tapped as if they were cards.
    return [x for x in out if 12 < x < int(w / s) - 12]


def hand_y(a, s):
    """A y (points) that lands ON a hand card rather than on its top edge."""
    band = hand_band(a, s)
    return None if band is None else int((band[0] + band[1]) / 2 / s)


def staged_box(a, s):
    """Bounding box (points) of a STAGED Foolish bubble sitting in the compose
    field, or None.

    It is the same table surface as the drawer, so the two are told apart by
    where they end: the drawer runs to the bottom of the screen and a staged
    bubble does not. Wanted for its close button, which is inset from the
    bubble's top-right corner - a staged bubble left in the field puts an
    unsent draft into every later frame, and one that belongs to a different
    game than the board underneath makes the frame a lie."""
    h, w = a.shape[0], a.shape[1]
    m = surface(a)
    rows = np.nonzero(m.mean(axis=1) > 0.25)[0]
    if not len(rows):
        return None
    for run in runs(rows, 6 * s):
        if run[-1] >= h - 12 * s:      # that one is the drawer
            continue
        # A staged bubble is a BLOCK. Without a size floor the tallest thing
        # that passed was the green battery glyph in the status bar (7pt), and
        # "clear the staged bubble" then tapped Control Center open.
        if len(run) < 60 * s:
            continue
        cols = np.nonzero(m[run[0]:run[-1] + 1, :].mean(axis=0) > 0.25)[0]
        if len(cols) < 40 * s:
            continue
        return (int(cols[0] / s), int(run[0] / s),
                int(cols[-1] / s), int(run[-1] / s))
    return None


def onboarding(a, s):
    """The y (points) of the blue action pill on one of Apple's first-run
    Messages sheets, or None if no sheet is up.

    Both sheets ("Shared with You", "Apple Intelligence in Messages") eat the
    first tap of a run if they are still there, and their buttons are at
    DIFFERENT heights - "OK" sits about 87% down, "Continue" about 93%. A rig
    that tapped a fixed 87% dismissed the first sheet and, on the second,
    landed on "Edit in Settings" instead: the run then walked into the
    Settings app and reported "could not open conversation" eleven times.
    So the pill is measured, not assumed. Our own buttons are wood, never a
    wide blue pill low on the screen, so there is nothing else to hit."""
    h, w = a.shape[0], a.shape[1]
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    blue = (b > 150) & (b > r + 60) & (b > g + 40)
    lo = int(h * 0.72)
    counts = blue[lo:, :].sum(axis=1)
    rows = np.nonzero(counts > w * 0.5)[0]
    if not len(rows):
        return None
    band = runs(rows, 8 * s)[0]
    return int((lo + (band[0] + band[-1]) / 2) / s)


if __name__ == "__main__":
    a, s = grab()
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    if what in ("bars", "all"):
        print("BARS", wood_bars(a, s))
    if what in ("cards", "all"):
        print("CARDS", hand_cards(a, s))
    if what in ("hand_y", "all"):
        y = hand_y(a, s)
        print("HAND_Y", y if y is not None else -1)
    if what in ("top", "all"):
        print("TOP", drawer_top(a, s))
    if what in ("staged", "all"):
        print("STAGED", staged_box(a, s))
    if what in ("onboarding", "all"):
        y = onboarding(a, s)
        print("ONBOARDING", y if y is not None else -1)
