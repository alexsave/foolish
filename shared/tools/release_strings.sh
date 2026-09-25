#!/bin/bash
# release_strings.sh - fail when a Release build carries what only DEBUG may.
#
#   shared/tools/release_strings.sh <X.app | X.ipa> [--forbid-framework NAME ...]
#
# Fails (exit 1), listing every hit, on:
#   - a `dev.<name>` dev-file name anywhere in the bundle: the dev-file readers
#     are compiled only under DEBUG, so a Release binary naming one means a
#     debug door leaked into the product;
#   - an em dash (U+2014) in a binary, a plist, a .strings/.stringsdict or any
#     text resource: user-facing text uses a plain hyphen;
#   - a forbidden framework, embedded (Frameworks/NAME.framework) or linked
#     (a load command naming NAME.framework) by any binary in the bundle.
#
# The byte scan is C (release_strings/rs_scan.c, built on first use). Binary
# plists and compiled .strings hold non-ASCII as UTF-16, so they are converted
# to XML first and the XML is scanned.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SCAN="$HERE/release_strings/build/rs_scan"
[ -x "$SCAN" ] && [ "$SCAN" -nt "$HERE/release_strings/rs_scan.c" ] \
  || make -s --no-print-directory -C "$HERE/release_strings" >/dev/null

IN="${1:?usage: release_strings.sh <X.app | X.ipa> [--forbid-framework NAME ...]}"; shift
FORBID=()
while [ $# -gt 0 ]; do
  case "$1" in
    --forbid-framework) FORBID+=("${2:?--forbid-framework needs a name}"); shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
case "$IN" in
  *.ipa) unzip -q "$IN" -d "$TMP/ipa"; APP="$(ls -d "$TMP"/ipa/Payload/*.app | head -1)" ;;
  *) APP="$IN" ;;
esac
[ -d "$APP" ] || { echo "no app bundle at $IN" >&2; exit 2; }

BAD=0
# Everything but compiled asset catalogs and images, whose compressed bytes are
# not text; plists and .strings are scanned as XML.
FILES=()
while IFS= read -r -d '' f; do
  case "$f" in
    *.car|*.png|*.jpg|*.jpeg|*.heic|*.ktx|*.ktx2|*.mp4|*.mov|*.caf|*.wav|*.m4a) continue ;;
    */_CodeSignature/*|*/embedded.mobileprovision|*/SC_Info/*) continue ;;
    *.plist|*.strings|*.stringsdict)
      x="$TMP/xml/${f#"$APP"/}"; mkdir -p "$(dirname "$x")"
      if plutil -convert xml1 -o "$x" "$f" 2>/dev/null; then FILES+=("$x"); else FILES+=("$f"); fi ;;
    *) FILES+=("$f") ;;
  esac
done < <(find "$APP" -type f -print0)

OUT="$("$SCAN" "${FILES[@]}" 2>&1)" || BAD=1
[ -n "$OUT" ] && LC_ALL=C sed -e "s|$TMP/xml/|<plist> |" -e "s|$APP/||" <<<"$OUT"

if [ "${#FORBID[@]}" -gt 0 ]; then
  for fw in "${FORBID[@]}"; do
    hit="$(find "$APP" -type d -name "$fw.framework" | sed "s|$APP/||")"
    [ -z "$hit" ] || { echo "forbidden framework embedded: $hit"; BAD=1; }
    while IFS= read -r -d '' f; do
      file -b "$f" | grep -q Mach-O || continue
      if otool -L "$f" 2>/dev/null | tail -n +2 | grep -q "/$fw.framework/"; then
        echo "forbidden framework linked: ${f#"$APP"/} -> $fw"; BAD=1
      fi
    done < <(find "$APP" -type f -perm -u+x -print0)
  done
fi

if [ "$BAD" = 0 ]; then echo "release strings clean: $(basename "$APP")"; else echo "release strings FAILED: $(basename "$APP")" >&2; fi
exit "$BAD"
