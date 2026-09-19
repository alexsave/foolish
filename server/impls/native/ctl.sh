#!/usr/bin/env bash
# ctl.sh - the packed control-plane wire (ctl_wire.h), from shell.
#
# The native server speaks no JSON: signing in, the lobby verbs and every
# answer that is not a view are packed frames now. These scripts used to build
# request bodies with a here-string of JSON and read replies with
# `grep -o '"token":"[^"]*"'` - a third scraper of a format that already had
# two. This file is the shell's copy of the ONE codec, kept deliberately small:
# four bytes of head, then length-prefixed fields.
#
#   [0] version  [1] kind  [2..3] payload length (uint16, little-endian)
#   [4..] payload - a string is [u8 len][len bytes]
#
# Source it and use the ctl_* helpers; nothing here needs to know more of the
# wire than the frames these tests actually send.
#
# Environment:
#   CTL_CURL   the curl invocation to use (default "curl -s"; tls_test.sh and
#              quic_test.sh pass "curl -sk" for their self-signed certs)
#   CTL_TMP    scratch directory for reply bodies (a fresh mktemp -d by default)

CTL_CURL="${CTL_CURL:-curl -s}"
CTL_TMP="${CTL_TMP:-$(mktemp -d /tmp/foolish_ctl.XXXXXX)}"

# The /meta verbs, as ctl_wire.h numbers them.
CTL_META_JOIN=1
CTL_META_START=2
CTL_META_ADD_BOT=3
CTL_META_CONTINUE=4

# ── building ───────────────────────────────────────────────────────────────

# One raw byte. printf's octal escape is the portable way to emit an arbitrary
# byte from a number in bash.
_ctl_byte() { printf "\\$(printf '%03o' "$1")"; }

_ctl_head() {   # <kind> <payload length>
    _ctl_byte 1
    _ctl_byte "$1"
    _ctl_byte $(( $2 & 255 ))
    _ctl_byte $(( ($2 >> 8) & 255 ))
}

_ctl_str() { _ctl_byte "${#1}"; printf '%s' "$1"; }

# CTL_AUTH (kind 0x01): [str username]
ctl_auth_frame() {
    _ctl_head 1 $(( 1 + ${#1} ))
    _ctl_str "$1"
}

# CTL_META (kind 0x02): [u8 verb][str game_id][str strategy]
ctl_meta_frame() {   # <verb> <game_id> [strategy]
    local strat="${3:-}"
    _ctl_head 2 $(( 1 + 1 + ${#2} + 1 + ${#strat} ))
    _ctl_byte "$1"
    _ctl_str "$2"
    _ctl_str "$strat"
}

# ── reading ────────────────────────────────────────────────────────────────

# The n-th (0-based) length-prefixed string in a frame's payload. Walks the
# fields rather than pattern-matching, so a field that happens to contain a
# separator byte cannot confuse it the way it could confuse a grep.
ctl_str_field() {   # <file> <n>
    od -An -v -tu1 "$1" | awk -v want="$2" '
        { for (i = 1; i <= NF; i++) b[n++] = $i }
        END {
            p = 4                                   # past [version][kind][len:u16]
            for (f = 0; ; f++) {
                if (p >= n) exit 1
                l = b[p++]
                if (p + l > n) exit 1
                if (f == want) {
                    s = ""
                    for (i = 0; i < l; i++) s = s sprintf("%c", b[p + i])
                    print s
                    exit 0
                }
                p += l
            }
        }'
}

# One SIGNED byte of the payload, at a 0-based offset. /status answers -1 for a
# game that does not exist, so the sign matters.
ctl_i8_field() {   # <file> <offset>
    od -An -v -tu1 -j $(( 4 + $2 )) -N1 "$1" | awk '{ v = $1; if (v > 127) v -= 256; print v }'
}

# The frame's kind byte, or "" if the file is too short to hold a head.
ctl_kind() { od -An -v -tu1 -j 1 -N1 "$1" | tr -d ' \n'; }

# ── the endpoints these tests use ──────────────────────────────────────────

# POST /auth/signup - prints the session token (CTL_SESSION field 2:
# [user_id][username][token]).
ctl_signup() {   # <base-url> <username>
    local f="$CTL_TMP/session.$$"
    ctl_auth_frame "$2" | $CTL_CURL -XPOST "$1/auth/signup" --data-binary @- -o "$f"
    ctl_str_field "$f" 2
}

# POST /create - prints the game_id (CTL_GAME field 0).
ctl_create() {   # <base-url> <token>
    local f="$CTL_TMP/game.$$"
    $CTL_CURL -XPOST "$1/create" -H "Authorization: Bearer $2" -o "$f"
    ctl_str_field "$f" 0
}

# POST /meta - the reply (CTL_LOBBY) is discarded; callers that care read
# /status instead, same as they always did.
ctl_meta() {   # <base-url> <token> <verb> <game_id> [strategy]
    ctl_meta_frame "$3" "$4" "${5:-}" \
        | $CTL_CURL -XPOST "$1/meta" -H "Authorization: Bearer $2" --data-binary @- -o /dev/null
}

# GET /status - prints the status int (0 waiting / 1 playing / 2 over, -1 for
# no such game).
ctl_status() {   # <base-url> <game_id>
    local f="$CTL_TMP/status.$$"
    $CTL_CURL "$1/status?game_id=$2" -o "$f"
    ctl_i8_field "$f" 0
}
