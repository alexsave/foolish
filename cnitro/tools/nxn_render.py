#!/usr/bin/env python3
# Render the NxN win-rate matrix produced by nxn_matrix.sh.
# Rows = protagonist (seat 0), cols = opponent. Cell = protagonist win-rate %.
#
#   nxn_render.py <outdir>            full render: grid + ranking + note
#   nxn_render.py <outdir> --live     grid only (pending cells shown as ·),
#                                     fixed height for in-place repaint
import os, sys

OUT = sys.argv[1]
LIVE = "--live" in sys.argv[2:]
strats = open(os.path.join(OUT, "strats.txt")).read().split()

# short display codes (aligned with strategy.h aliases)
CODE = {
    "random": "rand", "espresso": "esp", "handwritten": "hw", "robusta": "rob",
    "firecracker": "fc", "gunpowder": "gp", "blackpowder": "bp", "cordite": "cd",
    "astrolite": "as", "cordite_old": "cd0", "simple_heuristic": "sh",
    "champion": "ch", "ultimate_champion": "uc", "hacker": "hk", "fulminate": "fm",
    "espresso_prod": "ep", "handwritten_prod": "hp", "distilled": "dl",
    "semtex": "sx", "semtex_oracle": "sxo", "octogen": "og",
    "octogen_oracle": "ogo", "torpex": "tx", "novichok": "nv",
}

def raw(a, b):
    p = os.path.join(OUT, "cells", f"{a}.__.{b}")
    if not os.path.exists(p):
        return None
    parts = open(p).read().split()
    if len(parts) < 3 or parts[2] == "NA":
        return None
    return float(parts[2].rstrip("%"))

def val(a, b):
    # direct measurement if present; else the anti-symmetric mirror (100 - B→A)
    v = raw(a, b)
    if v is not None:
        return v
    m = raw(b, a)
    return None if m is None else 100.0 - m

W = 5
PENDING = "·" if LIVE else "."

def print_grid():
    print(" " * 20 + "".join(f"{CODE[b]:>{W}}" for b in strats))
    rows = []
    for a in strats:
        cells, tot, n, done = [], 0.0, 0, 0
        for b in strats:
            v = val(a, b)
            if v is None:
                cells.append(f"{PENDING:>{W}}")
            else:
                cells.append(f"{v:>{W}.0f}")
                if a != b:
                    tot += v; n += 1; done += 1
        avg = tot / n if n else 0.0
        # trailing "| avg NN.N (k)" — k = opponents measured so far, live signal
        tail = f"   | avg {avg:5.1f}" + (f" ({done})" if LIVE else "")
        print(f"{a:<18}{CODE[a]:>2}" + "".join(cells) + tail)
        rows.append((a, avg))
    return rows

rows = print_grid()

if not LIVE:
    print("\nOverall win-rate vs field (excl. self), ranked:")
    for a, avg in sorted(rows, key=lambda r: -r[1]):
        print(f"  {avg:5.1f}%  {a}")
    print("\nnote: novichok, semtex_oracle, octogen_oracle observe hidden info (cheaters).")
