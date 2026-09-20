#!/usr/bin/env python3
"""The rig's checks that need no simulator.

    python3 lib/test_rig.py

Three failures live here, and two of them REPORTED SUCCESS, which is what made
them expensive: a `first` that tapped the middle of the transcript and returned
0, and a finder whose numbers a shell cannot read. Each one is reachable from a
canned accessibility tree or a synthetic frame, so none of them needs a device.

`ax.py` is driven through its real command line, with a stub on `FOOLISH_IDB`
serving a recorded tree - the same path `rig.sh` takes, argument parsing and
all, rather than the functions underneath it.
"""
import ast
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
RIG = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import squares  # noqa: E402
import tween  # noqa: E402


# A REAL iOS 27 MESSAGES THREAD, recorded off `idb ui describe-all`
# (iPhone 17 Pro Max, 440x956pt, FoolishRigRepair). Frames are floats with
# Apple's rounding noise, exactly as they arrive.
#
# THE TRAP IS THE TWO "Messages" ELEMENTS. The back chevron is labelled `Back`
# on this OS, and the only things still called `Messages` are the application
# and a group the size of the whole screen - so a lookup that asks for
# `Messages` first and takes the smallest hit by AREA answers with a
# screen-sized element whose centre is the middle of the transcript.
IOS27_THREAD = [
    {"AXLabel": "Messages", "type": "Application",
     "frame": {"x": 0, "y": 0, "width": 440, "height": 956}},
    {"AXLabel": "Back", "type": "Button",
     "frame": {"x": 20, "y": 61.99999999999999,
               "width": 44, "height": 44.00000000000001}},
    {"AXLabel": "+1 (888) 555-1212", "type": "Button",
     "frame": {"x": 126.33333333333333, "y": 116.99999999999999,
               "width": 187.33333333333337, "height": 32.33333333333333}},
    {"AXLabel": "Messages", "type": "Group",
     "frame": {"x": 0, "y": 0, "width": 440, "height": 956}},
    {"AXLabel": "add", "type": "Button",
     "frame": {"x": 28, "y": 888, "width": 40, "height": 40}},
    {"AXLabel": "Message", "type": "TextField",
     "frame": {"x": 91, "y": 887.6666666666666,
               "width": 276.6666666666667, "height": 40.33333333333337}},
]

# The same thread through iOS 26, where the chevron IS called `Messages` - so
# three elements carry that label and only one of them is the control. The rig
# has to keep working here: this is the case the smallest-by-area rule got
# right, and a type filter must not lose it.
IOS26_THREAD = [
    dict(IOS27_THREAD[0]),
    {"AXLabel": "Messages", "type": "Button",
     "frame": {"x": 20, "y": 62, "width": 44, "height": 44}},
    dict(IOS27_THREAD[2]),
    dict(IOS27_THREAD[3]),
    dict(IOS27_THREAD[4]),
    dict(IOS27_THREAD[5]),
]

# The "+" APP MENU, recorded off the same device with the menu open. Two
# things in it are not where a fraction of the screen would put them: the
# rows start halfway DOWN the screen, and the scroll view that holds them
# (the unlabelled Group) ends at 921.7 - 26pt above where its last row ends.
IOS27_APP_MENU = [
    {"AXLabel": "Messages", "type": "Application",
     "frame": {"x": 0, "y": 0, "width": 440, "height": 956}},
    {"AXLabel": None, "type": "Group",
     "frame": {"x": 10, "y": 465.33333333333337,
               "width": 320, "height": 456.33333333333337}},
] + [
    {"AXLabel": lab, "type": "StaticText",
     "frame": {"x": 10, "y": 486.33333333333337 + 66 * i,
               "width": 320, "height": 66}}
    for i, lab in enumerate(["Camera", "Photos", "Audio", "#images",
                             "Apple Cash", "Check In", "Digital Touch"])
]

# Apple's first-run sheets, where the ORDER of the labels is the caller's
# policy: "Not Now" has to win over "Continue", because iOS 26's Check In
# sheet offers both and Continue walks into its setup.
FIRST_RUN_SHEET = [
    dict(IOS27_THREAD[0]),
    {"AXLabel": "Continue", "type": "Button",
     "frame": {"x": 40, "y": 800, "width": 360, "height": 50}},
    {"AXLabel": "Not Now", "type": "Button",
     "frame": {"x": 40, "y": 870, "width": 360, "height": 50}},
]


def stub_idb(tree):
    """A fake `idb` that answers `ui describe-all` with `tree`.

    Written to a temp dir and handed to ax.py through FOOLISH_IDB, so the test
    exercises the real subprocess call, the real JSON parse and the real
    argument parsing - not a monkeypatched `tree()`.
    """
    d = tempfile.mkdtemp(prefix="rigax")
    path = os.path.join(d, "idb")
    with open(path, "w") as fh:
        fh.write("#!/bin/sh\ncat <<'TREE'\n%s\nTREE\n" % json.dumps(tree))
    os.chmod(path, 0o755)
    return path


def ax(tree, *args):
    """`ax.py <args>` against `tree` -> (exit code, stdout stripped)."""
    env = dict(os.environ, FOOLISH_IDB=stub_idb(tree), FOOLISH_SIM="STUB")
    p = subprocess.run([sys.executable, os.path.join(HERE, "ax.py")] + list(args),
                       capture_output=True, text=True, env=env)
    return p.returncode, p.stdout.strip()


class AxFirstPicksAKind(unittest.TestCase):
    """`first` must be able to say what KIND of element it wants."""

    def test_the_back_chevron_on_ios_27(self):
        """The bug, from the side that matters: the chevron, not the transcript."""
        rc, out = ax(IOS27_THREAD, "first", "--type", "Button", "Messages", "Back")
        self.assertEqual(rc, 0)
        self.assertEqual(out, "42 84")          # the 44x44 chevron at 20,62

    def test_untyped_first_answers_with_the_whole_screen(self):
        """Why the filter exists. Held as a fact, not as a wish: an untyped
        lookup still reports success, and its answer is the middle of the
        transcript - a tap that stays in the thread it meant to leave."""
        rc, out = ax(IOS27_THREAD, "first", "Messages", "Back")
        self.assertEqual(rc, 0)
        self.assertEqual(out, "220 478")

    def test_the_back_chevron_on_ios_26(self):
        """Three elements labelled `Messages`; one of them is the control."""
        rc, out = ax(IOS26_THREAD, "first", "--type", "Button", "Messages", "Back")
        self.assertEqual(rc, 0)
        self.assertEqual(out, "42 84")

    def test_a_kind_that_is_not_there_is_not_a_hit(self):
        """`Message` is a TextField, so asking for a Button must not find it -
        and must not fall back to something else that happens to be labelled."""
        rc, _ = ax(IOS27_THREAD, "first", "--type", "Button", "Message")
        self.assertEqual(rc, 1)

    def test_the_label_order_survives_the_filter(self):
        """`Not Now` before `Continue` is policy, not a detail."""
        rc, out = ax(FIRST_RUN_SHEET, "first", "--type", "Button",
                     "Not Now", "OK", "Continue")
        self.assertEqual(rc, 0)
        self.assertEqual(out, "220 895")

    def test_smallest_still_wins_within_one_kind(self):
        """Two Buttons with the same label: the control is the smaller one."""
        tree = IOS27_THREAD + [{"AXLabel": "Back", "type": "Button",
                                "frame": {"x": 0, "y": 0,
                                          "width": 440, "height": 200}}]
        rc, out = ax(tree, "first", "--type", "Button", "Back")
        self.assertEqual(rc, 0)
        self.assertEqual(out, "42 84")


class TapBackAsksForAButton(unittest.TestCase):
    """rig.sh's own call site, checked by reading it: a `first` that can filter
    is no use if the one caller that needed it does not."""

    def test_tap_back_names_a_kind(self):
        with open(os.path.join(RIG, "rig.sh")) as fh:
            src = fh.read()
        body = re.search(r"^tap_back\(\) \{.*?^\}", src, re.S | re.M)
        self.assertIsNotNone(body, "tap_back is gone from rig.sh")
        self.assertIn("--type Button", body.group(0),
                      "tap_back asks for a label without a kind, so on iOS 27 "
                      "it can answer with the screen-sized `Messages` group")


class TheMenuDragIsMeasured(unittest.TestCase):
    """A drag that starts below a presented menu dismisses it, and the rig then
    blames the install for a menu it threw away itself. So both ends come from
    the menu."""

    def box(self):
        g = IOS27_APP_MENU[1]["frame"]
        return g["y"], g["y"] + g["height"]

    def test_both_ends_are_inside_the_menus_scroll_view(self):
        rc, out = ax(IOS27_APP_MENU, "menu")
        self.assertEqual(rc, 0)
        x, y_from, y_to = (int(v) for v in out.split())
        lo, hi = self.box()
        self.assertTrue(lo <= y_to < y_from <= hi,
                        "drag %d -> %d is not inside the menu (%.0f..%.0f)"
                        % (y_from, y_to, lo, hi))
        self.assertTrue(10 <= x <= 330, "x %d is outside the menu column" % x)

    def test_the_drag_follows_the_menu_and_a_fraction_does_not(self):
        """The whole point, stated as the difference it makes.

        89% of this screen is 851, which IS inside this menu - measured on
        6.9", 6.3" and 4.7" phones, the fraction lands inside every time. It is
        unmoored rather than yet wrong, and the way to show that is to move the
        menu: the same rows 200pt higher (a shorter list, a different host
        layout, a future popover) leave the fraction 129pt BELOW the menu,
        while a measured drag moves with it."""
        moved = [dict(el, frame=dict(el["frame"],
                                     y=el["frame"]["y"] - (200 if el["type"] != "Application" else 0)))
                 for el in IOS27_APP_MENU]
        lo, hi = (v - 200 for v in self.box())
        self.assertGreater(0.89 * 956, hi, "the moved menu is still under 89%")
        rc, out = ax(moved, "menu")
        self.assertEqual(rc, 0)
        y_from, y_to = int(out.split()[1]), int(out.split()[2])
        self.assertTrue(lo <= y_to < y_from <= hi,
                        "drag %d -> %d left the moved menu (%.0f..%.0f)"
                        % (y_from, y_to, lo, hi))

    def test_a_drag_that_moves_by_at_least_one_row(self):
        rc, out = ax(IOS27_APP_MENU, "menu")
        y_from, y_to = int(out.split()[1]), int(out.split()[2])
        self.assertGreaterEqual(y_from - y_to, 66,
                                "a scroll shorter than a row is not a scroll")

    def test_no_menu_is_a_failure_not_a_swipe(self):
        """The thread with no menu up. Answering anything here would drag
        across the transcript."""
        rc, _ = ax(IOS27_THREAD, "menu")
        self.assertEqual(rc, 1)

    def test_a_menu_with_no_scroll_view_falls_back_to_its_rows(self):
        """Not every OS offers the container. The rows are still a column."""
        rows = [el for el in IOS27_APP_MENU if el["type"] != "Group"]
        rc, out = ax(rows, "menu")
        self.assertEqual(rc, 0)
        y_from, y_to = int(out.split()[1]), int(out.split()[2])
        self.assertEqual((y_from, y_to), (915, 519))

    def test_a_short_menu_with_no_scroll_view_still_answers(self):
        """Three apps and no container is the thinnest thing that is still a
        menu; a rig that called that "no menu" would refuse to scroll a screen
        it could have scrolled."""
        rows = [el for el in IOS27_APP_MENU if el["type"] != "Group"][:4]
        rc, out = ax(rows, "menu")
        self.assertEqual(rc, 0)
        y_from, y_to = int(out.split()[1]), int(out.split()[2])
        self.assertEqual((y_from, y_to), (651, 519))


def only_builtins(value, where="value"):
    """Every scalar reachable from `value` is a plain int/float/str/bool/None.

    A numpy scalar prints as `219.8` on its own and as `np.float64(219.8)`
    inside a list or a tuple, which is where the finders' answers live. A shell
    reads that as the word `np.float64`, `printf %.0f` turns the word into 0,
    and the tap goes to the corner of the screen while the driver reports the
    coordinate it meant. Nothing downstream can tell the two apart, so the
    types are pinned here, at the seam.
    """
    if isinstance(value, (list, tuple)):
        return [p for i, v in enumerate(value)
                for p in only_builtins(v, "%s[%d]" % (where, i))]
    if isinstance(value, dict):
        return [p for k, v in value.items()
                for p in only_builtins(v, "%s[%r]" % (where, k))]
    if type(value).__module__ != "builtins":
        return ["%s is %s (%r)" % (where, type(value).__name__, value)]
    return []


def ruler_frame(h=2868, w=1320):
    """A synthetic ruler frame: the red top bar, the green bottom bar, a
    magenta square on the opponent and two cyan pair squares. Enough for
    `squares_in` and `tween.read_array`, which is where the numpy scalars are
    minted."""
    a = np.zeros((h, w, 3), dtype=np.uint8)
    s = 3
    a[300:306, :] = (255, 0, 0)                      # box top bar
    a[2400:2406, :] = (0, 255, 0)                    # box bottom bar
    side = int(squares.SIDE_PT * s)
    for x0 in (400, 700):                            # table pairs
        a[1200:1200 + side, x0:x0 + side] = (0, 255, 255)
    a[600:600 + side, 900:900 + side] = (255, 0, 255)   # the opponent
    return a


class FindersAnswerInPlainNumbers(unittest.TestCase):
    def test_squares_are_plain_floats(self):
        found = squares.squares_in(ruler_frame())
        self.assertTrue(found, "the synthetic frame drew no squares")
        self.assertEqual(only_builtins(found, "squares_in()"), [])

    def test_a_squares_answer_can_be_read_back_as_a_literal(self):
        """rig.sh parses the finders' lists with `ast.literal_eval`, which
        refuses `np.float64(219.8)` outright."""
        found = squares.squares_in(ruler_frame())
        try:
            back = ast.literal_eval(repr(found))
        except (ValueError, SyntaxError) as e:
            self.fail("rig.sh cannot parse %r: %s" % (repr(found)[:90], e))
        self.assertEqual(back, found)

    def test_the_ruler_box_is_plain_floats(self):
        box = tween.read_array(ruler_frame())
        self.assertIsNotNone(box, "the synthetic frame drew no ruler box")
        self.assertIn("opp_pt", box, "the synthetic frame drew no opponent square")
        self.assertEqual(only_builtins(box, "read_array()"), [])


def synthetic_board(h=2868, w=1320):
    """A frame with the landmarks ui.py's finders look for, at 3x.

    Not a screenshot: a transcript-black top, a felt drawer under it, one
    wooden button row, a row of white cards, and Messages' grab handle (a
    mid-luminance pill with black above it). Enough that every `ui.py`
    subcommand answers with a number instead of None, which is what this is
    for - the answers are read as text by rig.sh, and text is where a numpy
    scalar shows itself.
    """
    a = np.zeros((h, w, 3), dtype=np.uint8)
    edge = 1701                                  # 567pt at 3x
    a[edge:] = (19, 55, 39)                      # felt
    a[edge + 27:edge + 42, int(w * 0.47):int(w * 0.53)] = (105, 105, 105)
    a[2100:2180, 120:w - 120] = (171, 120, 84)   # a wooden button
    a[2360:2420, 200:260] = (171, 120, 84)       # the rules checkbox
    for x0 in range(300, 1000, 120):             # the hand
        a[2500:2720, x0:x0 + 100] = (255, 255, 255)
    a[2790:2830, 140:200] = (171, 120, 84)       # the control row icons
    a[2790:2830, w - 200:w - 140] = (171, 120, 84)
    return a


class UiSpeaksNumbersAShellCanRead(unittest.TestCase):
    """rig.sh reads `ui.py`'s answers with `ast.literal_eval` and with
    `printf`. Both take a numpy scalar's repr as a word, not a number: one
    raises, the other prints 0 and taps the corner of the screen while the
    driver reports the coordinate it meant."""

    @classmethod
    def setUpClass(cls):
        from PIL import Image
        cls.work = tempfile.mkdtemp(prefix="rigui")
        Image.fromarray(synthetic_board()).save(os.path.join(cls.work, "_ui.png"))
        env = dict(os.environ, FOOLISH_WORK=cls.work, FOOLISH_SIM="STUB")
        p = subprocess.run([sys.executable, os.path.join(HERE, "ui.py"), "all"],
                           capture_output=True, text=True, env=env)
        cls.lines = [l for l in p.stdout.splitlines() if l.strip()]

    def test_the_synthetic_board_is_read_at_all(self):
        """Otherwise every assertion below is vacuously true."""
        self.assertIn("TOP 567", self.lines)
        self.assertTrue(any(l.startswith("CARDS [") and l != "CARDS []"
                            for l in self.lines), self.lines)
        self.assertTrue(any(l.startswith("BARS [") and l != "BARS []"
                            for l in self.lines), self.lines)

    def test_no_answer_names_a_numpy_type(self):
        bad = [l for l in self.lines if "np." in l or "numpy" in l]
        self.assertEqual(bad, [], "a finder printed a numpy repr: %s" % bad)

    def test_every_answer_is_a_python_literal(self):
        """Exactly what `bar_y`, `openbubble`, `lobbytap` and `play` do to it."""
        for line in self.lines:
            head, _, rest = line.partition(" ")
            if not rest or head in ("HOSTEDGE",):     # two values on one line
                continue
            try:
                ast.literal_eval(rest)
            except (ValueError, SyntaxError) as e:
                self.fail("rig.sh cannot parse %r: %s" % (line, e))


class RoundingIsLaundered(unittest.TestCase):
    """The pattern, not the site.

    `round(x)` with no ndigits returns a plain int whatever `x` was.
    `round(x, 1)` hands back the type it was GIVEN, so `round(np.float64(y), 1)`
    is still a numpy scalar - and that is how a measurement ends up printed as
    `np.float64(219.8)` inside somebody's list. Two of the rig's finders already
    wrote `round(float(...), 1)`, each one after a run went wrong; this is that
    lesson written down where the next finder has to read it.
    """

    @staticmethod
    def has_ndigits(text):
        """A comma at the top level of this round()'s argument list."""
        depth = 0
        for ch in text:
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                if depth == 0:
                    return False
                depth -= 1
            elif ch == "," and depth == 0:
                return True
        return False

    def test_every_rounded_measurement_names_a_plain_type(self):
        bad = []
        for name in sorted(os.listdir(HERE)):
            if not name.endswith(".py") or name.startswith("test_"):
                continue
            with open(os.path.join(HERE, name)) as fh:
                for n, line in enumerate(fh, 1):
                    code = line.split("#", 1)[0]
                    for m in re.finditer(r"(?<![\w.])round\(", code):
                        tail = code[m.end():]
                        if not self.has_ndigits(tail):
                            continue
                        if not re.match(r"\s*(float|int)\(", tail):
                            bad.append("%s:%d: %s" % (name, n, line.strip()))
        self.assertEqual(bad, [], "a round(x, n) whose value is not explicitly "
                                  "float()/int() hands a numpy scalar on:\n  "
                                  + "\n  ".join(bad))


class ProductIdentityLivesInOnePlace(unittest.TestCase):
    """The rig is about to drive a second product. Every bundle id, App Group,
    scheme and display name it knows is one named constant at the top of
    rig.sh, so a second product is a block of assignments and not a hunt."""

    IDENT = re.compile(r"cards\.foolish|group\.cards|FoolishMessages|Foolish\.xcodeproj"
                       r"|\bFoolishKit\b|\"Foolish\"")

    def setUp(self):
        with open(os.path.join(RIG, "rig.sh")) as fh:
            self.src = fh.read().split("\n")

    def test_the_block_exists(self):
        block = "\n".join(self.src)
        for var in ("APP_ID=", "EXT_DOM=", "APP_GROUP=", "SCHEME=",
                    "XCPROJ=", "MENU_NAME=", "APPEX=", "LOG_SUBSYSTEM="):
            self.assertIn(var, block, "no %s constant in rig.sh" % var)

    def test_nothing_below_the_block_spells_the_product(self):
        start = next(i for i, l in enumerate(self.src) if l.startswith("APP_ID="))
        end = next(i for i, l in enumerate(self.src[start:], start)
                   if l.startswith("# ---- end of the product block"))
        bad = []
        for n, line in enumerate(self.src, 1):
            if start < n <= end + 1:                 # the block itself
                continue
            code = line.split("#", 1)[0] if not line.lstrip().startswith("#") else ""
            if self.IDENT.search(code):
                bad.append("%d: %s" % (n, line.strip()))
        self.assertEqual(bad, [], "product identity spelled outside the block:\n  "
                                  + "\n  ".join(bad))


if __name__ == "__main__":
    unittest.main(verbosity=2)
