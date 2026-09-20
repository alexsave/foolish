#!/usr/bin/env bash
# Regenerate every structgen output with the libclang tool, one run per build.
# Layout flags come from c/Makefile itself, so a -D cap change there is picked up.
#
#   sdk/ts/gen/             the modules production TS imports: accessors, and
#                           layout_hash.<build>.ts, the LAYOUT_HASH each wasm
#                           module is checked against (its own module, so the
#                           browser can check it without importing a reader of
#                           the unmasked Game - e2e/security_client_boundary)
#   sdk/kotlin/gen/         the module a JVM client compiles: the same value
#                           snapshots again, over the bytes JNI hands across
#                           rather than over an address the JVM cannot hold.
#                           Generated for an Android triple and the headers'
#                           DEFAULT caps (specs/android_layout.args says why),
#                           and it carries its own sgCheckLayout.
#   sdk/swift/gen/          the module FoolishKit compiles: value snapshots of
#                           the structs iOS reads out of the kernel it LINKS,
#                           generated for the iOS caps and the iOS triple. Its
#                           SG_LAYOUT_HASH is the one c/Makefile bakes into the
#                           library (`make ios-lib`), so a stale xcframework or
#                           a stale module is a startup refusal, never a wrong
#                           offset (sdk/swift/KernelLayout.swift).
#   tools/structgen/gen/    the generator's own genericity fixtures
#
#   gen.sh                 write all four. THIS RUNS AS PART OF EVERY BUILD -
#                          `npm run build`, `typecheck` and every test lane go
#                          through it (package.json, the `gen` script and its
#                          pre-hooks), so the modules a lane compiles are the
#                          ones this tree's headers describe.
#   gen.sh --verify-wasm   …and link test/verify.c into build/verify.wasm, which
#                          test/verify.test.ts reads. Separate because that link
#                          needs wasm-ld, and a lane that only needs the modules
#                          should only need libclang.
#   gen.sh --check         generate TWICE, as two separate processes into two
#                          temp trees, and refuse a difference. The generator
#                          has to be a function of this tree and of nothing else.
#   gen.sh --print-dirs    the output directories, repo-relative. Needs no
#                          toolchain: it is how the gate below asks what is
#                          generated instead of keeping its own list.
#
# NOTHING HERE IS COMMITTED, and the argument is one this repo has already paid
# for. `verify.wasm` sat committed under gen/ with `diff -x verify.wasm` excusing
# it from the freshness check, because its bytes really are the toolchain's; the
# exclusion is what let it rot, still holding the layouts anim_plan.h had before
# AnimPlan grew while `gen.sh --check` reported everything fresh (f6338351). An
# artifact nothing compares is an artifact nothing keeps fresh, and the cheapest
# way to have nothing to compare is to have nothing committed. So these are
# ignored build outputs, written by every lane that consumes them, and
# e2e/validation/generated_outputs_validation.test.ts refuses a tracked one.
#
# What --check USED to do was diff the committed copies against a fresh run.
# With nothing committed there is no stale copy to find, so it proves the other
# half instead: that two runs agree. A generator that ordered a hash table by
# address, or wrote a timestamp, would hand two lanes of the same commit two
# different layouts, and only the LAYOUT_HASH handshake would ever notice. It is
# the check gen.sh already makes of verify.wasm, applied to the modules.
#
# That is safe only because the generated TEXT does not depend on which libclang
# writes it. Measured on this tree, byte-identical including both layout hashes:
# Homebrew clang 22.1.8 (macOS arm64), apt.llvm.org clang 22.1.8 (Linux x86_64
# and aarch64) and Ubuntu clang 18.1.3. test/hash.sh independently pins the hash
# itself to layout facts only, which is the second line of defence.
#
# game_layout.<build>.ts and layout_hash.<build>.ts are ALSO written by the wasm make targets (c/Makefile,
# "Layout hash"), from the same specs/game_layout.args and the same flags, so
# `make -C c wasm-bots` after a header edit leaves the module and the wasm in
# agreement. This script is the full regeneration and the CI check.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"

# THE GENERATOR IS SHARED, ITS CONFIGURATION IS NOT. structgen and datagen build
# for both products in this monorepo and live in shared/tools; what they are
# POINTED AT - specs/*.args, this script, and the product test fixtures - is
# foolish's and stays beside it. So two roots below: "$sg" is the tool, "$here"
# is this product's configuration of it.
sg="$root/shared/tools/structgen"

# The output directories, repo-relative, as ONE fact. `gen.sh --print-dirs`
# answers with them and needs no toolchain, so the gate that refuses a tracked
# structgen output (e2e/validation/generated_outputs_validation.test.ts) asks
# the generator what it writes instead of keeping its own list. Printed before
# anything is built, for the same reason.
OUT_DIRS="sdk/ts/gen
sdk/swift/gen
sdk/kotlin/gen
tools/structgen/gen"
if [ "${1:-}" = "--print-dirs" ]; then printf '%s\n' "$OUT_DIRS"; exit 0; fi

CLANG="${WASM_CC:-/opt/homebrew/opt/llvm/bin/clang}"
make -s -C "$sg" build/structgen
SG="$sg/build/structgen"
flags() { make -s -C "$root/c" -f Makefile -f "$sg/print.mk" "sg-print-$1"; }
spec() { grep -v '^[[:space:]]*#' "$here/specs/$1.args"; }
BOTS="$(flags WASM_BOT_CFLAGS)"
# …and the same four as absolute paths, read back from the one list above so
# the two cannot drift.
prod="$root/$(printf '%s\n' "$OUT_DIRS" | sed -n 1p)"
swift="$root/$(printf '%s\n' "$OUT_DIRS" | sed -n 2p)"
kotlin="$root/$(printf '%s\n' "$OUT_DIRS" | sed -n 3p)"
fixtures="$root/$(printf '%s\n' "$OUT_DIRS" | sed -n 4p)"
# Where the two --check runs are told to write. Not for general use: the whole
# point of this script is that a build writes the real thing.
if [ -n "${SG_OUT_ROOT:-}" ]; then
  prod="$SG_OUT_ROOT/ts"; swift="$SG_OUT_ROOT/swift"; kotlin="$SG_OUT_ROOT/kotlin"; fixtures="$SG_OUT_ROOT/fixtures"
fi
check=0
case "${1:-}" in
  ""|--check|--verify-wasm) ;;
  *) echo "gen.sh: unknown argument '$1' (want nothing, --check, --verify-wasm or --print-dirs)" >&2; exit 2 ;;
esac
[ "${1:-}" = "--check" ] && check=1
mkdir -p "$prod" "$fixtures" "$swift" "$kotlin"

# The resident Game prefix the TS marshal reads and writes, per wasm build.
set -f   # the spec is split on whitespace, never globbed
# shellcheck disable=SC2207
GAME=(--cwd "$root/c" $(spec game_layout))
set +f
"$SG" "${GAME[@]}" --build "bots=$BOTS" --ts "$prod/game_layout.bots.ts" --hash-ts "$prod/layout_hash.bots.ts"
"$SG" --cwd "$root/c" --header anim_plan.h --header legal.h --build "bots=$BOTS" \
  --root AnimPlan --root AnimFrame --root AnimBeats --root AnimEvent --root LegalMoves \
  --const ANIM_TIME_MS --const ANIM_GAP_MS --const ANIM_STEP_NONE --const ANIM_NEVER \
  --const ANIM_EVT_ --const ANIM_LOC_ --const ANIM_CONFLICT_ --const ANIM_SEAT_NONE \
  --ts "$prod/anim.bots.ts"

# The web client's reader of its slot (c/src/client_table.h): snapshot readers
# only, no accessor over the struct (specs/view_layout.args).
set -f
# shellcheck disable=SC2207
VIEW=(--cwd "$root/c" $(spec view_layout))
set +f
"$SG" "${VIEW[@]}" --build "bots=$BOTS" --ts "$prod/view_layout.bots.ts"

# The FMSG bridge's header (c/src/msg_wire.h MsgHeader): a snapshot reader, a
# writer and the codec's constants (specs/msg_layout.args).
set -f
# shellcheck disable=SC2207
MSG=(--cwd "$root/c" $(spec msg_layout))
set +f
"$SG" "${MSG[@]}" --build "bots=$BOTS" --ts "$prod/msg_layout.bots.ts"

# The Oracle's Mode B candidate table (c/src/oracle_mt.h), read back from
# oracle-mt.wasm (specs/oracle_layout.args).
set -f
# shellcheck disable=SC2207
ORACLE=(--cwd "$root/c" $(spec oracle_layout))
set +f
"$SG" "${ORACLE[@]}" --build "oracle_mt=$(flags WASM_ORACLE_MT_CFLAGS)" --ts "$prod/oracle_layout.oracle_mt.ts"

# The structs iOS reads out of the kernel it LINKS (specs/ios_layout.args), as
# Swift value snapshots. Its own caps and its own triple: neither is the wasm
# build's, and the layouts really do differ - a pointer alone is 4 bytes there
# and 8 here.
set -f
# shellcheck disable=SC2207
IOS=(--cwd "$root/c" $(spec ios_layout))
set +f
"$SG" "${IOS[@]}" --build "ios=$(flags IOS_CFLAGS)" --target "$(flags IOS_LAYOUT_TRIPLE)" \
  --swift "$swift/kernel.ios.swift"

# The same structs again for a JVM client (specs/android_layout.args), as Kotlin
# value snapshots over a ByteBuffer. Its own triple, and the headers' DEFAULT
# caps rather than a borrowed set: c/Makefile has no Android flags to read, and
# IOS_CAPS is shrunk for an iMessage extension's memory ceiling, which an app
# does not have. A cap mismatch between this module and an NDK build of the
# kernel is what the generated sgCheckLayout refuses at startup.
#
# ONE ABI, and aarch64 is it, because that is what ships. The other Android ABIs
# lay these structs out identically - there is no pointer in them, which is why
# Kotlin can read them at all - but plain `char` is unsigned on ARM and signed
# on x86, and the layout hash carries a scalar's kind. So an x86_64 run writes
# the same readers under a different hash; test/kotlin.sh pins exactly that, and
# an emulator build has to be stamped from its own run.
ANDROID_TRIPLE="aarch64-linux-android21"
set -f
# shellcheck disable=SC2207
ANDROID=(--cwd "$root/c" $(spec android_layout))
set +f
"$SG" "${ANDROID[@]}" --build "android=-O2 -Isrc" --target "$ANDROID_TRIPLE" \
  --kotlin "$kotlin/Kernel.kt" --kotlin-package cards.foolish.kernel

# ---- the translated strings (shared/tools/datagen) --------------------------
#
# A SIBLING TOOL, not a structgen flag. structgen asks clang for shape - every
# offset, size and stride - and never reads a value; datagen asks for contents
# and never reads a layout. They share this driver, tools/llvm.mk and the
# libclang it finds, and nothing else. See shared/tools/datagen/datagen.c.
#
# The source is c/i18n: one keys.h, one registry, and one strings_<code>.c per
# language. NONE OF IT IS COMPILED INTO ANYTHING SHIPPED - c/i18n is outside
# c/src and appears in no *_SRC list, because twenty-five languages is around
# 150 KB of string data and bots.wasm.gz is downloaded by every visitor.
# The gate is e2e/validation/i18n_source_of_truth.test.ts, whose first test
# ('no translated string reaches the shipped wasm') gunzips the shipped module
# and searches it rather than trusting this comment.
#
# ONE MODULE PER LANGUAGE, and that is a bundle decision, not tidiness: a
# dynamic import keeps every export of its target alive in the web bundle
# whatever the importer uses (src/wasm/msgKernel.ts learned this the expensive
# way), so the module the site imports for a language has to BE one language.
make -s -C "$root/shared/tools/datagen" build/datagen
DG="$root/shared/tools/datagen/build/datagen"
i18n_ts="$prod/i18n"; i18n_swift="$swift/i18n"; i18n_kotlin="$kotlin/i18n"
mkdir -p "$i18n_ts" "$i18n_swift" "$i18n_kotlin"
dg() { "$DG" --cwd "$root/c/i18n" "$@"; }

# The registry first: what languages there are, what each calls itself, and
# which way it is written. A table of structs, so datagen reads its columns by
# field name - the case that proves this tool is not string-table-shaped.
dg --header languages.h --table FS_LANGUAGES --require-complete --name FoolishLanguages \
   --ts "$i18n_ts/languages.ts" --swift "$i18n_swift/FoolishLanguages.swift" \
   --kotlin "$i18n_kotlin/FoolishLanguages.kt" --kotlin-package cards.foolish.i18n
# Every key that exists, in one list. --ts-const so TypeScript keeps them as
# LITERAL types: the website's `StringId` union is derived from this array
# (src/localization/strings.ts), so the set of keys a call site may ask for is
# the set the C declares, checked by tsc, and not a second list to fall behind.
dg --header keys.h --table FS_KEY_NAME --require-complete --name FoolishStringKeys --ts-const \
   --ts "$i18n_ts/keys.ts" --swift "$i18n_swift/FoolishStringKeys.swift" \
   --kotlin "$i18n_kotlin/FoolishStringKeys.kt" --kotlin-package cards.foolish.i18n

# …and one module per language. THE LIST COMES FROM THE REGISTRY, read back out
# of the C, so adding a language is adding its file and its row and nothing
# else. A row whose strings_<code>.c is missing fails here, by name.
dg --header languages.h --table FS_LANGUAGES --json "$i18n_ts/.languages.json"
codes="$(sed -n 's/.*"code": "\([a-z][a-z]*\)".*/\1/p' "$i18n_ts/.languages.json")"
rm -f "$i18n_ts/.languages.json"
[ -n "$codes" ] || { echo "gen: the language registry (c/i18n/languages.h) read back empty" >&2; exit 1; }
for code in $codes; do
  up="$(printf '%s' "$code" | tr '[:lower:]' '[:upper:]')"
  cap="$(printf '%s%s' "$(printf '%s' "${code%"${code#?}"}" | tr '[:lower:]' '[:upper:]')" "${code#?}")"
  # --require-complete is the load-bearing flag. One 25-row table made a missing
  # key a compile error; twenty-five independent tables would make it a silent
  # empty string on a board in a language nobody here reads. This is what took
  # that job over, and it names every key it cannot find.
  #
  # Every language carries every key, so the strict form is what runs here.
  # datagen also has --require-complete-if TABLE.COLUMN, which requires only the
  # slots a companion column marks - the shape for a table that is allowed to be
  # partly translated. Nothing needs it today (shared/tools/datagen/test/cli.sh
  # covers it), and the day a key lands that cannot be translated in one commit, it is
  # the flag to reach for instead of letting a hole through unnoticed.
  dg --header "strings_$code.c" --table "FS_STRINGS_$up" --labels "FS_STRINGS_$up.0=FS_KEY_NAME" \
     --require-complete --name "FoolishStrings$cap" \
     --ts "$i18n_ts/strings.$code.ts" --swift "$i18n_swift/FoolishStrings$cap.swift" \
     --kotlin "$i18n_kotlin/FoolishStrings$cap.kt" --kotlin-package cards.foolish.i18n
done

# Genericity fixtures (test/verify.test.ts).
"$SG" --cwd "$sg/test" --header kinds.h --root Kinds --build wasm= --const K_ --const KFLAG_ --ts "$fixtures/kinds.ts"
"$SG" --cwd "$sg/test" --header snap.h --root Snap --root SPtr --build wasm= --snapshot Snap --snapshot SPtr --snapshot-only \
  --count Snap.pairs=n_pairs --count Snap.items=n_items --count Snap.text=n_text --count SItem.text=len --writer Snap \
  --count SPtr.vals=n_vals --count SPtr.items=n_items --count SPtr.name=name_len --count SPtr.none=n_none --ts "$fixtures/snap.ts"

if [ "$check" = 1 ]; then
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
  # Two independent PROCESSES, not two passes in one: whatever a run leaves
  # behind in memory cannot be what makes the second agree with the first.
  SG_OUT_ROOT="$tmp/a" bash "$0"
  SG_OUT_ROOT="$tmp/b" bash "$0"
  if diff -r "$tmp/a" "$tmp/b"; then
    echo "gen: reproducible - two runs of the generator over this tree wrote the same bytes"
    exit 0
  fi
  echo "gen: NOT reproducible - two runs of the generator over one tree disagree"
  exit 1
fi
[ "${1:-}" = "--verify-wasm" ] || exit 0

verify_link() {
  "$CLANG" --target=wasm32 -nostdlib -ffreestanding -O1 -I"$here/test" -I"$sg/test" -I"$root/c/src" -isystem "$root/c/wasm/include" \
    -D_Thread_local= -DMAX_LOG_PAIRS=64 -DMAX_LEGAL_MOVES=4096 -DMAX_MOVE_CARDS=28 -DMAX_BATTLES=64 \
    -Wl,--no-entry -Wl,--export-all "$here/test/verify.c" -o "$1"
}
# The same source, linked again: the module the test reads has to be a function
# of verify.c and the headers it includes, and of nothing else.
#
# THE TWO LINKS ARE GIVEN THE SAME BASENAME, in two directories, and that is
# load-bearing. wasm-ld writes a `name` custom section whose module-name
# subsection is the output file's basename, so linking once to `verify.wasm` and
# once to a `mktemp` name produced two modules differing by exactly those bytes:
# identical section tables, identical strings, and `gen: verify.wasm does not
# build reproducibly` on every CI run from the day this check landed. macOS
# homebrew clang emits no module-name subsection at all, which is why it passed
# on the machine it was written on and only Linux ever saw it. With one basename
# the section says the same thing in both, and what is left to differ is what
# the check is for: a __DATE__, an address, an uninitialised pad.
mkdir -p "$here/build/twin"
verify_link "$here/build/verify.wasm"
verify_link "$here/build/twin/verify.wasm"
if ! cmp -s "$here/build/verify.wasm" "$here/build/twin/verify.wasm"; then
  echo "gen: verify.wasm does not build reproducibly - two links of the same source differ"
  exit 1
fi
rm -rf "$here/build/twin"
