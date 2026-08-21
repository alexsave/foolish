"""Locate Foolish UI elements from a simulator screenshot (device points).

Everything is found by colour, not by hard-coded coordinates, because the
layout shifts between compact/expanded, player counts, locales and DEVICES -
which is what kept sending taps into the Settings gear.

Colour tests are appearance-agnostic on purpose: the board's wool is warm and
saturated in both light and dark mode, while a card face is near-achromatic
(white in light mode, near-black in dark). Testing "is this pixel grey" rather
than "is this pixel dark" is what lets the same rig drive a light-mode iPhone
SE and a dark-mode iPhone 17.
"""
import os, subprocess, sys
import numpy as np
from PIL import Image

SIM = os.environ.get("FOOLISH_SIM", "EFB2FD39-DD17-4284-9C46-013142226F6F")
SHOT = os.path.join(os.environ.get("FOOLISH_WORK", "/tmp/msgrig"), "_ui.png")


def grab():
    os.makedirs(os.path.dirname(SHOT), exist_ok=True)
    subprocess.run(["xcrun", "simctl", "io", SIM, "screenshot", SHOT],
                   capture_output=True)
    a = np.asarray(Image.open(SHOT).convert("RGB")).astype(int)
    # Points, not pixels: idb injects touches in points. Every 3x iPhone is at
    # least 1000px wide, every 2x one is under it.
    return a, (3 if a.shape[1] >= 1000 else 2)


def wood(a):
    """Wooden button pixels: warm, mid-to-bright, and clearly not wool."""
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    return (r > 120) & (r > b * 1.7) & (g > b) & (g < r * 0.85)


def card(a):
    """Card-face pixels: near-achromatic, either end of the luminance range."""
    hi, lo = a.max(axis=2), a.min(axis=2)
    return (hi - lo < 34) & ((hi < 80) | (lo > 150))


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


def hand_cards(a, s):
    """Face-up hand cards along the bottom -> [x_pt] centres."""
    h = a.shape[0]
    strip = card(a)[int(h * 0.88):, :]
    cols = np.nonzero(strip.mean(axis=0) > 0.35)[0]
    return [int(np.mean(gp) / s) for gp in runs(cols, 12 * s) if len(gp) > 30]


def hand_top(a, s):
    """Top edge of the hand strip (points) - where a card tap must land below."""
    m = card(a).mean(axis=1)
    lo = int(a.shape[0] * 0.72)
    for y in range(lo, a.shape[0] - 1):
        if m[y] > 0.55 and m[min(y + 10 * s, a.shape[0] - 1)] > 0.55:
            return int(y / s)
    return None


def drawer_top(a, s):
    """Top edge of the presented drawer (points)."""
    w = a.shape[1]
    band = a[:, int(w * 0.37):int(w * 0.87), :]
    warm = ((band[:, :, 0] - band[:, :, 2]) > 25).mean(axis=1)
    rows = np.nonzero(warm > 0.5)[0]
    return int(rows.min() / s) if len(rows) else None


if __name__ == "__main__":
    a, s = grab()
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    if what in ("bars", "all"):
        print("BARS", wood_bars(a, s))
    if what in ("cards", "all"):
        print("CARDS", hand_cards(a, s))
    if what in ("hand_y", "all"):
        # A tap 20pt into the strip lands on the card, never on its top edge.
        top = hand_top(a, s)
        print("HAND_Y", top + 20 if top is not None else -1)
    if what in ("top", "all"):
        print("TOP", drawer_top(a, s))
