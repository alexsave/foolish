#!/bin/bash
# test.sh - release_strings.sh against fixture bundles, one defect each.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
RS="$HERE/../release_strings.sh"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
FAIL=0
DASH="$(printf '\xe2\x80\x94')"     # U+2014, spelled out so this file carries none
expect() {  # expect NAME WANT_EXIT GREP [args...]
  local name="$1" want="$2" pat="$3"; shift 3
  local out rc
  out="$("$RS" "$@" 2>&1)"; rc=$?
  if [ "$rc" != "$want" ] || ! grep -q -- "$pat" <<<"$out"; then
    echo "FAIL $name: exit $rc (want $want), output:"; echo "$out" | sed 's/^/    /'; FAIL=1
  else echo "ok   $name"; fi
}
mk() { mkdir -p "$T/$1.app"; printf '%b' "$2" > "$T/$1.app/bin"; }

# Near misses that must NOT count: a word ending in dev, a URL, a hyphen, and
# E2 80 94 inside non-text bytes (it occurs by chance in machine code).
mk clean 'hello - world\0developer.apple.com\0abcdev.ruler\0\x01\x02\xe2\x80\x94\x03\x04\0'
expect clean 0 "release strings clean" "$T/clean.app"

mk dev 'x\0dev.ruler\0'
expect dev-file 1 "dev-file dev.ruler" "$T/dev.app"

mk dash 'x\0'
printf '<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict><key>k</key><string>tap \xe2\x80\x94 to play</string></dict></plist>' > "$T/dash.app/Info.plist"
plutil -convert binary1 "$T/dash.app/Info.plist"      # UTF-16 inside: raw bytes never match
expect em-dash-binary-plist 1 "Info.plist: em-dash .*tap $DASH to play" "$T/dash.app"

mk swiftlit 'Good \xe2\x80\x94 your turn\0'
expect em-dash-literal 1 "em-dash" "$T/swiftlit.app"

mk zhdash 'x\0\xe4\xbd\xa0\xe2\x80\x94\xe2\x80\x94\xe5\xa5\xbd\0'
expect chinese-dash-pair-ok 0 "release strings clean" "$T/zhdash.app"

mk fw 'x\0'; mkdir -p "$T/fw.app/Frameworks/Bad.framework"
expect forbidden-framework 1 "forbidden framework embedded: Frameworks/Bad.framework" "$T/fw.app" --forbid-framework Bad
expect other-framework-ok 0 "release strings clean" "$T/fw.app" --forbid-framework Good

(cd "$T" && mkdir -p Payload && cp -R dev.app Payload/ && zip -qr dev.ipa Payload)
expect ipa 1 "dev-file dev.ruler" "$T/dev.ipa"

# The shapes a byte-in-a-row scan never saw (SECURITY_REVIEW_SHARED.md,
# section 6); each was a real bundle that passed as clean.
mk eof 'x\0Done \xe2\x80\x94'
expect em-dash-at-file-end 1 "em-dash \"Done $DASH\"" "$T/eof.app"

mk dbl 'x\0Wait \xe2\x80\x94\xe2\x80\x94 your turn\0'
expect english-dash-pair 1 "em-dash .*Wait $DASH$DASH your turn" "$T/dbl.app"

mk upper 'x\0dev.Ruler\0'
expect dev-file-upper-case 1 "dev-file dev.Ruler" "$T/upper.app"

mk utf16 'x\0'
printf 'T\0a\0p\0 \0\x14\x20 \0t\0o\0 \0p\0l\0a\0y\0' > "$T/utf16.app/help.txt"
expect em-dash-utf16 1 "em-dash \"Tap $DASH to play\" (utf-16le)" "$T/utf16.app"

# 14 20 among units that are not ASCII is machine code, not text.
mk utf16noise '\x41\x4e\x9a\x7c\x14\x20\x42\x4f\x43\x50\0'
expect utf16-noise-ok 0 "release strings clean" "$T/utf16noise.app"

mk car 'x\0'; printf 'BOMStore\0\0Tap \xe2\x80\x94 to play\0' > "$T/car.app/Assets.car"
expect em-dash-asset-catalog 1 "Assets.car: em-dash" "$T/car.app"

# A Swift small string never lies in a row: `let key = ... "dev.ruler"` as
# arm64 builds it (mov x9 + three movk, then mov x8 + movk for the second
# word), from a real -O binary, behind a Mach-O magic.
mk small '\xcf\xfa\xed\xfe\x0c\x00\x00\x01\x08\x0f\x80\x52\x89\xac\x8c\xd2\xc9\xce\xa5\xf2\x49\xae\xce\xf2\x89\xad\xec\xf2\x7f\x16\x00\xf1\x00\x81\x89\x9a\x48\x0e\x80\xd2\x08\x20\xfd\xf2\x13\x20\xfc\xd2\x61\x82\x88\x9a\x48\x00\x00\x90'
expect dev-file-small-string 1 "dev-file dev.ruler (swift small string)" "$T/small.app"

# `"dev." + name`: the prefix is a 4-byte small string of its own.
mk concat '\xcf\xfa\xed\xfe\x0c\x00\x00\x01\x88\xac\x8c\x52\xc8\xce\xa5\x72\x09\x0f\x80\x52\x7f\x16\x00\xf1\x34\x81\x88\x9a\x08\x80\xfc\xd2\x16\x20\xfc\xd2\xd3\x82\x88\x9a'
expect dev-prefix-small-string 1 "dev-file dev\. (swift small string)" "$T/concat.app"

# The same words in a file that is not Mach-O are not code.
mk notmacho '\x00\x00\x00\x00\x0c\x00\x00\x01\x08\x0f\x80\x52\x89\xac\x8c\xd2\xc9\xce\xa5\xf2\x49\xae\xce\xf2\x89\xad\xec\xf2\x7f\x16\x00\xf1\x00\x81\x89\x9a\x48\x0e\x80\xd2\x08\x20\xfd\xf2\x13\x20\xfc\xd2\x61\x82\x88\x9a'
expect small-string-needs-macho 0 "release strings clean" "$T/notmacho.app"

# THE STRUCTURAL CHECK: the shared DevFlags.swift itself, compiled with and
# without DEBUG, and stripped the way an archive strips an executable. With
# DEBUG it must fail whatever its strings look like; without, it must pass.
DF="$HERE/../../swift/MessagesKit/DevFlags.swift"
if command -v swiftc >/dev/null 2>&1; then
  for cfg in debug release; do
    mkdir -p "$T/df_$cfg.app"
    flag=(); [ "$cfg" = debug ] && flag=(-D DEBUG)
    swiftc -O -parse-as-library -emit-library ${flag[@]+"${flag[@]}"} "$DF" -o "$T/df_$cfg.app/lib.dylib" 2>/dev/null
    cp "$T/df_$cfg.app/lib.dylib" "$T/df_$cfg.app/stripped"; strip -x "$T/df_$cfg.app/stripped" 2>/dev/null
  done
  expect devflags-compiled-in 1 "dev-flags symbols" "$T/df_debug.app"
  rm "$T/df_debug.app/lib.dylib"
  expect devflags-stripped 1 "stripped: dev-flags (the DevFlags type is compiled in)" "$T/df_debug.app"
  expect devflags-compiled-out 0 "release strings clean" "$T/df_release.app"
else
  echo "skip devflags (no swiftc)"
fi

# `otool -L | tail | grep -q` under pipefail: grep stops at the first match,
# the writer takes SIGPIPE, and a long load-command list read as clean. A
# stand-in otool prints the forbidden framework first and 5000 lines after.
mkdir -p "$T/bin"
cat > "$T/bin/otool" <<'SH'
#!/bin/bash
echo "$2:"; echo "	/System/Library/Frameworks/Bad.framework/Bad (compatibility version 1.0.0)"
for i in $(seq 1 5000); do echo "	/usr/lib/libfiller$i.dylib (compatibility version 1.0.0, current version 1.0.0)"; done
SH
chmod +x "$T/bin/otool"
mk longotool '\xcf\xfa\xed\xfe\x0c\x00\x00\x01'
PATH="$T/bin:$PATH" expect forbidden-framework-long-otool 1 "forbidden framework linked: bin -> Bad" "$T/longotool.app" --forbid-framework Bad

exit $FAIL
