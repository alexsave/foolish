#!/usr/bin/env python3
"""Structural check for a single-file surface study.

    python3 shared/tools/check_ui_doc.py <game>/docs/UI.html [...]

WHY THIS EXISTS. These documents are one big HTML file that gets forty rounds of
edits, and the failure mode is silent: markup that does not balance still renders,
just wrong, and an unterminated url() in a stylesheet takes every declaration
after it down with no error anywhere. An earlier study in this repo shipped the
same class of bug three times before it had a checker.

Product-neutral on purpose - it names no game, so it lives in shared/.
"""
import re
import sys
from pathlib import Path


def check(path: Path):
    s = s0 = path.read_text()
    bad = []

    body = s[s.find('<body>'):] if '<body>' in s else s
    depth = 0
    for m in re.finditer(r'<div\b|</div>', body):
        depth += 1 if m.group(0).startswith('<div') else -1
        if depth < 0:
            bad.append(f"an extra </div> at line {body[:m.start()].count(chr(10)) + 1}")
            break
    else:
        if depth:
            bad.append(f"{depth} <div> left open")

    if s.count('<template') != s.count('</template>'):
        bad.append("a <template> is never closed - its view will render as nothing")

    # every switcher button must have a view to mount, and vice versa
    btn = set(re.findall(r'<button data-view="([\w-]+)"', s))
    tpl = set(re.findall(r'<template data-view="([\w-]+)"', s))
    for v in btn - tpl:
        bad.append(f'button "{v}" has no <template data-view="{v}"> to mount')
    for v in tpl - btn:
        bad.append(f'template "{v}" can never be reached - no button selects it')

    # A data URI carries double quotes. Put one in an inline style="" and the
    # attribute ends early, the declaration dies, and the page still renders -
    # just wrong. Cost an hour the first time.
    for m in re.finditer(r'style="([^"]*)"', s):
        v = m.group(1)
        if v.rstrip().endswith(("url(", "url(\'")) or v.count("(") != v.count(")"):
            line = s[:m.start()].count(chr(10)) + 1
            bad.append(f"line {line}: an inline style= is cut off mid-value "
                       f"({v[-40:]!r}) - a double quote in a data URI?")

    css = "".join(re.findall(r'<style>(.*?)</style>', s, re.S))
    if css.count('{') != css.count('}'):
        bad.append(f"stylesheet braces do not balance: {css.count('{') - css.count('}'):+d}")
    if css.count('"') % 2:
        bad.append("stylesheet has an odd number of double quotes - an unterminated url()?")

    # a var() that nothing defines invalidates its WHOLE declaration, quietly.
    # NOTE the inline sweep: a custom property is just as legally set by a
    # style="--bd:348px" attribute as by the stylesheet, and an earlier version
    # of this check reported both documents' sizing tokens as dead.
    inline = " ".join(re.findall(r'style="([^"]*)"', s))
    used = set(re.findall(r'var\((--[a-z0-9-]+)', css + " " + inline))
    defd = set(re.findall(r'(--[a-z0-9-]+)\s*:', css + " " + inline))
    for v in sorted(used - defd):
        bad.append(f"{v} is used but never defined - every rule using it is dead")

    # A box that wraps its own class ("bwrap" nested inside "bwrap") is a
    # percentage-sizing trick: the innermost one is what a `width:100%` canvas
    # actually measures against, and nothing clips the outer one to match it.
    # Two numbers that used to move together (a find-and-replace across one
    # of them, a copy-pasted row edited on one side only) silently stop
    # agreeing, and the drawn board sticks out past the box the label next to
    # it still claims - exactly the shape a screenshot catches and a diff of
    # the markup does not, because both numbers are still valid CSS.
    for m in re.finditer(
            r'<div class="([\w-]+)" style="width:(\d+)px;height:(\d+)px[^"]*">'
            r'\s*<div class="\1" style="width:(\d+)px;height:(\d+)px', s):
        cls, w1, h1, w2, h2 = m.groups()
        if w1 != w2 or h1 != h2:
            line = s[:m.start()].count(chr(10)) + 1
            bad.append(f'line {line}: nested "{cls}" wrappers disagree on size '
                       f'({w1}x{h1} outside, {w2}x{h2} inside) - the inner one '
                       f'is what a 100%-sized canvas measures, so it will '
                       f'overflow or shrink inside the outer box')

    # A canvas sized from clientWidth with a hardcoded fallback ("px || 34")
    # only covers ONE variant of that canvas. A modifier class that scales it
    # up ("markico big" at 46 next to plain "markico" at 34) is invisible to
    # that fallback, so a canvas measured before layout - a hidden tab, a
    # cloned template not yet in the document - silently draws at the wrong
    # resolution, or not at all, and nothing on screen says why.
    canvas_class_size = {}
    for m in re.finditer(r'canvas((?:\.[\w-]+)+)\s*\{[^}]*?width:\s*(\d+)px', css):
        classes = frozenset(m.group(1).lstrip('.').split('.'))
        canvas_class_size[classes] = int(m.group(2))

    def effective_size(elt_classes):
        best = None
        for classes, px in canvas_class_size.items():
            if classes <= elt_classes and (best is None or len(classes) > len(best[0])):
                best = (classes, px)
        return best[1] if best else None

    def classes_of(tag):
        m = re.search(r'class="([^"]*)"', tag)
        return set(m.group(1).split()) if m else set()

    canvases = [(classes_of(c), c) for c in re.findall(r'<canvas\b[^>]*>', s)]

    script = "".join(re.findall(r'<script>(.*?)</script>', s, re.S))
    for fm in re.finditer(r'function\s+\w+\([^)]*\)\s*\{', script):
        depth, i = 0, fm.start()
        started = False
        while i < len(script):
            if script[i] == '{':
                depth += 1; started = True
            elif script[i] == '}':
                depth -= 1
                if started and depth == 0:
                    break
            i += 1
        body = script[fm.start():i + 1]
        sel_m = re.search(r"querySelectorAll\('([^']*canvas[^']*)'\)", body)
        fb_m = re.search(r'\.client(?:Width|Height)\s*\|\|\s*(\d+)', body)
        if not (sel_m and fb_m):
            continue
        sel, fallback = sel_m.group(1), int(fb_m.group(1))
        attr_m = re.search(r'\[([\w-]+)\]', sel)
        matched_sizes = set()
        for classes, tag in canvases:
            if attr_m and attr_m.group(1) not in tag:
                continue
            px = effective_size(set(classes))
            if px is not None:
                matched_sizes.add(px)
        if len(matched_sizes) > 1:
            line = script[:fm.start()].count(chr(10)) + s[:s.find('<script>')].count(chr(10)) + 1
            uncovered = sorted(matched_sizes - {fallback})
            bad.append(f"line {line}: a single clientWidth fallback of "
                       f"{fallback} in {sel!r}'s init function covers only "
                       f"one of the sizes it can draw {sorted(matched_sizes)} "
                       f"- the {uncovered} variant(s) will size wrong if "
                       f"measured before layout (a hidden tab, a canvas "
                       f"not yet in the document)")

    if bad:
        print(f"{path}: {len(bad)} problem(s)")
        for b in bad:
            print(f"  {b}")
        return 1
    print(f"{path} ok - {s0.count('class=\"device')} devices, "
          f"{len(tpl)} views, markup and css balanced")
    return 0


if __name__ == "__main__":
    args = sys.argv[1:] or ["uttt/docs/UI.html", "pickemup/docs/UI.html"]
    sys.exit(max(check(Path(a)) for a in args))
