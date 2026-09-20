#!/usr/bin/env python3
"""Structural check for a single-file surface study.

    python3 shared/tools/check_ui_doc.py <game>/docs/UI.html [...]

WHY THIS EXISTS. These documents are one big HTML file that gets forty rounds of
edits, and the failure mode is silent: markup that does not balance still renders,
just wrong, and an unterminated url() in a stylesheet takes every declaration
after it down with no error anywhere. The werewolf study shipped the same class
of bug three times before it had a checker.

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
    btn = set(re.findall(r'<button data-view="([a-z]+)"', s))
    tpl = set(re.findall(r'<template data-view="([a-z]+)"', s))
    for v in btn - tpl:
        bad.append(f'button "{v}" has no <template data-view="{v}"> to mount')
    for v in tpl - btn:
        bad.append(f'template "{v}" can never be reached - no button selects it')

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

    if bad:
        print(f"{path}: {len(bad)} problem(s)")
        for b in bad:
            print(f"  {b}")
        return 1
    print(f"{path} ok - {s0.count('class=\"device')} devices, "
          f"{len(tpl)} views, markup and css balanced")
    return 0


if __name__ == "__main__":
    args = sys.argv[1:] or ["uttt/docs/UI.html", "lastcard/docs/UI.html"]
    sys.exit(max(check(Path(a)) for a in args))
