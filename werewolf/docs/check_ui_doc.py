#!/usr/bin/env python3
"""Structural check for docs/UI.html.

WHY THIS EXISTS. The doc carries two mockup systems. The flow mockups use
`.night / .inner / .shields / .sh`, which are defined unscoped. The style
comparison uses `.scr / .in / .grid / .s / .bb / .thr`, which are scoped under
`.sv` so they cannot clobber the first set.

A device in the wrong one renders as a LAYOUT bug and is a SELECTOR bug, which
is why it fooled a reader three times: `.hdr`, `.ask`, `.said` and `.stage` are
unscoped, so a broken device keeps its type and headings and loses only its
grid - a shield becomes three stacked lines of text.

Two ways to get it wrong, and this checks both:
  * a device using the SCOPED structure that sits outside any `.sv`
  * a device using the UNSCOPED structure that sits inside one

Deliberately keyed on the STRUCTURAL class names, not on the theme names
(`cl`, `mn`, `ae`, `md`, `az`). An earlier version of this check listed the
themes and missed three bubbles whose class attribute was empty.

    python3 docs/check_ui_doc.py
"""
import re
import sys
from pathlib import Path

SCOPED = ('class="grid"', 'class="bb"', 'class="thr"', 'class="scr"', 'class="in"')
# NOTE the closing quotes: an earlier version used a bare 'class="night' prefix,
# which happily matched class="nightglow" and reported four false positives.
UNSCOPED = ('class="shields"', 'class="inner"', 'class="night"',
            'class="night ', 'class="bub"')

DOC = Path(__file__).resolve().parent / "UI.html"


def extent(s: str, start: int):
    """End offset of the <div> opening at `start`, by brace-style depth."""
    depth = 0
    for m in re.finditer(r'<div\b|</div>', s[start:]):
        depth += 1 if m.group(0).startswith('<div') else -1
        if depth == 0:
            return start + m.end()
    return None


def main() -> int:
    s = DOC.read_text()
    problems = []

    # --- every .sv wrapper's span -------------------------------------
    spans = []
    for m in re.finditer(r'<div class="sv">', s):
        end = extent(s, m.start())
        if end is None:
            problems.append("a <div class=\"sv\"> is never closed")
        else:
            spans.append((m.start(), end))

    def wrapped(i):
        return any(a <= i < b for a, b in spans)

    # --- devices on the wrong side of the scope ------------------------
    for m in re.finditer(r'<div class="device[^"]*"', s):
        end = extent(s, m.start())
        if end is None:
            problems.append(f"unclosed device at line {s[:m.start()].count(chr(10)) + 1}")
            continue
        blk = s[m.start():end]
        line = s[:m.start()].count('\n') + 1
        if any(c in blk for c in SCOPED) and not wrapped(m.start()):
            problems.append(f"line {line}: device uses the SCOPED structure but is outside any .sv")
        if any(c in blk for c in UNSCOPED) and wrapped(m.start()):
            problems.append(f"line {line}: device uses the UNSCOPED structure but is inside a .sv")

    # --- the document has to be balanced at all --------------------------
    off = s.find('<body>')
    base = s[:off].count('\n')          # absolute lines, not body-relative
    body = s[off:]
    depth = 0
    for m in re.finditer(r'<div\b|</div>', body):
        depth += 1 if m.group(0).startswith('<div') else -1
        if depth < 0:
            problems.append(f"an extra </div> at line {base + body[:m.start()].count(chr(10)) + 1}")
            break
    else:
        if depth:
            problems.append(f"{depth} <div> left open at the end of the document")

    # --- and the stylesheet has to parse ---------------------------------
    css = s[s.find('<style>') + 7:s.find('</style>')]
    if css.count('{') != css.count('}'):
        problems.append(f"stylesheet braces do not balance: {css.count('{') - css.count('}'):+d}")
    if css.count('"') % 2:
        problems.append("stylesheet has an odd number of double quotes - an unterminated url()?")

    if problems:
        print(f"docs/UI.html: {len(problems)} problem(s)")
        for p in problems:
            print(f"  {p}")
        return 1

    devices = s.count('class="device')
    print(f"docs/UI.html ok - {devices} devices, {len(spans)} scoped regions, markup and css balanced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
