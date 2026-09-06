#!/bin/sh
# The two shipped libraries, asserted.
#
# libfoolish.a is what FoolishKit links, and FoolishKit ships inside
# FoolishMessagesApp - so anything in this archive that the extension's link
# reaches is in the iMessage bundle. The whole point of the split is that the
# strategy ladder is not in it. octogen in particular is the strongest brain in
# the roster and the one worth reverse-engineering.
#
# Checked on the ARCHIVE rather than on a linked binary because the archive is
# what ships: a link test only proves what one caller happened to reach today.
#
# Usage: ios/archive_check.sh <libfoolish.a> <libfoolishbots.a>
set -e
CORE="$1"; BOTS="$2"
[ -n "$CORE" ] && [ -n "$BOTS" ] || { echo "usage: archive_check.sh <core.a> <bots.a>" >&2; exit 2; }

LADDER='octogen|cordite|semtex|torpex|novichok|astrolite|espresso|robusta|firecracker|blackpowder|gunpowder|handwritten|simple_heuristic|champion|hacker|fulminate|distill|leafbook|bot_roster|bot_drive|bot_knob'

# Symbols DEFINED by the core archive. random_strategy_set_seed is defined in
# game.c, not a strategy file, so it is core by construction and not matched
# by the pattern above.
HITS=$(nm "$CORE" 2>/dev/null | grep -E ' [TtDdBbRrSs] ' | grep -oE "($LADDER)[A-Za-z0-9_]*" | sort -u || true)
if [ -n "$HITS" ]; then
    echo "the CORE library defines strategy-ladder symbols:" >&2
    echo "$HITS" | sed 's/^/  /' >&2
    echo "" >&2
    echo "  -> FoolishKit links this archive and ships inside FoolishMessagesApp," >&2
    echo "     so these are in the iMessage bundle. Move the file to IOS_BOTS_SRC." >&2
    exit 1
fi

# And the bots archive must actually HAVE them - a split that quietly shipped
# an empty bots library would pass the check above and break the host app.
OCTO=$(nm "$BOTS" 2>/dev/null | grep -cE ' [TtDdBbRrSs] .*octogen' || true)
if [ "$OCTO" -eq 0 ]; then
    echo "the BOTS library defines no octogen symbol - the ladder went missing," >&2
    echo "  not into the other archive. Check IOS_BOTS_SRC." >&2
    exit 1
fi

echo "ios archives ok (core: 0 ladder symbols; bots: $OCTO octogen symbols)"
