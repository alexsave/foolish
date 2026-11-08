#!/bin/bash
# cbx_cutover.sh - the cutover proof: foolish at 48af777d3 with c_base/ replaced by ~/Dev/cbase (its
# working tree) mounted at cbase/, the product's moved files and overlay applied, foolish's copies of
# the compile fragments deleted in favour of cbase's, and foolish/server's own Makefile including
# cbase/cbase.mk. Usage: cbx_cutover.sh [setup|check|names|products|all] (default all).
set -u
S="$(cd "$(dirname "$0")" && pwd)"   # the kit folder: the scratch clone is built here as cbx_cutover/
C=$S/cbx_cutover
step=${1:-all}

setup() {
  rm -rf "$C"
  git clone -q --no-hardlinks "$S/survey" "$C" 2>/dev/null
  git -C "$C" rm -q -r c_base
  mkdir -p "$C/cbase"
  rsync -a --exclude '.git' --exclude '/build' --exclude '/example/build' --exclude '/client/build' \
        --exclude '/tools/structgen/build' --exclude '/tools/datagen/build' /Users/alex/Dev/cbase/ "$C/cbase/"
  # Every path into the old directory is a path into cbase/ now (paths only: "c_base/" with its
  # slash; live names such as the cbase stack, table and binaries never held the underscore).
  git -C "$C" grep -l 'c_base/' -- . ':!cbase' | while read -r f; do perl -pi -e 's{c_base/}{cbase/}g' "$C/$f"; done
  # What cbase says about itself, which a product's test reads: the console's title.
  perl -pi -e 's{c_base console}{cbase console}g' "$C/foolish/server/test/unit_admin.c"
  # What moves into foolish (the report's list), then the overlay (the product Makefile and what the
  # product's own files change at the cutover).
  rsync -a "$S/to_foolish/" "$C/foolish/"
  rsync -a "$S/to_foolish_root/" "$C/"
  rsync -a "$S/foolish_overlay/" "$C/"
  python3 "$S/cbx_foolish_patch.py" "$C"
  # The product's helpers by name: cbase's lib/ is on every include path of the server's build.
  (cd "$C/cbase/lib" && find . -name '*.h' | sed 's|^\./||') > "$S/cbx_lib_headers.txt"
  for f in $(grep -rl 'shared/c/' "$C/foolish/server" --include='*.c' --include='*.h'); do
    while read -r h; do
      perl -pi -e 's{#include "(?:\.\./)+shared/c/\Q'"$h"'\E"}{#include "'"$h"'"}g' "$f"
    done < "$S/cbx_lib_headers.txt"
  done
  # The product's name-gate allowlist: its 25 lines, as the product names its files.
  { printf '# name_allow.txt - the name gate'"'"'s allowlist for foolish/server'"'"'s files (cbase/core/test/unit_name_gate.c says the format).\n'
    cat "$S/to_foolish/server/test/name_allow.body"; } > "$C/foolish/server/test/name_allow.txt"
  rm -f "$C/foolish/server/test/name_allow.body"
  # ONE OWNER OF THE COMPILE FRAGMENTS: foolish's copies go, every includer reaches cbase's, saying
  # its C_ROOT (and CB, for the generators and the wasm guard) as cbase's fragments require.
  rm -rf "$C/shared/tools/c_rules.mk" "$C/shared/tools/c_rules_test.sh" "$C/shared/tools/gen_tools.mk" "$C/shared/tools/llvm.mk" \
         "$C/shared/tools/wasm_cc.mk" "$C/shared/tools/structgen" "$C/shared/tools/sgcommon" "$C/shared/scripts/ci_llvm.sh"
  for mk in $(cd "$C" && git grep -l 'include \.\./\.\./shared/tools/\(c_rules\|gen_tools\|wasm_cc\)\.mk' -- '*/Makefile'); do
    perl -0pi -e '
      s{^include \.\./\.\./shared/tools/c_rules\.mk$}{C_ROOT := ../..\ninclude ../../cbase/tools/c_rules.mk}m;
      s{^include \.\./\.\./shared/tools/gen_tools\.mk$}{CB := ../../cbase/\nDATAGEN_DIR := ../../shared/tools/datagen\ninclude ../../cbase/tools/gen_tools.mk}m;
      s{^include \.\./\.\./shared/tools/wasm_cc\.mk$}{CB := ../../cbase/\nWASM_SYSINC := ../../shared/c/wasm/include\ninclude ../../cbase/tools/wasm_cc.mk}m;
    ' "$C/$mk"
  done
  perl -pi -e 's{^include \.\./llvm\.mk$}{include ../../../cbase/tools/llvm.mk}' "$C/shared/tools/datagen/Makefile"
  perl -pi -e 's{bash (\.\./)?shared/scripts/ci_llvm\.sh}{bash $1cbase/tools/ci_llvm.sh}g; s{shared/tools/structgen}{cbase/tools/structgen}g' $(cd "$C" && git ls-files '.github/workflows/*.yml' | sed "s|^|$C/|")
}

check() {
  cd "$C/foolish/server" || exit 2
  date > "$S/cbx_cutover_check.log"
  nice -n 10 make -j2 CHECK_JOBS=2 all check >> "$S/cbx_cutover_check.log" 2>&1
  echo "EXIT $?" >> "$S/cbx_cutover_check.log"
  date >> "$S/cbx_cutover_check.log"
  tail -3 "$S/cbx_cutover_check.log"
}

names() {
  cd "$C/foolish/server" || exit 2
  { ls build/logs | sed 's/^build_test_//; s/\.log$//; s/_build_.*//' | sed 's/^/run /'
    ls build/gate | grep '\.err$' | sed 's/\.err$//; s/^/gate /'
    grep -E '^\./build/[a-z0-9_]+_test$' "$S/cbx_cutover_check.log" | sed 's|^\./build/|shared |'
    grep -q '^\./build/test/unit_client$' "$S/cbx_cutover_check.log" && echo "client unit_client"
    grep -q '^\./build/asan/test/unit_client$' "$S/cbx_cutover_check.log" && echo "client asan_unit_client"
    grep -q '^\./build/asan/fuzz/client fuzz/corpus/client$' "$S/cbx_cutover_check.log" && echo "client fuzz_replay_client"
  } | sort -u > "$S/cbx_cutover_names.txt"
  diff "$S/baseline_names.txt" "$S/cbx_cutover_names.txt" > "$S/cbx_names.diff"
  echo "baseline $(wc -l < "$S/baseline_names.txt") names, cutover $(wc -l < "$S/cbx_cutover_names.txt")"
  cat "$S/cbx_names.diff"
}

# Every other product's Makefile still reads (make -n) on cbase's fragments.
products() {
  for d in foolish/c uttt/c werewolf/c chuiniu/c pickemup/c tallybones/c shared/tools/datagen; do
    if (cd "$C/$d" && make -n > /dev/null 2> "$S/cbx_products_$(echo $d | tr / _).err"); then echo "ok   $d reads"; else echo "FAIL $d"; head -3 "$S/cbx_products_$(echo $d | tr / _).err"; fi
  done
}

case $step in
  setup) setup ;;
  check) check ;;
  names) names ;;
  products) products ;;
  all) setup && check; names; products ;;
esac
