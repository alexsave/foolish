#!/usr/bin/env bash
# THE WASM SOURCE SET, and its hash. The repo's one answer to "which files is a
# wasm module built from", derived from the build system rather than mirrored.
#
# WHO READS IT NOW:
#   c/Makefile             writes the hash into c/build/bots_test.stamp
#   e2e/helpers/bots_test_wasm.ts   compares that stamp to decide whether the
#                          uncommitted test module needs rebuilding
#
# WHAT IT USED TO ALSO DO, and no longer does: `--write` wrote the hash into a
# COMMITTED file, sdk/ts/wasm/WASM_STAMP, next to three committed .wasm.gz
# artifacts. Its job was to let a human prove they had run make, for the case
# that turned up on the 1.1(52) release branch - a real kernel change (four new
# functions in msg_wire.c) whose shipped bytes did not move, because no wasm
# export reaches them and the linker drops them. There was nothing to commit and
# no way to say "I built this", so the stamp became the proof.
#
# That whole problem was downstream of one thing: the artifacts were committed
# and built by hand. They are neither now (scripts/wasm_build.sh), so there is no
# human to take at their word and nothing to prove - a lane that runs the build
# itself does not need a note saying somebody ran the build. `--write`,
# WASM_STAMP and scripts/check_wasm_freshness.sh went together, because they were
# three parts of one workaround.
#
# The hash survives because it answers a question the build still asks, and it is
# a good answer: it is over SOURCES, not output bytes, so it is identical from
# every toolchain (measured e3fa5e8b... from clang 18 on Linux, clang 22 on Linux
# and clang 22 on macOS, on one tree). That is what makes it usable as a
# rebuild key on any machine.
#
# Usage:
#   scripts/wasm_stamp.sh --list    # the source set, one path per line
#   scripts/wasm_stamp.sh --hash    # sha256 over that set's contents
set -euo pipefail
cd "$(dirname "$0")/.."

# BYTE ORDER, NOT THE USER'S LOCALE. The hash is over a SORTED list, so the
# collation `sort` uses is part of the hash. A Mac runs under en_US.UTF-8 and
# CI's Linux under C/POSIX, and the two order the same filenames differently -
# which showed up as a stamp that was correct on the machine that wrote it and
# wrong in CI, with no file in the tree actually differing. Pin it here, once,
# so the stamp means the same thing everywhere.
export LC_ALL=C

sha() {  # one file -> bare hex, on both a Mac and CI's Linux
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# The .c list comes out of the Makefile's own WASM_*_SRC variables - not a
# second copy of them - plus every header they can include.
#
# Two more inputs, and two OUTPUTS, since the layout handshake (c/Makefile
# "Layout hash"):
#   * tools/structgen/structgen.c and specs/*.args decide SG_LAYOUT_HASH, which
#     is compiled into every module - a new hash rule is a new module.
#   * sdk/ts/gen/game_layout.*.ts and layout_hash.*.ts are written by the same
#     make targets; layout_hash.*.ts is what the hosts check each module against
#     (LAYOUT_HASH). They count as wasm outputs: hashing their text here means a
#     module regenerated without a wasm rebuild, or hand-edited, no longer
#     matches the stamp. Unlike the .wasm bytes this text is
#     toolchain-independent (the hash is spelling-free and CI pins LLVM 22 for
#     gen.sh --check), so it is safe to hash.
# --no-print-directory ON BOTH, and it is not decoration. `-s` silences the
# recipes but NOT "make[1]: Entering directory '...'", and make prints those
# whenever MAKELEVEL is set - which it is here, because c/Makefile calls this
# script FROM A RECIPE. Five lines of English then arrived in the middle of a
# list of source paths, prefixed into `c/Entering`, `c/make[1]:` and friends.
#
# It has been doing that all along and was invisible: the hash loop skips a path
# that is not a file, so the hash stayed right and nothing looked wrong.
# check_paths() below is what made it fatal, and that is the correct trade - a
# list of sources that is 5% English is a list nobody can reason about.
sources() {
  # PATHS COME BACK REPO-RELATIVE OR NOT AT ALL. The Makefile's lists are
  # relative to c/, so this prefixes them with `c/` - and since the two shared
  # primitives moved, two of them come back as `../shared/c/*.c` and that prefix
  # produced `c/../shared/c/sha256.c`. That path OPENS FINE, so the hash stayed
  # honest and nothing looked wrong; but two spellings of one file never match
  # when this list is compared against anything else that names it, and the now
  # deleted freshness gate compared it against `git diff --name-only`. Collapse
  # `c/../` the same way the structgen line below collapses its own, and assert
  # the result below.
  make -C c -s --no-print-directory print-wasm-src | tr ' ' '\n' | sed '/^$/d' | sed 's|^|c/|' | sed 's|^c/\.\./||'
  ls c/src/*.h c/wasm/include/* shared/c/*.h 2>/dev/null || true
  # structgen's own source and specs, because the layout hash compiled into
  # every module comes from them. NOT the modules it writes: those are build
  # outputs now, ignored and absent from a fresh checkout, and hashing them
  # would (a) make this script need libclang, which a caller that only wants
  # the source list should not need, and (b) add nothing - they are a function
  # of these two plus the headers above plus the WASM_* lines hash_all already
  # reads out of c/Makefile.
  #
  # ASK STRUCTGEN'S MAKEFILE WHAT STRUCTGEN IS MADE OF, rather than naming its
  # files here. This line used to read `ls tools/structgen/structgen.c`, from
  # when that was the whole program. It is now seven files plus tools/sgcommon,
  # and each time it moved, this list stayed still: measured, appending a line
  # to sg_hash.c left this hash byte-identical while appending one to
  # structgen.c moved it, so the emitter that writes the layout hash could
  # change and a stale module would still read current. That is the precise
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
  make -s --no-print-directory -C shared/tools/structgen -f print.mk -f Makefile \
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
# demand a rebuild.
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

# EVERY PATH IN THE LIST MUST OPEN, FROM THE REPO ROOT. hash_all skips a path
# that is not a file, so a path that is merely openable by some other spelling
# (`c/../shared/...`) is a source this hash has quietly stopped covering - and
# the hash is what decides whether the test module gets rebuilt. A move is
# exactly when that happens, and it is silent, so it is checked rather than
# assumed.
check_paths() {
  bad=$(sources | sort -u | while IFS= read -r f; do [ -e "$f" ] || echo "$f"; done)
  [ -z "$bad" ] && return 0
  echo "wasm_stamp.sh: these source paths do not exist from the repo root:" >&2
  printf '  %s\n' $bad >&2
  echo "(a path that cannot be opened here is skipped by the hash, so an edit to" >&2
  echo " it would not rebuild anything keyed on this - see the header)" >&2
  return 1
}

case "${1:---hash}" in
  --list) check_paths && sources | sort -u ;;
  --hash) check_paths && hash_all ;;
  *) echo "usage: $0 [--list|--hash]" >&2; exit 2 ;;
esac
