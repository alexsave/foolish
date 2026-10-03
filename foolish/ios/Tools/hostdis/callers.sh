#!/bin/bash
# List the methods that send a selector, from dump.sh output.
#
#   ios/Tools/hostdis/callers.sh <tag.name.text.txt> <selector> [tag.name.objc.txt]
#
# Prints "<count> <enclosing method>" for each method containing a call to
# `_objc_msgSend$<selector>`. Selector arguments must match exactly, colons
# included. Without the objc file the enclosing method is otool's own label,
# which a stripped binary (the host plugin) does not have; pass the objc file
# and each call is attributed to the nearest method start at or below it.
# This is how the report's caller claims (for example M2c, the
# setCurrentViewController: call sites) were counted.
set -euo pipefail
text=${1:?text dump}; sel=${2:?selector}; objc=${3:-/dev/null}
awk -v want="\"_objc_msgSend\$$sel\"" -v objc="$objc" '
function hex(s,   i, c, v) { v = 0; s = tolower(s)
    for (i = 1; i <= length(s); i++) { c = index("0123456789abcdef", substr(s, i, 1)) - 1; v = v * 16 + c }
    return v }
BEGIN { while ((getline l < objc) > 0)
    if (match(l, /0x[0-9A-Fa-f]+  [-+]\[.*\]/)) { a[++na] = hex(substr(l, RSTART + 2, index(substr(l, RSTART), " ") - 3)); m[na] = substr(l, RSTART + index(substr(l, RSTART), " ") + 1) } }
/^[-+]\[.*\]:$/ { fn = $0 }
index($0, want) {
    who = fn
    if (na) { at = hex(substr($0, 1, 16)); best = -1
        for (i = 1; i <= na; i++) if (a[i] <= at && (best < 0 || a[i] > a[best])) best = i
        who = best < 0 ? "?" : m[best] }
    n[who]++ }
END { for (f in n) print n[f], f }' "$text" | sort -k2
