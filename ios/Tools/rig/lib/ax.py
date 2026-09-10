"""Locate a UI element by accessibility label, in DEVICE POINTS.

Why this exists: the rig's taps used to be hard-coded screen coordinates read
off one phone (iPhone 17), so moving to any other device - a smaller drawer, a
different toolbar height, a phone with no home indicator - drove taps into the
wrong controls. The accessibility tree reports frames in points on every
device, so a rig that asks for "Foolish" or "Create game" by name is portable.

  msgax.py find "Create game"     -> "X Y" (centre, points) or exit 1
  msgax.py dump [substring]       -> every labelled element, for exploring
"""
import json
import os
import subprocess
import sys

SIM = os.environ.get("FOOLISH_SIM", "EFB2FD39-DD17-4284-9C46-013142226F6F")
IDB = os.environ.get("FOOLISH_IDB", "idb")


def tree():
    out = subprocess.run([IDB, "ui", "describe-all", "--udid", SIM],
                         capture_output=True, text=True)
    try:
        return json.loads(out.stdout)
    except json.JSONDecodeError:
        return []


def centre(el):
    f = el["frame"]
    return f["x"] + f["width"] / 2, f["y"] + f["height"] / 2


def find(label, exact=False):
    """Smallest element whose label matches - the smallest one is the control
    itself rather than a group that happens to contain it."""
    hits = []
    for el in tree():
        lab = el.get("AXLabel") or ""
        if (lab == label) if exact else (label.lower() in lab.lower()):
            f = el["frame"]
            if f["width"] > 0 and f["height"] > 0:
                hits.append((f["width"] * f["height"], el))
    if not hits:
        return None
    return centre(min(hits, key=lambda h: h[0])[1])


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "dump"
    if cmd == "screen":
        app = tree()
        if not app:
            sys.exit(1)
        f = app[0]["frame"]
        print(f"{f['width']:.0f} {f['height']:.0f}")
    elif cmd == "find":
        pt = find(sys.argv[2], exact="--exact" in sys.argv)
        if pt is None:
            sys.exit(1)
        print(f"{pt[0]:.0f} {pt[1]:.0f}")
    else:
        needle = sys.argv[2].lower() if len(sys.argv) > 2 else ""
        for el in tree():
            lab = el.get("AXLabel") or ""
            if lab and needle in lab.lower():
                f = el["frame"]
                print(f"{lab[:44]:<46} {el['type']:<12} "
                      f"{f['x']:.0f},{f['y']:.0f} {f['width']:.0f}x{f['height']:.0f}")
