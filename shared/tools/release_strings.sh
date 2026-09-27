#!/bin/bash
# release_strings.sh - fail when a Release build carries what only DEBUG may.
#
#   shared/tools/release_strings.sh <X.app | X.ipa> [--forbid-framework NAME ...]
#
# Fails (exit 1), listing every hit, on:
#   - DevFlags compiled in: a `DevFlags` symbol in `nm` of any Mach-O, or the
#     type's name in its bytes (which survives stripping). DevFlags is the one
#     dev-file reader and is compiled only under DEBUG; this is the check that
#     catches a leak whatever its strings look like;
#   - a `dev.<name>` dev-file name anywhere in the bundle, as UTF-8, UTF-16 or
#     a Swift small string rebuilt from arm64 immediates (rs_scan.c says how);
#   - an em dash (U+2014) in a binary, a plist, a .strings/.stringsdict or any
#     text resource: user-facing text uses a plain hyphen;
#   - a forbidden framework, embedded (Frameworks/NAME.framework) or linked
#     (a load command naming NAME.framework) by any binary in the bundle.
#
# The byte scan is C (release_strings/rs_scan.c, built on first use). Binary
# plists and compiled .strings are converted to XML first and the XML is
# scanned; everything else, asset catalogs included, is scanned raw.
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
# Everything but images and media, whose compressed bytes are not text;
# plists and .strings are scanned as XML. An asset catalog (.car) IS scanned:
# its names and data assets are stored raw.
FILES=()
while IFS= read -r -d '' f; do
  case "$f" in
    *.png|*.jpg|*.jpeg|*.heic|*.ktx|*.ktx2|*.mp4|*.mov|*.caf|*.wav|*.m4a) continue ;;
    */_CodeSignature/*|*/embedded.mobileprovision|*/SC_Info/*) continue ;;
    *.plist|*.strings|*.stringsdict)
      x="$TMP/xml/${f#"$APP"/}"; mkdir -p "$(dirname "$x")"
      if plutil -convert xml1 -o "$x" "$f" 2>/dev/null; then FILES+=("$x"); else FILES+=("$f"); fi ;;
    *) FILES+=("$f") ;;
  esac
done < <(find "$APP" -type f -print0)

OUT="$("$SCAN" "${FILES[@]}" 2>&1)" || BAD=1
[ -n "$OUT" ] && LC_ALL=C sed -e "s|$TMP/xml/|<plist> |" -e "s|$APP/||" <<<"$OUT"

# Every Mach-O in the bundle. Tool output is captured, then grepped: under
# pipefail, `tool | grep -q` fails when grep stops reading early and the tool
# takes SIGPIPE, which reported a long `otool -L` as clean.
MACHO=()
while IFS= read -r -d '' f; do
  KIND="$(file -b "$f")"
  case "$KIND" in *Mach-O*) MACHO+=("$f") ;; esac
done < <(find "$APP" -type f -print0)

if [ "${#MACHO[@]}" -gt 0 ]; then
  for f in "${MACHO[@]}"; do
    SYMS="$(nm -a "$f" 2>/dev/null || true)"
    if grep -q DevFlags <<<"$SYMS"; then
      echo "${f#"$APP"/}: dev-flags symbols: $(grep -c DevFlags <<<"$SYMS") (DevFlags is DEBUG-only)"; BAD=1
    fi
  done
fi

if [ "${#FORBID[@]}" -gt 0 ]; then
  for fw in "${FORBID[@]}"; do
    hit="$(find "$APP" -type d -name "$fw.framework" | sed "s|$APP/||")"
    [ -z "$hit" ] || { echo "forbidden framework embedded: $hit"; BAD=1; }
    [ "${#MACHO[@]}" -gt 0 ] || continue
    for f in "${MACHO[@]}"; do
      LINKS="$(otool -L "$f" 2>/dev/null || true)"
      LINKS="${LINKS#*$'\n'}"     # the first line names the file itself
      if grep -qF "/$fw.framework/" <<<"$LINKS"; then
        echo "forbidden framework linked: ${f#"$APP"/} -> $fw"; BAD=1
      fi
    done
  done
fi

if [ "$BAD" = 0 ]; then echo "release strings clean: $(basename "$APP")"; else echo "release strings FAILED: $(basename "$APP")" >&2; fi
exit "$BAD"
