#!/usr/bin/env bash
# The wasm source set, and its hash - the one derivation both the make targets
# and scripts/check_wasm_freshness.sh read.
#
# WHY THIS FILE EXISTS. The freshness gate's rule used to be "a branch that
# touches a wasm source must also update a committed artifact", which cannot
# express the case that turned up on the 1.1(52) release branch: the kernel
# change was REAL (four new functions in msg_wire.c), the rebuild was real, and
# the shipped bytes did not move by one byte - the new symbols are reachable
# from no wasm export, so the linker drops them. There was nothing to commit,
# and no way to prove the build had been run. The alternatives were both bad:
# exempt msg_wire.c forever (blinding the gate to the drift it exists to catch)
# or commit an artifact nobody rebuilt.
#
# So the artifacts now carry a STAMP of the sources they were last built from.
# "I ran the build" becomes a fact in the diff whether or not the bytes moved,
# and the gate gets STRONGER rather than weaker: commit order could be satisfied
# by any artifact touch, while the stamp is only satisfied by the sources that
# are actually in the tree.
#
# The stamp is over SOURCES, not over output bytes, and that is deliberate - the
# same reason check_wasm_freshness.sh gives for not diffing bytes: a rebuild on a
# different toolchain legitimately produces different bytes (measured on this
# repo: bots.wasm.gz moved 83 B on an untouched tree across clang versions), so a
# byte stamp would be red on every machine but one. Sources are the thing every
# machine agrees on.
#
# Usage:
#   scripts/wasm_stamp.sh --list    # the source set, one path per line
#   scripts/wasm_stamp.sh --hash    # sha256 over that set's contents
#   scripts/wasm_stamp.sh --write   # write sdk/ts/wasm/WASM_STAMP
set -euo pipefail
cd "$(dirname "$0")/.."

# BYTE ORDER, NOT THE USER'S LOCALE. The hash is over a SORTED list, so the
# collation `sort` uses is part of the hash. A Mac runs under en_US.UTF-8 and
# CI's Linux under C/POSIX, and the two order the same filenames differently -
# which showed up as a stamp that was correct on the machine that wrote it and
# wrong in CI, with no file in the tree actually differing. Pin it here, once,
# so the stamp means the same thing everywhere.
export LC_ALL=C

STAMP=sdk/ts/wasm/WASM_STAMP

sha() {  # one file -> bare hex, on both a Mac and CI's Linux
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# The .c list comes out of the Makefile's own WASM_*_SRC variables - not a
# second copy of them - plus every header they can include. Same derivation
# check_wasm_freshness.sh used to do inline; it calls --list now.
sources() {
  make -C c -s print-wasm-src | tr ' ' '\n' | sed '/^$/d' | sed 's|^|c/|'
  ls c/src/*.h c/wasm/include/* 2>/dev/null || true
}

# The hash covers the source CONTENTS plus the c/Makefile lines that decide what
# the modules are (the WASM_* assignments: flags, caps, source lists). A new
# make target or a comment in that file is not a kernel change and must not
# demand a rebuild - the same line-level rule the gate already applied.
hash_all() {
  { sources | sort -u | while IFS= read -r f; do
      [ -f "$f" ] && printf '%s %s\n' "$(sha "$f")" "$f"
    done
    grep -E '^[A-Za-z0-9_]*WASM[A-Za-z0-9_]*[[:space:]]*[:?+]?=' c/Makefile | sort
  } | if command -v sha256sum >/dev/null 2>&1; then sha256sum; else shasum -a 256; fi \
    | cut -d' ' -f1
}

case "${1:---hash}" in
  --list) sources | sort -u ;;
  --hash) hash_all ;;
  --write)
    h=$(hash_all)
    n=$(sources | sort -u | wc -l | tr -d ' ')
    cat > "$STAMP" <<EOF
# The wasm source set the committed artifacts were last built from.
#
# Written by the wasm make targets (make -C c wasm wasm-guards wasm-bots
# wasm-oracle wasm-oracle-mt), read by scripts/check_wasm_freshness.sh. It is
# how a rebuild that changes no shipped byte still proves it happened - see the
# header of scripts/wasm_stamp.sh.
#
# Do not hand-edit. If this line disagrees with the tree, the artifacts beside
# it were built from different C than the C you are looking at.
sources $n
sha256 $h
EOF
    echo "wasm stamp: $h ($n sources)" ;;
  *) echo "usage: $0 [--list|--hash|--write]" >&2; exit 2 ;;
esac
