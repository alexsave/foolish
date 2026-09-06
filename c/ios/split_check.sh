#!/bin/sh
# The bridge's split, asserted on the object file.
#
# Foolish.xcframework is a static archive, so ld links it per OBJECT: a binary
# that never calls a bot entry leaves the whole strategy ladder out - but ONLY
# while ios_api.c names no bot symbol. One convenience call put back in the core
# half (a bot_roster_find to fill some default, say - which is exactly what used
# to be in fio_new_game) silently re-links 21 brains, cordite's simulator and
# the roster into the iMessage extension, and nothing else would notice.
#
# Measured, at the time this was written: a binary calling only the message
# entries linked 0 ladder symbols at 139,640 bytes; the same binary plus one
# fio_bot_drive_packed call linked 47 at 273,592.
#
# Usage: ios/split_check.sh <ios_api.o>
set -e
OBJ="$1"
[ -n "$OBJ" ] || { echo "usage: split_check.sh <ios_api.o>" >&2; exit 2; }

# The doors into the bot module. Not a list of every ladder symbol: these are
# what a caller in the core half would actually reach for, and each one drags
# the rest.
BAD='bot_drive|bot_roster_|bot_cycle_delay_ms|bot_knob|_strategy_choose|cordite_|octogen_|leafbook'

# `nm -u` is the undefined list - what this object asks the linker to find.
# should_bot_act and random_strategy_set_seed are NOT here on purpose: both are
# defined in src/game.c, which every binary links anyway.
HITS=$(nm -u "$OBJ" 2>/dev/null | grep -oE "$BAD[A-Za-z0-9_]*" | sort -u || true)

if [ -n "$HITS" ]; then
    echo "ios_api.c references the bot module:" >&2
    echo "$HITS" | sed 's/^/  /' >&2
    echo "" >&2
    echo "  -> that re-links the whole strategy ladder into every binary using ANY" >&2
    echo "     part of the bridge, the iMessage extension included. Move the entry" >&2
    echo "     to ios/ios_bots_api.c (see ios/ios_internal.h)." >&2
    exit 1
fi
echo "ios bridge split ok (ios_api.o names no bot symbol)"
