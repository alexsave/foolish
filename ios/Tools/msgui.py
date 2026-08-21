"""Locate Foolish UI elements from a simulator screenshot (device points).

Everything is found by colour, not by hard-coded coordinates, because the
layout shifts between compact/expanded, player counts and locales - which is
what kept sending taps into the Settings gear.
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
    return np.asarray(Image.open(SHOT).convert("RGB")).astype(int)


def wood_bars(a, x0=0, x1=1206, min_width_pt=120):
    """Wide wooden buttons -> [(y_pt, width_pt)], top to bottom."""
    r, g, b = a[:, x0:x1, 0], a[:, x0:x1, 1], a[:, x0:x1, 2]
    m = (r > 120) & (r > b * 1.7) & (g > b) & (g < r * 0.85)
    counts = m.sum(axis=1)
    rows = np.nonzero(counts > min_width_pt * 3)[0]
    bars, cur = [], []
    for y in rows:
        if cur and y - cur[-1] > 12:
            bars.append(cur); cur = []
        cur.append(y)
    if cur:
        bars.append(cur)
    out = []
    for bnd in bars:
        if len(bnd) < 20:            # ignore thin strips
            continue
        y = int(np.mean(bnd) / 3)
        w = int(counts[bnd].max() / 3)
        out.append((y, w))
    return out


def hand_cards(a):
    """Face-up hand cards along the bottom -> [x_pt] centres."""
    h = a.shape[0]
    strip = a[int(h * 0.86):, :, :]
    dark = strip.max(axis=2) < 70
    cols = np.nonzero(dark.mean(axis=0) > 0.35)[0]
    groups, cur = [], []
    for x in cols:
        if cur and x - cur[-1] > 12:
            groups.append(cur); cur = []
        cur.append(x)
    if cur:
        groups.append(cur)
    return [int(np.mean(gp) / 3) for gp in groups if len(gp) > 30]


def drawer_top(a):
    warm = (a[:, 450:1050, 0] - a[:, 450:1050, 2]) > 25
    rows = np.nonzero(warm.mean(axis=1) > 0.5)[0]
    return int(rows.min() / 3) if len(rows) else None


if __name__ == "__main__":
    a = grab()
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    if what in ("bars", "all"):
        print("BARS", wood_bars(a))
    if what in ("cards", "all"):
        print("CARDS", hand_cards(a))
    if what in ("top", "all"):
        print("TOP", drawer_top(a))
