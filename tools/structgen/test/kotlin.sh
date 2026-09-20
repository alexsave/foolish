#!/usr/bin/env bash
# The Kotlin emitter (--kotlin), and the Kotlin half of tools/datagen.
#
# WHAT THIS TEST CANNOT DO, said first because it changes how to read the rest:
# NOTHING HERE COMPILES ANY KOTLIN. There is no kotlinc on the machines this
# repo builds on, and no Android toolchain either, so unlike test/swift.sh -
# which compiles the generated Swift against C built from the same headers and
# runs it - this asserts on the emitted TEXT. Every Kotlin line below is
# unproven as Kotlin. If kotlinc ever lands here, the test to write is swift.sh's
# twin (a JNI probe filling the fixture through its own field names, read back
# through the generated readers), and this file becomes its shape check.
#
# So the offsets get their evidence sideways instead. The Kotlin and the Swift
# emitters read ONE model, so for one spec at one triple they must lay their
# readers on exactly the same bytes; swift.sh proves the Swift offsets are the
# offsets offsetof gives. Chained, that is what says the Kotlin offsets are C's,
# and it is the one check here that is about bytes rather than about text.
#
#   kotlin.sh        generate and check
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$here/../.." && pwd)"
# The generator is shared (shared/tools/structgen); this test, and the specs
# and fixtures it points the generator at, are this product's.
sg="$root/shared/tools/structgen"
make -s -C "$sg" build/structgen
make -s -C "$root/shared/tools/datagen" build/datagen
SG="$sg/build/structgen"
DG="$root/shared/tools/datagen/build/datagen"
out="$here/build/kotlin"
rm -rf "$out"; mkdir -p "$out"
fails=0

ok() { # result description
    if [ "$1" -eq 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails + 1)); fi
}
has() { # file pattern description  (a fixed string, so Kotlin's punctuation is safe)
    grep -qF -- "$2" "$1"; ok $? "$3"
}
hasnt() { # file pattern description
    ! grep -qF -- "$2" "$1"; ok $? "$3"
}
expect_fail() { # description pattern args...
    local what="$1" pat="$2"; shift 2
    local err; err="$("$SG" "$@" 2>&1 >/dev/null)"; local rc=$?
    if [ $rc -ne 0 ] && printf '%s' "$err" | grep -qF -- "$pat"; then echo "ok   $what"
    else echo "FAIL $what (rc=$rc): $err"; fails=$((fails + 1)); fi
}

if command -v kotlinc >/dev/null 2>&1; then
    echo "kotlin.sh: kotlinc IS on this machine - this test still only reads the text; see the header" >&2
else
    echo "kotlin.sh: no kotlinc on this machine - nothing below compiles the Kotlin it checks"
fi

TRIPLE="aarch64-linux-android21"

# ---- the fixture the Kotlin emitter has of its own --------------------------
"$SG" --cwd "$here/test" --header kotlin.h --root KtThing --build "android=" --target "$TRIPLE" \
  --snapshot KtThing --snapshot-only --writer KtThing \
  --count KtThing.tags=n_tags \
  --kotlin "$out/thing.kt" --kotlin-package cards.foolish.test
K="$out/thing.kt"

has "$K" 'package cards.foolish.test'                       "the module declares the package it was asked for"
has "$K" 'import java.nio.ByteBuffer'                       "it imports ByteBuffer"
has "$K" 'import java.nio.ByteOrder'                        "…and ByteOrder, which the endianness guard needs"
has "$K" 'fun readKtThing(buf: ByteBuffer, p: Int): KtThingSnap' "a reader takes a buffer and a byte offset"
has "$K" 'fun writeKtThing(buf: ByteBuffer, p: Int, s: KtThingSnap)' "a writer takes the snapshot back"
has "$K" 'data class KtThingSnap('                          "a snapshot is a data class, so equals() is its members'"
has "$K" 'const val C_SIZE = '                              "…carrying the C size a host allocates"

# The keyword rule, which is not Swift's list.
has "$K" 'val `in`: Int,'                                   "a member named for a Kotlin keyword is backticked"
has "$K" 'val `object`: Int,'                               "…and so is the next one Swift would emit bare"
has "$K" 'val isOpen: Int,'                                 "…while a name that only LOOKS like one is left alone"
has "$K" 's.`in`'                                           "the writer reads the backticked member back"

# The widening rule, which is not Swift's either: Kotlin's Int is 32 bits.
has "$K" 'val bigU32: Long,'                                "a uint32 widens to Long, where Int would lose the top bit"
has "$K" 'val gameId: ULong,'                               "a uint64 keeps its signedness as ULong"
has "$K" 'val delta: Long,'                                 "an int64 is Long"
has "$K" 'val ratio: Double,'                               "a float widens to Double"
has "$K" 'val ready: Boolean,'                              "a bool is Boolean"
has "$K" 'and 0xFFFFFFFFL)'                                 "…and the uint32 is masked, not sign-extended"
has "$K" 'val tags: List<Int>,'                             "an array is a List and never an Array, whose equals() is identity"
has "$K" 'val name: String,'                                "a NUL-terminated char[N] is a String"

# A count is checked before it is used, in Long, and only then narrowed.
has "$K" 'if (n_tags < 0L || n_tags > 4L) throw SGLayoutException.Count("KtThing.tags", n_tags, 4L)' \
                                                            "a count is range-checked before anything is read with it"
has "$K" 'for (i in 0 until n_tags.toInt())'                "…and narrowed for the loop only after the check"
has "$K" 'if (s.tags.size > 4) throw SGLayoutException.TooLong("KtThing.tags", s.tags.size.toLong(), 4L)' \
                                                            "the writer refuses a list longer than its field"

# The handshake, generated rather than left to a host.
HASH="$("$SG" --cwd "$here/test" --header kotlin.h --root KtThing --build "android=" --target "$TRIPLE" --print-hash)"
has "$K" "val SG_LAYOUT_HASH: UInt = ${HASH}u"              "the module carries this run's layout hash"
has "$K" 'fun sgCheckLayout(libraryHash: UInt)'             "…and the check that compares it with the library's"
has "$K" 'sealed class SGLayoutException'                   "the refusal type is one sealed class"
hasnt "$K" 'class Null('                                    "…with no null case, because a pointer never reaches Kotlin"

# The endianness guard. The JVM's default order is big-endian and every target
# this kernel builds for is little-endian, so this is not a nicety.
has "$K" 'if (b.order() == ByteOrder.LITTLE_ENDIAN) b else b.duplicate().order(ByteOrder.LITTLE_ENDIAN)' \
                                                            "every reader asks for little-endian rather than trusting the caller"
has "$K" '    val sgb = sgLE(buf)'                          "…on entry, into a local that no C field name can shadow"

# ---- what Kotlin will not read ----------------------------------------------
expect_fail "a pointer field is refused, with the reason" "is a pointer: Kotlin reads a ByteBuffer" \
  --cwd "$here/test" --header kotlin.h --root KtPtr --build "android=" --target "$TRIPLE" \
  --snapshot KtPtr --snapshot-only --count KtPtr.body=body_len \
  --kotlin "$out/ptr.kt" --kotlin-package p
expect_fail "--kotlin with no package"          "needs --kotlin-package" \
  --cwd "$here/test" --header kotlin.h --root KtThing --build "android=" --snapshot KtThing --kotlin "$out/x.kt"
expect_fail "--kotlin-package with no --kotlin" "nothing is being written in Kotlin" \
  --cwd "$here/test" --header kotlin.h --root KtThing --build "android=" --snapshot KtThing --print-hash --kotlin-package p
expect_fail "--kotlin with no --snapshot"       "Kotlin gets value snapshots" \
  --cwd "$here/test" --header kotlin.h --root KtThing --build "android=" --kotlin "$out/x.kt" --kotlin-package p

# ---- the offsets, against the emitter swift.sh proves against C --------------
#
# One model, two emitters: for one spec at one triple the bytes they read must
# be the same bytes. Both files are reduced to the multiset of byte offsets and
# array strides they name - Swift spells an offset `fromByteOffset: N` or
# `p + N`, Kotlin always `p + N` - and the two lists must be equal.
SNAP=(--cwd "$sg/test" --header snap.h --root Snap --build "android=" --target "$TRIPLE"
      --snapshot Snap --snapshot-only --writer Snap
      --count Snap.pairs=n_pairs --count Snap.items=n_items --count Snap.text=n_text --count SItem.text=len)
"$SG" "${SNAP[@]}" --swift "$out/snap.swift"
"$SG" "${SNAP[@]}" --kotlin "$out/snap.kt" --kotlin-package cards.foolish.test
offsets() { # file
    sed -e 's/fromByteOffset: /@/g' -e 's/toByteOffset: /@/g' \
        -e 's/sgUTF8Set(p, /@/g' -e 's/sgCStrSet(p, /@/g' -e 's/sgUTF8(p, /@/g' -e 's/sgCStr(p, /@/g' \
        -e 's/p + /@/g' "$1" | { grep -o -e '@[0-9][0-9]*' -e 'i \* [0-9][0-9]*' || true; } | sort
}
diff <(offsets "$out/snap.swift") <(offsets "$out/snap.kt") >/dev/null
ok $? "the Kotlin readers name the same byte offsets and strides the Swift ones do"
sizes() { grep -o -e 'cSize = [0-9]*' -e 'C_SIZE = [0-9]*' "$1" | grep -o '[0-9]*' | sort; }
diff <(sizes "$out/snap.swift") <(sizes "$out/snap.kt") >/dev/null
ok $? "…and agree about every record's size"

# ---- one module for every Android ABI ---------------------------------------
#
# These structs hold no pointer, which is why Kotlin can read them at all, and
# nothing else in them is laid out differently between Android's ABIs. What DOES
# differ is that plain `char` is unsigned on ARM and signed on x86, and the
# layout hash carries a scalar's kind - so the readers are identical and the
# hash is not. That is a real trap for an app shipping both: the emulator's .so
# has to be stamped from its own run. Pinned here so it is a stated fact rather
# than a discovery.
ANDROID=(--cwd "$root/c")
while IFS= read -r line; do ANDROID+=($line); done < <(grep -v '^[[:space:]]*#' "$here/specs/android_layout.args")
"$SG" "${ANDROID[@]}" --build "android=-O2 -Isrc" --target aarch64-linux-android21 \
  --kotlin "$out/arm.kt" --kotlin-package cards.foolish.kernel
"$SG" "${ANDROID[@]}" --build "android=-O2 -Isrc" --target x86_64-linux-android21 \
  --kotlin "$out/x86.kt" --kotlin-package cards.foolish.kernel
# Taken once, into a file: under `set -o pipefail` a `diff | grep` pipeline
# carries diff's own "they differ" out as the pipeline's status, which is not
# what is being asked here.
diff "$out/arm.kt" "$out/x86.kt" > "$out/abi.diff" || true
[ "$(grep -c '^[<>]' "$out/abi.diff")" = 4 ] \
  && grep -q 'SG_LAYOUT_HASH' "$out/abi.diff" \
  && grep -q '// target:' "$out/abi.diff"
ok $? "arm64 and x86_64 get the same readers, and differ only in the target line and the hash"

# The real spec, reduced to what it must contain: the board a client draws.
has "$out/arm.kt" 'data class TableViewSnap('               "the real spec emits the client's slot"
has "$out/arm.kt" 'val seats: List<ViewSeatSnap>,'          "…with the roster joined to it"
has "$out/arm.kt" 'val gameId: String,'                     "…and a counted char[N] as a String"

# Two runs, two processes, same bytes - the promise gen.sh --check makes, made
# here for the emitter it now also covers.
"$SG" "${SNAP[@]}" --kotlin "$out/again.kt" --kotlin-package cards.foolish.test
cmp -s "$out/snap.kt" "$out/again.kt"
ok $? "two runs over one tree write the same Kotlin"

# ---- the translated strings (tools/datagen --kotlin) -------------------------
#
# 25 languages x every key, one file each, for the same reason the C is split
# per language: a build keeps the language it needs and not the other 24.
i18n="$out/i18n"; mkdir -p "$i18n"
dg() { "$DG" --cwd "$root/c/i18n" "$@"; }
dg --header keys.h --table FS_KEY_NAME --require-complete --name FoolishStringKeys \
   --kotlin "$i18n/FoolishStringKeys.kt" --kotlin-package cards.foolish.i18n
dg --header languages.h --table FS_LANGUAGES --require-complete --name FoolishLanguages \
   --kotlin "$i18n/FoolishLanguages.kt" --kotlin-package cards.foolish.i18n
nkeys="$(grep -c '^    "' "$i18n/FoolishStringKeys.kt")"
[ "$nkeys" -gt 100 ]; ok $? "the key list came back with $nkeys keys"
has "$i18n/FoolishLanguages.kt" 'data class FoolishLanguagesRow(' "the registry is a data class per row"
has "$i18n/FoolishLanguages.kt" 'val FoolishLanguages: List<FoolishLanguagesRow> = listOf(' "…and a List of them"

# THE LIST OF LANGUAGES COMES FROM THE C, not from this file: a language added
# to the registry is covered here the day it lands.
codes="$(grep -o 'code = "[a-z][a-z]*"' "$i18n/FoolishLanguages.kt" | sed 's/.*"\(.*\)"/\1/')"
[ -n "$codes" ]; ok $? "the language codes read back out of the generated registry"
nlang=0; short=""
for code in $codes; do
    up="$(printf '%s' "$code" | tr '[:lower:]' '[:upper:]')"
    cap="$(printf '%s%s' "$(printf '%s' "${code%"${code#?}"}" | tr '[:lower:]' '[:upper:]')" "${code#?}")"
    dg --header "strings_$code.c" --table "FS_STRINGS_$up" --labels "FS_STRINGS_$up.0=FS_KEY_NAME" \
       --require-complete --name "FoolishStrings$cap" \
       --kotlin "$i18n/FoolishStrings$cap.kt" --kotlin-package cards.foolish.i18n || { short="$short $code(datagen)"; continue; }
    n="$(grep -c '" to "' "$i18n/FoolishStrings$cap.kt")"
    [ "$n" = "$nkeys" ] || short="$short $code($n/$nkeys)"
    grep -q "^val FoolishStrings$cap: Map<String, String> = mapOf(\$" "$i18n/FoolishStrings$cap.kt" || short="$short $code(shape)"
    nlang=$((nlang + 1))
done
[ -z "$short" ]; ok $? "all $nlang languages carry all $nkeys keys, each as its own Map<String, String>${short:+ - short:$short}"
[ "$nlang" -ge 20 ]; ok $? "…and there are $nlang of them"

[ $fails -eq 0 ] && echo "kotlin: all pass (text only - nothing here compiled any Kotlin)" \
  || { echo "kotlin: $fails failed"; exit 1; }
