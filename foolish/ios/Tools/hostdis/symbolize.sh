#!/bin/bash
# Read `otool -tV` on stdin and rewrite every `bl 0x<stub>` whose target is an
# objc stub (from objc_stubs, file $1) as `bl "_objc_msgSend$<selector>"`, the
# same spelling otool already uses for binaries whose stubs it can name.
set -euo pipefail
awk -v stubs="${1:?stubs file}" '
BEGIN { while ((getline l < stubs) > 0) { split(l, f, " "); sel[f[1]] = f[2] } }
{
    if (match($0, /\tbl\t0x[0-9a-f]+/)) {
        a = substr($0, RSTART + 6, RLENGTH - 6)
        if (a in sel) { sub(/\tbl\t0x[0-9a-f]+.*/, "\tbl\t\"_objc_msgSend$" sel[a] "\"") }
    }
    print
}'
