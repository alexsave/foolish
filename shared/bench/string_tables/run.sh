#!/bin/bash
# Build and run the string-table toy: foolish's real en + ru strings, looked up
# three ways from Swift. Needs the generated Swift tables (bash tools/structgen/gen.sh).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../../.." && pwd)"
B="$(mktemp -d)"; trap 'rm -rf "$B"' EXIT
cp "$HERE/CTables/"* "$B/"; cp "$ROOT/c/i18n/keys.h" "$ROOT/c/i18n/strings_en.c" "$ROOT/c/i18n/strings_ru.c" "$B/"
G="$ROOT/sdk/swift/gen/i18n"
[ -f "$G/FoolishStringsRu.swift" ] || { echo "run: bash tools/structgen/gen.sh first" >&2; exit 1; }
(cd "$B" && xcrun clang -O2 -c strings_en.c strings_ru.c)
xcrun swiftc -O -wmo -I "$B" "$HERE/Sources/main.swift" "$HERE/Sources/Cache.swift" \
  "$G/FoolishStringsEn.swift" "$G/FoolishStringsRu.swift" "$B/strings_en.o" "$B/strings_ru.o" -o "$B/toy"
"$B/toy" warm
for m in cold-swift cold-c cold-cache; do
  for i in $(seq 25); do "$B/toy" $m; done | sort -n |
    awk -v m=$m '{a[NR]=$1} END{printf "%-11s median %.1f us (25 fresh processes)\n", m, a[13]/1000}'
done
