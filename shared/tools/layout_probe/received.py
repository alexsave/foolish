#!/usr/bin/env python3
"""received.py - judge every probe bubble this Mac has received (or synced).

Reads ~/Library/Messages/chat.db read-only for the probe's messages, takes each
one's picture file from Messages' Attachments folder and its variant (name, n,
grey) from the bubble's own URL, and runs `build/sweep --judge` at that grid.
The terminal needs Full Disk Access. Run `make build/sweep` first.

    received.py [--since 'YYYY-MM-DD HH:MM']
"""
import os, plistlib, sqlite3, subprocess, sys, urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
SWEEP = os.path.join(HERE, "build", "sweep")
since = sys.argv[sys.argv.index("--since") + 1] if "--since" in sys.argv else "2000-01-01 00:00"
db = sqlite3.connect("file:" + os.path.expanduser("~/Library/Messages/chat.db") + "?mode=ro", uri=True)
rows = db.execute(
    """select m.ROWID, datetime(m.date/1000000000+978307200,'unixepoch','localtime'), m.is_from_me,
              m.payload_data, a.filename
       from message m join message_attachment_join j on j.message_id = m.ROWID
       join attachment a on a.ROWID = j.attachment_id
       where m.balloon_bundle_id like '%layoutprobe%'
         and datetime(m.date/1000000000+978307200,'unixepoch','localtime') >= ?
       order by m.date, m.is_from_me""", (since,)).fetchall()

def variant(payload):
    for o in plistlib.loads(payload)["$objects"]:
        s = o if isinstance(o, str) else str(getattr(o, "get", lambda k: "")("NS.relative") or "")
        if "v=" in s and "n=" in s:
            q = urllib.parse.parse_qs(urllib.parse.urlsplit(s).query)
            return q["v"][0], int(q["n"][0]), q.get("c", ["1"])[0] != "0"
    return "?", 243, True

print(f"{'row':>5} {'time':19} {'dir':4} {'variant':10} {'file px':>9} {'bytes':>10}  result")
for rid, when, mine, payload, fn in rows:
    path = os.path.expanduser(fn)
    name, n, grey = variant(payload)
    out = subprocess.run([SWEEP, "--judge", path, "--cells", str(n)] + ([] if grey else ["--colour"]),
                         capture_output=True, text=True).stdout.split("\n")
    px = bytes_ = "?"
    for l in out:
        if " px, " in l:
            px = l.split(" px,")[0].strip(); bytes_ = l.split(" px,")[1].split(" bytes")[0].strip()
        if l.strip().startswith("wrong"):
            res = l.strip()
    q = subprocess.run([SWEEP, "--match", path], capture_output=True, text=True).stdout
    quality = q.split("quality: ")[1].split(" ")[0] if "quality: " in q else "?"
    print(f"{rid:>5} {when:19} {'sent' if mine else 'recv':4} {name:10} {px:>9} {bytes_:>10}  q{quality} {res}")
