#!/bin/bash
# test.sh - release_strings.sh against fixture bundles, one defect each.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
RS="$HERE/../release_strings.sh"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
FAIL=0
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
expect em-dash-binary-plist 1 "Info.plist: em-dash .*tap — to play" "$T/dash.app"

mk swiftlit 'Good \xe2\x80\x94 your turn\0'
expect em-dash-literal 1 "em-dash" "$T/swiftlit.app"

mk zhdash 'x\0\xe4\xbd\xa0\xe2\x80\x94\xe2\x80\x94\xe5\xa5\xbd\0'
expect chinese-dash-pair-ok 0 "release strings clean" "$T/zhdash.app"

mk fw 'x\0'; mkdir -p "$T/fw.app/Frameworks/Bad.framework"
expect forbidden-framework 1 "forbidden framework embedded: Frameworks/Bad.framework" "$T/fw.app" --forbid-framework Bad
expect other-framework-ok 0 "release strings clean" "$T/fw.app" --forbid-framework Good

(cd "$T" && mkdir -p Payload && cp -R dev.app Payload/ && zip -qr dev.ipa Payload)
expect ipa 1 "dev-file dev.ruler" "$T/dev.ipa"

exit $FAIL
