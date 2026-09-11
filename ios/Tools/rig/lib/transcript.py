#!/usr/bin/env python3
"""Write a Messages transcript straight into the simulator's sms.db.

WHY THIS EXISTS
---------------
Every Foolish screenshot is a *drawer over a chat*, and until now that chat was
empty: a black void with a raw phone number at the top, because a fresh
simulator has no conversations and there is no supported way to receive one.
The owner's note, verbatim: "id like for you to set up the emulator and text
the two contacts back and forth so there are some 'text bubbles' in the chat
transcript above."

Sending from the simulator only ever produces OUR OWN bubbles (blue, right).
An incoming grey bubble cannot be faked through the UI at all - the simulator
has no iMessage account and nothing ever arrives. So the transcript is written
where Messages reads it from: `Library/SMS/sms.db`.

WHAT MAKES IT SHOW UP
---------------------
Four tables, in this order, or Messages shows an empty thread that still
counts as a conversation (which passes every naive check):

  handle             one row per remote address, (id, service) unique
  chat               the thread; `guid` MUST be "<service>;<-|+>;<identifier>"
  chat_handle_join   who is in it
  message            the bubbles; `date` is nanoseconds since 2001-01-01 UTC
  chat_message_join  which bubbles are in it, with `message_date` repeated

`imagent` owns this file while it runs and keeps its own cache, so the write
only sticks if imagent and MobileSMS are BOTH dead first, and the -wal/-shm
are checkpointed after. `rig.sh transcript` does that around this script; do
not call it directly on a running simulator and believe the result.

CONTACT NAMES come free: a stock simulator ships Apple's own address book
(Kate Bell, Daniel Higgins, John Appleseed, Anna Haro, Hank Zakroff, David
Taylor), so a handle that matches one of their numbers renders as a NAME in
the navigation bar and in group avatars. That is why the cast below is those
people and not invented ones - an invented number renders as "+1 (555)..."
in the one place a store screenshot most wants a human name.
"""
import argparse
import os
import sqlite3
import subprocess
import sys
import time
import uuid

APPLE_EPOCH = 978307200  # 2001-01-01 UTC, in unix seconds

# The cast. `handle` is what iMessage addresses; `first` is only used to build
# the matching seat name for the board, so a bubble from Kate sits above a
# board whose seat says "Kate".
CAST = [
    ("Kate",   "+14155553695"),
    ("Daniel", "+14085555270"),
    ("John",   "+18885551212"),
    ("Anna",   "+15555228243"),
    ("Hank",   "+17075551854"),
    ("David",  "+15556106679"),
]

ME = "alex@foolish.cards"  # the local account; never rendered, only stored


def ns(unix_seconds: float) -> int:
    return int((unix_seconds - APPLE_EPOCH) * 1_000_000_000)


def sms_db(sim: str) -> str:
    return os.path.expanduser(
        f"~/Library/Developer/CoreSimulator/Devices/{sim}/data/Library/SMS/sms.db")


def connect(sim: str) -> sqlite3.Connection:
    path = sms_db(sim)
    if not os.path.exists(path):
        sys.exit(f"no sms.db at {path} - boot the simulator and open Messages once")
    db = sqlite3.connect(path)
    # sms.db's own triggers call functions that only exist inside Apple's
    # process (`verify_chat` validates a chat guid, `is_mic_enabled` gates the
    # CloudKit sync bookkeeping, and the attachment ones unlink files). Plain
    # sqlite3 has never heard of them, so an INSERT fails with "no such
    # function" before it writes a byte. Registering harmless stand-ins is
    # what lets the write go through: `verify_chat` returns the guid it was
    # handed (we build it in Apple's own "<service>;<-|+>;<id>" form anyway),
    # and everything else answers "nothing to do".
    db.create_function("verify_chat", 1, lambda g: g)
    db.create_function("guid_for_chat", 3, lambda a, b, c: f"{b};-;{a}")
    db.create_function("is_mic_enabled", 0, lambda: 0)
    db.create_function("after_delete_message_plugin", -1, lambda *a: None)
    db.create_function("before_delete_attachment_path", -1, lambda *a: None)
    db.create_function("delete_attachment_path", -1, lambda *a: None)
    db.create_function("delete_chat_background_before_deleting_chat", -1,
                       lambda *a: None)
    # A -wal left behind by imagent is the difference between "the rows are
    # there" and "Messages sees them". Fold it in on the way out.
    db.execute("PRAGMA journal_mode=WAL")
    return db


def upsert_handle(db, address: str) -> int:
    row = db.execute("SELECT ROWID FROM handle WHERE id=? AND service='iMessage'",
                     (address,)).fetchone()
    if row:
        return row[0]
    cur = db.execute(
        "INSERT INTO handle (id, country, service, uncanonicalized_id) "
        "VALUES (?, 'us', 'iMessage', ?)", (address, address))
    return cur.lastrowid


def upsert_chat(db, addresses, display_name=None) -> int:
    """1:1 (style 45) for one address, group (style 43) for more."""
    group = len(addresses) > 1
    if group:
        room = f"chat{uuid.uuid4().hex[:16]}"
        guid = f"iMessage;+;{room}"
        ident = room
    else:
        room = None
        guid = f"iMessage;-;{addresses[0]}"
        ident = addresses[0]
    row = db.execute("SELECT ROWID FROM chat WHERE guid=?", (guid,)).fetchone()
    if row:
        chat_id = row[0]
    else:
        cur = db.execute(
            "INSERT INTO chat (guid, style, state, account_id, chat_identifier, "
            "service_name, room_name, account_login, is_archived, "
            "last_addressed_handle, display_name, group_id, is_filtered, "
            "successful_query) "
            "VALUES (?, ?, 3, ?, ?, 'iMessage', ?, ?, 0, ?, ?, ?, 0, 1)",
            (guid, 43 if group else 45, str(uuid.uuid4()).upper(), ident,
             room, f"E:{ME}", ME, display_name,
             str(uuid.uuid4()).upper()))
        chat_id = cur.lastrowid
    for a in addresses:
        hid = upsert_handle(db, a)
        db.execute("INSERT OR IGNORE INTO chat_handle_join (chat_id, handle_id) "
                   "VALUES (?, ?)", (chat_id, hid))
    return chat_id


def add_message(db, chat_id: int, text: str, when: float, from_me: bool,
                address: str | None = None) -> int:
    """One bubble. `address` names the sender for an incoming message; for an
    outgoing one it names the recipient, which is what `other_handle` wants."""
    hid = upsert_handle(db, address) if address else 0
    t = ns(when)
    cur = db.execute(
        "INSERT INTO message (guid, text, handle_id, service, account, "
        "account_guid, date, date_read, date_delivered, is_delivered, "
        "is_finished, is_from_me, is_read, is_sent, item_type, type, "
        "was_data_detected, is_empty, error, share_status, share_direction, "
        "expire_state, message_source, part_count) "
        "VALUES (?, ?, ?, 'iMessage', ?, ?, ?, ?, ?, 1, 1, ?, 1, ?, 0, 0, "
        "0, 0, 0, 0, 0, 0, 0, 1)",
        (str(uuid.uuid4()).upper(), text, hid, f"E:{ME}",
         str(uuid.uuid4()).upper(), t, t if not from_me else 0,
         t if from_me else 0, 1 if from_me else 0, 1 if from_me else 0))
    mid = cur.lastrowid
    db.execute("INSERT INTO chat_message_join (chat_id, message_id, message_date) "
               "VALUES (?, ?, ?)", (chat_id, mid, t))
    return mid


# The banter. Short, ordinary, and about the game without ever explaining it -
# a store screenshot's transcript is scenery, and scenery that reads as ad copy
# is the fastest way to make a shot look staged.
SCRIPT_1TO1 = [
    (False, "are you around? one game before dinner"),
    (True,  "always. deal me in"),
    (False, "you still owe me from last time"),
    (True,  "that was one hand and you know it"),
    (False, "sending"),
]

SCRIPT_GROUP = [
    (0, "table's open, who's in"),
    (1, "in"),
    (2, "give me two minutes"),
    (-1, "starting without John again"),
    (2, "rude"),
    (3, "in"),
    (4, "in"),
]


def build(sim: str, players: int, reset: bool) -> None:
    db = connect(sim)
    if reset:
        for t in ("chat_message_join", "chat_handle_join", "message",
                  "chat", "handle", "attachment", "message_attachment_join"):
            try:
                db.execute(f"DELETE FROM {t}")
            except sqlite3.OperationalError:
                pass

    now = time.time()
    if players <= 2:
        who = CAST[0]
        chat_id = upsert_chat(db, [who[1]])
        # Walk backwards from ~11 minutes ago so the last bubble is recent but
        # the thread still reads as a conversation rather than a burst.
        for i, (mine, text) in enumerate(SCRIPT_1TO1):
            add_message(db, chat_id, text, now - 660 + i * 95, mine, who[1])
        label = f"1:1 with {who[0]} ({who[1]})"
    else:
        members = CAST[:min(players - 1, len(CAST))]
        chat_id = upsert_chat(db, [m[1] for m in members], display_name=None)
        for i, (whoi, text) in enumerate(SCRIPT_GROUP):
            mine = whoi < 0
            addr = None if mine else members[whoi % len(members)][1]
            add_message(db, chat_id, text, now - 900 + i * 105, mine, addr)
        label = "group of %d (%s)" % (len(members) + 1,
                                      ", ".join(m[0] for m in members))

    db.commit()
    db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    written = db.total_changes
    db.close()
    print(f"transcript: {label}, {written} rows written")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", default=os.environ.get("FOOLISH_SIM", ""))
    ap.add_argument("--players", type=int, default=2,
                    help="2 writes a 1:1 thread; 3+ writes a group of that size")
    ap.add_argument("--keep", action="store_true",
                    help="add to the existing transcript instead of replacing it")
    a = ap.parse_args()
    if not a.sim:
        sys.exit("set FOOLISH_SIM or pass --sim")
    build(a.sim, a.players, reset=not a.keep)


if __name__ == "__main__":
    main()
