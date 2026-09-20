"""Locate a UI element by accessibility label, in DEVICE POINTS.

Why this exists: the rig's taps used to be hard-coded screen coordinates read
off one phone (iPhone 17), so moving to any other device - a smaller drawer, a
different toolbar height, a phone with no home indicator - drove taps into the
wrong controls. The accessibility tree reports frames in points on every
device, so a rig that asks for "Foolish" or "Create game" by name is portable.

  msgax.py find "Create game"          -> "X Y" (centre, points) or exit 1
  msgax.py first [--type T] L1 L2 ...  -> the first of several labels present
  msgax.py dump [substring]            -> every labelled element, for exploring
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


def first(t, labels, kind=None):
    """The centre of the first of `labels` that is on screen, IN THE ORDER
    GIVEN, optionally restricted to one element KIND.

    The order is the caller's policy, not a detail: the first-run sheet hunt
    must try "Not Now" before "Continue", because iOS 26's Check In sheet
    offers both and Continue walks into its setup.

    `kind` IS WHAT MAKES THE ORDER SAFE, and it is not optional for anything
    that taps. Within one label the smallest hit by area wins, on the theory
    that the smallest element carrying a name is the control itself rather
    than a group that contains it - and on iOS 27 that theory breaks in the
    worst possible place. A Messages thread carries THREE elements labelled
    `Messages`: the application, a group the size of the whole screen, and
    (through iOS 26 only) the back chevron. On iOS 27 the chevron is called
    `Back`, so the smallest `Messages` is a 440x956 group whose centre is the
    middle of the transcript - `tap_back` asked for `Messages` first, tapped
    the conversation, RETURNED 0, and left the caller inside the thread it
    believed it had left. Every read after that was of the wrong screen, and
    the rig blamed the conversation for a thread that was on screen.

    So a caller says what it wants: `first(t, ["Messages", "Back"], "Button")`
    cannot match an Application or a Group, falls through to the chevron, and
    is right on both versions of the OS. Matched as a substring of the type,
    the same test `here` uses, because idb spells compound types.
    """
    for want in labels:
        hits = [el for el in t
                if (el.get("AXLabel") or "") == want
                and (kind is None or kind in el.get("type", ""))
                and el["frame"]["width"] > 0 and el["frame"]["height"] > 0]
        if hits:
            return centre(min(hits, key=lambda e: e["frame"]["width"]
                              * e["frame"]["height"]))
    return None


def menu_scroll(t):
    """Where to drag to scroll the "+" app menu -> (x, from_y, to_y), or None.

    MEASURED, NOT A FRACTION OF THE SCREEN. This used to drag from 89% of the
    screen height up to 55%, two numbers with nothing behind them. A drag that
    starts below a presented menu, or ends above it, DISMISSES the menu - and
    the app is then genuinely not on screen, so the rig blames the install
    ("is the extension installed?") for a menu it threw away itself.

    Measured on three phones, iOS 27.0: the menu is a FIXED 320x456.5pt
    popover, bottom-anchored above the compose bar - 6.9" (440x956) 465.3..921.7,
    6.3" (402x874) 383.3..839.7, 4.7" (375x667) 200.5..657.0. So the fraction
    happens to land inside it on every iPhone, and that is luck, not design:
    the menu's position is a function of the popover's fixed size and the
    home-indicator inset, not of the screen's height. It is already wrong at
    iPad heights - the old end point, 0.55H, is above the menu's top edge
    (H - 490.5) for any H over 1090pt - and it is wrong the moment Apple
    changes the popover.

    THE MENU IS A COLUMN OF IDENTICAL CELLS, and that is the whole finder. Our
    extension reports nothing to the accessibility tree and Apple's rows are
    named in whatever language the device is set to, so neither the type nor
    the label can be trusted; the SHAPE can. The app rows are the biggest set
    of elements sharing one x and one width, they all have the same height, and
    the scroll view that holds them shares their x and width and is several
    rows tall. Recorded on a 6.9" phone (iOS 27.0, 440x956pt): seven StaticText
    rows at x=10 w=320 h=66, inside an unlabelled Group at x=10 y=465.3 w=320
    h=456.3 - which is 26pt SHORT of the last row, so "inside the rows" and
    "inside the menu" are not the same thing and the rows alone are not enough.

    Both ends of the drag are therefore ROW CENTRES that lie inside that box:
    the lowest such row to start from and the highest to end on. A row centre
    cannot be under the menu (it is a row), cannot be in the home-indicator
    strip (a row is inset above the compose bar), and cannot be outside the
    scroll view (it is tested against it).
    """
    if not t:
        return None
    screen_w = t[0]["frame"]["width"]
    cols = {}
    for el in t:
        f = el["frame"]
        if f["width"] <= 0 or f["height"] <= 0 or f["width"] >= screen_w:
            continue
        cols.setdefault((round(f["x"]), round(f["width"])), []).append(f)
    if not cols:
        return None
    col = max(cols.values(), key=len)
    if len(col) < 3:                  # a column, not a coincidence
        return None
    heights = sorted(f["height"] for f in col)
    row_h = heights[len(heights) // 2]          # the modal cell height
    rows = [f for f in col if abs(f["height"] - row_h) < 2]
    if len(rows) < 3:
        return None
    # The scroll view, when the tree offers one: same column, several rows
    # tall. Without it the rows' own union is the best box there is.
    tall = [f for f in col if f["height"] > row_h * 1.5]
    box = max(tall, key=lambda f: f["height"]) if tall else {
        "y": min(f["y"] for f in rows),
        "height": max(f["y"] + f["height"] for f in rows) - min(f["y"] for f in rows)}
    lo, hi = box["y"], box["y"] + box["height"]
    inside = sorted(f["y"] + f["height"] / 2 for f in rows
                    if lo <= f["y"] + f["height"] / 2 <= hi)
    if len(inside) < 2:
        return None
    f0 = rows[0]
    return f0["x"] + f0["width"] / 2, inside[-1], inside[0]


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "dump"
    if cmd == "screen":
        app = tree()
        if not app:
            sys.exit(1)
        f = app[0]["frame"]
        print(f"{f['width']:.0f} {f['height']:.0f}")
    elif cmd == "here":
        # `in_thread` AND the thread's identity, from ONE tree. The thread's
        # header is a Button carrying the remote address; "add" (the compose +)
        # is what proves a thread is open at all. Same answers as the two calls
        # this replaces, same exact-vs-substring rules: "add" exact, the header
        # a substring, matched on the printed line the way `dump | grep Button`
        # did.
        t = tree()
        if not any((el.get("AXLabel") or "") == "add" for el in t):
            sys.exit(1)
        want = sys.argv[2] if len(sys.argv) > 2 else ""
        if not want:
            sys.exit(0)
        w = want.lower()
        sys.exit(0 if any(w in (el.get("AXLabel") or "").lower()
                          and "Button" in el.get("type", "")
                          for el in t) else 1)
    elif cmd == "first":
        # first [--type KIND] LABEL... - see `first()` for why KIND matters.
        args, kind = sys.argv[2:], None
        if len(args) >= 2 and args[0] == "--type":
            kind, args = args[1], args[2:]
        c = first(tree(), args, kind)
        if c is None:
            sys.exit(1)
        print(f"{c[0]:.0f} {c[1]:.0f}")
    elif cmd == "menu":
        # "X FROM_Y TO_Y" - one upward drag inside the "+" app menu.
        m = menu_scroll(tree())
        if m is None:
            sys.exit(1)
        print(f"{m[0]:.0f} {m[1]:.0f} {m[2]:.0f}")
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
