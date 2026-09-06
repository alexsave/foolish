#!/usr/bin/env python3
"""Inline the payload and the app into shell.html to make one standalone page.

No network at view time: the graph, the script and the styles all ship in the
file. The only external reference is the Google Fonts stylesheet, which degrades
to the declared fallback stack when it cannot load.
"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.abspath(sys.argv[1])
OUT = os.path.abspath(sys.argv[2])

shell = open(os.path.join(HERE, "shell.html"), encoding="utf-8").read()
app = open(os.path.join(HERE, "app.js"), encoding="utf-8").read()
payload = open(os.path.join(WORK, "payload.json"), encoding="utf-8").read()

# The payload rides in a <script type="application/json">, so the one sequence
# that could break out of it must not appear inside.
if "</script" in payload.lower():
    payload = payload.replace("</", "<\\/")

html = shell.replace("__PAYLOAD__", payload).replace("__SCRIPT__", app)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, "w", encoding="utf-8").write(html)
print("wrote %s (%.2f MB)" % (OUT, os.path.getsize(OUT) / 1e6))
