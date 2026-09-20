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
#
# Two more inputs, and two OUTPUTS, since the layout handshake (c/Makefile
# "Layout hash"):
#   * tools/structgen/structgen.c and specs/*.args decide SG_LAYOUT_HASH, which
#     is compiled into every module - a new hash rule is a new module.
#   * sdk/ts/gen/game_layout.*.ts and layout_hash.*.ts are written by the same
#     make targets; layout_hash.*.ts is what the hosts check each module against
#     (LAYOUT_HASH). They count as wasm outputs: hashing their text here means a
#     module regenerated without a wasm rebuild, or hand-edited, no longer
#     matches the stamp. Unlike the .wasm
#     bytes this text is toolchain-independent (the hash is spelling-free and CI
#     pins LLVM 22 for gen.sh --check), so it is safe to stamp.
sources() {
  # PATHS COME BACK REPO-RELATIVE OR NOT AT ALL. The Makefile's lists are
  # relative to c/, so this prefixes them with `c/` - and since the two shared
  # primitives moved, two of them come back as `../shared/c/*.c` and that prefix
  # produced `c/../shared/c/sha256.c`. That path OPENS FINE, so the hash stayed
  # honest and nothing looked wrong; but check_wasm_freshness.sh `comm`s this
  # list against `git diff --name-only`, which says `shared/c/sha256.c`, and two
  # spellings of one file never match. The gate would have gone on passing while
  # quietly watching neither. So collapse `c/../` the same way the structgen
  # line below collapses its own, and assert the result below.
  make -C c -s print-wasm-src | tr ' ' '\n' | sed '/^$/d' | sed 's|^|c/|' | sed 's|^c/\.\./||'
  ls c/src/*.h c/wasm/include/* shared/c/*.h 2>/dev/null || true
  # structgen's own source and specs, because the layout hash compiled into
  # every module comes from them. NOT the modules it writes: those are build
  # outputs now, ignored and absent from a fresh checkout, and hashing them
  # would (a) make this script need libclang, which the freshness job
  # deliberately does not install, and (b) add nothing - they are a function of
  # these two plus the headers above plus the WASM_* lines hash_all already
  # reads out of c/Makefile.
  #
  # ASK STRUCTGEN'S MAKEFILE WHAT STRUCTGEN IS MADE OF, rather than naming its
  # files here. This line used to read `ls tools/structgen/structgen.c`, from
  # when that was the whole program. It is now seven files plus tools/sgcommon,
  # and each time it moved, this list stayed still: measured, appending a line
  # to sg_hash.c left this hash byte-identical while appending one to
  # structgen.c moved it, so the emitter that writes the layout hash could
  # change and a committed wasm would still read fresh. That is the precise
  # hole this script exists to close, reopened by a refactor rather than by any
  # edit to the kernel - twice in one afternoon, because a list of files is a
  # copy of a fact that lives somewhere else.
  #
  # The Makefile already declares the fact (SG_SRC/SG_HDR and the shared
  # SGC_SRC/SGC_HDR out of tools/llvm.mk) because it has to build from it, so
  # it cannot drift from the program the way a second list can. This is the
  # same move as `make -C c -s print-wasm-src` above: the build system is asked,
  # not mirrored. Paths come back relative to tools/structgen, hence the
  # rewrite of the ../sgcommon ones.
  make -s -C shared/tools/structgen -f print.mk -f Makefile \
       sg-print-SG_SRC sg-print-SG_HDR sg-print-SGC_SRC sg-print-SGC_HDR \
    | tr ' ' '\n' | sed '/^$/d' \
    | sed 's|^|shared/tools/structgen/|' | sed 's|shared/tools/structgen/\.\./|shared/tools/|'
  # llvm.mk moved with the generator; the specs did NOT - they are this
  # product's description of its own structs and stay beside it.
  ls shared/tools/llvm.mk tools/structgen/specs/*.args
}

# The hash covers the source CONTENTS plus the c/Makefile lines that decide what
# the modules are (the WASM_* assignments: flags, caps, source lists). A new
# make target or a comment in that file is not a kernel change and must not
# demand a rebuild - the same line-level rule the gate already applied.
#
# …AND THE --export= LINES, because an export list is a MULTI-LINE value and the
# assignment pattern only ever saw its first line. WASM_API_EXPORTS is fifty
# lines of backslash continuations; adding or dropping an entry left this hash
# byte-identical (measured 2026-09-19: dropping wasm_replay_error_detail moved
# the shipped module by 14 gzip bytes and the stamp by none). An export table IS
# what a module is - it decides both the module's surface and, because an export
# roots its code, what the linker may drop - so a changed one must demand a
# rebuild like any other kernel change. No comment in c/Makefile holds
# `--export=`; the lists are the only thing this matches.
hash_all() {
  { sources | sort -u | while IFS= read -r f; do
      [ -f "$f" ] && printf '%s %s\n' "$(sha "$f")" "$f"
    done
    grep -E '^[A-Za-z0-9_]*WASM[A-Za-z0-9_]*[[:space:]]*[:?+]?=|--export=' c/Makefile | sort
  } | if command -v sha256sum >/dev/null 2>&1; then sha256sum; else shasum -a 256; fi \
    | cut -d' ' -f1
}

# EVERY PATH IN THE LIST MUST OPEN, FROM THE REPO ROOT. The list is compared
# against `git diff --name-only` output, so a path that is merely openable by
# some other spelling (`c/../shared/...`) is a path this gate has stopped
# watching. A move is exactly when that happens, and it is silent, so it is
# checked rather than assumed.
check_paths() {
  bad=$(sources | sort -u | while IFS= read -r f; do [ -e "$f" ] || echo "$f"; done)
  [ -z "$bad" ] && return 0
  echo "wasm_stamp.sh: these source paths do not exist from the repo root:" >&2
  printf '  %s\n' $bad >&2
  echo "(a path that cannot be opened here cannot be matched against git's" >&2
  echo " paths either - check_wasm_freshness.sh would watch nothing)" >&2
  return 1
}

case "${1:---hash}" in
  --list) check_paths && sources | sort -u ;;
  --hash) check_paths && hash_all ;;
  --write)
    check_paths || exit 1
    h=$(hash_all)
    n=$(sources | sort -u | wc -l | tr -d ' ')
    cat > "$STAMP" <<EOF
# The wasm source set the committed artifacts were last built from.
#
# Written by the wasm make targets (make -C c wasm-bots
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
