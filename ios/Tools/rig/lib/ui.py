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


def bar_spans(a, s, y_pt):
    """The wooden buttons ON one bar row -> [(x_centre_pt, width_pt)], left to
    right.

    `wood_bars` answers WHERE a row of controls is by summing wood per row,
    which cannot tell one wide button from two side by side - and the lobby's
    Start/Leave row is exactly two, so every tap aimed at the middle of that
    row landed in the daylight BETWEEN them (FSpace.m) or, worse, on Leave when
    Start was meant. This reads the row itself: the runs of wood columns at that
    y, which is one span for a lone button and two for the pair.
    """
    row = wood(a)[int(y_pt * s), :]
    out = []
    for run in runs(np.nonzero(row)[0], 8 * s):
        if len(run) < 30 * s:                 # not a checkbox, not a speck
            continue
        out.append((int(np.mean(run) / s), int(len(run) / s)))
    return out


def checkbox(a, s):
    """The rules checkbox's plank -> (x_pt, y_pt), or None.

    A SMALL square of wood, which is why `wood_bars` cannot see it (it filters
    at 120pt of width to keep the settings gear out of the button list) and why
    the rig could not touch the one control on the lobby that animates. Found as
    the narrow wood run lowest on the screen that is NOT one of the corner icons
    - the gear and the rulebook sit at the very foot, so the search stops above
    them.
    """
    w = wood(a)
    icons = wood_icons(a, s)
    floor = (min(y for _, y in icons) - 20) * s if icons else a.shape[0]
    counts = w.sum(axis=1)
    best = None
    for run in runs(np.nonzero(counts > 8 * s)[0], 6 * s):
        if run[-1] >= floor:
            continue
        y = int(np.mean(run))
        cols = np.nonzero(w[y, :])[0]
        if cols.size == 0:
            continue
        span = cols.max() - cols.min()
        if span > 60 * s:                     # a button row, not a checkbox
            continue
        best = (int(np.mean(cols) / s), int(y / s))
    return best


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


def last_bubble(a, s):
    """The NEWEST Foolish bubble sitting in the transcript -> (x_pt, y_pt).

    Sending the first bubble of a game DISMISSES the drawer (the extension
    cannot be bound to a conversation it has not been opened from), so the only
    way back onto that game is to tap its bubble - and a rig that cannot do that
    can only ever film a lobby it created in the same breath, never one the
    thread actually holds.

    A sent bubble is a block of FELT in the chat. The presented drawer is felt
    too, so anything at or below `drawer_top` is excluded; what is left is the
    lowest tall run of table-coloured rows, and the bubble's own x centre is
    taken from the pixels in it (the bubble is inset from one side, so the
    screen's midpoint lands on the wrong thing at the edges).
    """
    cover = surface(a)
    h, w = a.shape[0], a.shape[1]
    top = drawer_top(a, s)
    limit = int((top - 8) * s) if top is not None else h
    band = cover[:limit, :].mean(axis=1) > 0.20
    best = None
    for run in runs(np.nonzero(band)[0], 4 * s):
        if len(run) < 40 * s:                 # a bubble is tall; a tapback is not
            continue
        best = run
    if best is None:
        return None
    y = int(np.mean(best))
    cols = np.nonzero(cover[y, :])[0]
    if cols.size == 0:
        return None
    return int(np.mean(cols) / s), int(y / s)


def hand_band(a, s):
    """(top, bottom) pixel rows of the hand strip - the LOWEST band of
    card-carrying rows inside the drawer.

    Anchored to the drawer, not to a fraction of the screen. The old form
    scanned a fixed `0.88 * height` band, which was true of one phone with one
    table and became silently wrong when either changed.

    It reads its bands from `card_bands`, and that is the fix rather than a
    tidy-up: this used to run its own full-width `> 0.30` test, and a row's
    card fraction scales with HOW MANY cards are in it. A three-card hand in a
    COMPACT drawer scores under 0.30 and vanished, so the lowest band that
    survived was the table's battle row - and `hand_y` then answered with the
    TABLE. Every select, hint and drag aimed a hundred points too high, on the
    one presentation the App Store frames are shot in."""
    bands = card_bands(a, s)
    if not bands:
        return None
    top_px, bot_px = bands[-1]
    # A hand is ONE card tall. When the lowest attack on the table sits within
    # a couple of points of the hand the two runs merge, and a merged band
    # reported the attack as part of the hand - so the rig never offered it as
    # a cover target and every defender board came back "no legal move".
    if bot_px - top_px > 85 * s:
        top_px = bot_px - 78 * s
    return top_px, bot_px


def hand_cards(a, s):
    """Face-up hand cards -> [x_pt] centres, left to right."""
    band = hand_band(a, s)
    if band is None:
        return []
    strip = card(a)[band[0]:band[1] + 1, :]
    cols = np.nonzero(strip.mean(axis=0) > 0.35)[0]
    w = a.shape[1]
    # Gap 3pt, not 6: cards in a fanned hand sit about 4pt apart, so a 6pt
    # tolerance merged a whole four-card hand into ONE run and the rig saw a
    # single card where there were four.
    out = [int(np.mean(gp) / s) for gp in runs(cols, 3 * s) if len(gp) > 6 * s]
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


def spam_banner(a, s):
    """The y (points) of Apple's unknown-sender banner, or None.

    The simulator has no iMessage account, so every thread is unverified and
    Messages glues a banner under the newest message: two lines of grey text
    and a blue "Report Spam" pill. It cannot be switched off - both
    `sForceUnknownFilteringCompleted` and `FilterUnknownSenders` leave it
    exactly where it was - and while a drawer is presented it is not in the
    accessibility tree either, because the drawer hides the whole transcript
    from it. So it is found by colour, like our own board.

    Defined as ANY content between the bottom of the newest bubble and the
    compose bar, rather than as the blue pill. Looking for the pill alone was
    wrong in the one way that matters for a photograph: the pill scrolls behind
    the compose bar a full line before the grey text above it does, so the
    scroll stopped with "may be spam" still on screen and reported success."""
    h, w = a.shape[0], a.shape[1]
    top = drawer_top(a, s)
    # Stop short of the compose bar, which is itself a wide light block sitting
    # just above the drawer and would otherwise read as the newest bubble.
    limit = (top - 60) * s if top is not None else h - 120 * s
    if limit <= 300:
        return None
    cov = (a.max(axis=2) > 24)[:, int(w * 0.10):int(w * 0.90)].mean(axis=1)
    wide = np.nonzero(cov[300:limit] > 0.5)[0]
    bubble_bottom = (300 + int(wide[-1])) if len(wide) else 300
    hi, lo = a.max(axis=2), a.min(axis=2)
    grey = (hi - lo < 30) & (hi > 110) & (hi < 215)
    counts = grey[:, int(w * 0.10):int(w * 0.90)].sum(axis=1)
    rows = np.nonzero(counts[bubble_bottom + 1:limit] > w * 0.05)[0]
    if not len(rows):
        return None
    return int((bubble_bottom + 1 + int(rows[0])) / s)


def wood_icons(a, s):
    """The small wooden squares on the board's control row -> [(x_pt, y_pt)].

    `wood_bars` only reports runs at least 120pt wide, which is right for a
    button but excludes the gear and the book - and asking it for "the lowest
    bar" then returned whatever wide button happened to be lower on screen,
    so "open Settings" tapped Add player instead and produced two frames of
    the wrong surface entirely."""
    m = wood(a)
    counts = m.sum(axis=1)
    rows = [r for r in runs(np.nonzero(counts > 14 * s)[0], 10 * s)
            if len(r) >= 14 * s]
    if not rows:
        return []
    band = rows[-1]                       # the control row is the lowest one
    cols = np.nonzero(m[band[0]:band[-1] + 1, :].mean(axis=0) > 0.5)[0]
    y = int((band[0] + band[-1]) / 2 / s)
    out = []
    for gp in runs(cols, 6 * s):
        wpt = len(gp) / s
        if 16 <= wpt <= 70:               # a square, not a full-width plank
            out.append((int(np.mean(gp) / s), y))
    return out


def card_bands(a, s):
    """Every run of card-carrying rows inside the drawer, top to bottom.

    The last one is the hand; the ones above it are the battle rows.

    Measured over the CENTRAL 56% of the width, and at a low threshold. Both
    matter. Full width put the seat badges, the deck and the trump into the
    same profile as the table, which forced the threshold up to 0.30 - and at
    0.30 a LONE uncovered attack, one card across the whole board, scores about
    0.16 and is invisible. The rig then never offered the one card the defender
    had to cover and reported "no legal move" on a board with an obvious one.
    Down the middle, that same attack scores 0.23 and the chrome scores under
    0.20."""
    top = drawer_top(a, s)
    if top is None:
        return []
    lo, h, w = top * s, a.shape[0], a.shape[1]
    m = card(a)[lo:, int(w * 0.22):int(w * 0.78)].mean(axis=1)
    return [(lo + int(r[0]), lo + int(r[-1]))
            for r in runs(np.nonzero(m > 0.22)[0], 8 * s)
            if len(r) >= 10 * s and lo + int(r[-1]) < h - 10 * s]


def table_cards(a, s):
    """Cards on the TABLE -> [(x_pt, y_pt)], top row first.

    Everything above the hand strip. Wanted because a DEFENDER's move is not a
    button: a card is selected and then dropped on the attack it covers, so the
    rig has to know where the attacks are.

    Bands are re-sliced into CARD-HEIGHT rows before the columns are read. Two
    battle rows a couple of points apart merge into one run, and reading the
    columns of a merged run reports every card at the merged centre - a y that
    is on neither row, so every tap missed."""
    hand = hand_band(a, s)
    if hand is None:
        return []
    w, row = a.shape[1], 74 * s
    m = card(a)
    out = []
    for t, b in card_bands(a, s):
        b = min(b, hand[0] - 1)
        if b - t < 20 * s:
            continue
        n = max(1, round((b - t) / row))
        step = (b - t) / n
        for k in range(n):
            t0, b0 = int(t + k * step), int(t + (k + 1) * step)
            cols = np.nonzero(m[t0:b0 + 1, :].mean(axis=0) > 0.30)[0]
            y = int((t0 + b0) / 2 / s)
            for gp in runs(cols, 3 * s):
                if len(gp) < 8 * s:
                    continue
                x = int(np.mean(gp) / s)
                if 12 < x < int(w / s) - 12:
                    out.append((x, y))
    return out


def last_message(a, s):
    """(x_pt, y_pt) of the NEWEST Foolish message in the transcript, either
    side, located by its app icon.

    A message collapses into a caption line only when the NEXT send shares its
    MSSession, and a send shares it only if that message is SELECTED. Selecting
    just the INCOMING ones collapses only Kate's - our own stay full bubbles,
    and the transcript ends up with several game cards in it when a real thread
    has exactly one. So tap the newest message whoever sent it: ours sit on the
    right, hers on the left."""
    top = drawer_top(a, s)
    h, w = a.shape[0], a.shape[1]
    hi = (top * s) if top is not None else h
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    icon = (r > 110) & (r > g * 1.9) & (r > b * 1.9)
    band = icon[:hi, :]
    rows = np.nonzero(band.sum(axis=1) > 6 * s)[0]
    if not len(rows):
        return None
    run = runs(rows, 6 * s)[-1]
    cols = np.nonzero(band[run[0]:run[-1] + 1, :].sum(axis=0) > 0)[0]
    if not len(cols):
        return None
    return (int(cols.mean() / s), int((run[0] + run[-1]) / 2 / s))


def last_incoming(a, s):
    """(x_pt, y_pt) of the NEWEST incoming Foolish message in the transcript,
    or None - located by its app icon, which is a small strongly-red mark on
    the LEFT of the conversation.

    Wanted because an incoming message only collapses into a caption line when
    the NEXT send shares its MSSession, and a send only shares it if that
    message is SELECTED. Tapping a fixed point hit whichever caption line
    happened to be there, so the newest one stayed a full pill - which is not
    what a real thread looks like: the owner's is game-text lines and then one
    bubble, the sender's own."""
    top = drawer_top(a, s)
    h, w = a.shape[0], a.shape[1]
    lo = 0 if top is None else 0
    hi = (top * s) if top is not None else h
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    icon = (r > 110) & (r > g * 1.9) & (r > b * 1.9)
    left = icon[:hi, :int(w * 0.18)]
    rows = np.nonzero(left.sum(axis=1) > 6 * s)[0]
    if not len(rows):
        return None
    band = runs(rows, 6 * s)[-1]           # the lowest icon is the newest
    cols = np.nonzero(left[band[0]:band[-1] + 1, :].sum(axis=0) > 0)[0]
    if not len(cols):
        return None
    return (int(cols.mean() / s), int((band[0] + band[-1]) / 2 / s))


if __name__ == "__main__":
    a, s = grab()
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    if what in ("bars", "all"):
        print("BARS", wood_bars(a, s))
    if what in ("icons", "all"):
        print("ICONS", wood_icons(a, s))
    if what in ("box", "all"):
        print("BOX", checkbox(a, s))
    if what in ("bubble", "all"):
        print("BUBBLE", last_bubble(a, s))
    if what == "span":
        print("SPAN", bar_spans(a, s, int(sys.argv[2])))
    if what in ("cards", "all"):
        print("CARDS", hand_cards(a, s))
    if what in ("table", "all"):
        print("TABLE", table_cards(a, s))
    if what in ("hand_y", "all"):
        y = hand_y(a, s)
        print("HAND_Y", y if y is not None else -1)
    if what in ("top", "all"):
        print("TOP", drawer_top(a, s))
    if what in ("incoming", "all"):
        print("INCOMING", last_incoming(a, s))
    if what in ("spam", "all"):
        print("SPAM", spam_banner(a, s))
    if what in ("lastmsg", "all"):
        print("LASTMSG", last_message(a, s))
    if what in ("staged", "all"):
        print("STAGED", staged_box(a, s))
    if what in ("onboarding", "all"):
        y = onboarding(a, s)
        print("ONBOARDING", y if y is not None else -1)
