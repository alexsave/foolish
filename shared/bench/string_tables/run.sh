#!/bin/bash
# Build and run the string-table toy: a real product's en + ru C string tables,
# looked up from Swift three ways.
#   run.sh <dir with keys.h, strings_en.c, strings_ru.c> <key prefix, e.g. FS>
# The Swift dictionaries are generated here from the same C files, in the shape
# a generator emitting [String: String] tables would, so no product's generated
# code is needed.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "${1:?dir with keys.h and strings_*.c}" && pwd)"; P="${2:?key prefix}"
B="$(mktemp -d)"; trap 'rm -rf "$B"' EXIT
cp "$SRC/keys.h" "$SRC/strings_en.c" "$SRC/strings_ru.c" "$HERE/CTables/module.modulemap" "$B/"
cat > "$B/tables.h" <<H
#include "keys.h"
extern const char *const ${P}_STRINGS_EN[${P}_K_COUNT];
extern const char *const ${P}_STRINGS_RU[${P}_K_COUNT];
static inline const char *toy_text(int lang, int key) { return (lang ? ${P}_STRINGS_RU : ${P}_STRINGS_EN)[key]; }
static inline int toy_count(void) { return ${P}_K_COUNT; }
H
for L in en ru; do
  python3 - "$SRC/strings_$L.c" "$P" "$L" > "$B/Table_$L.swift" <<'PY'
import re, sys
src, p, lang = sys.argv[1], sys.argv[2], sys.argv[3]
rows = re.findall(r'\[\s*%s_K_(\w+)\s*\]\s*=\s*("(?:[^"\\]|\\.)*")' % p, open(src).read())
print("let Table%s: [String: String] = [" % lang.capitalize())
for k, v in rows: print('    "%s": %s,' % (k.lower(), v))
print("]")
PY
done
(cd "$B" && xcrun clang -O2 -c strings_en.c strings_ru.c)
xcrun swiftc -O -wmo -I "$B" "$HERE/Sources/main.swift" "$HERE/Sources/Cache.swift" \
  "$B/Table_en.swift" "$B/Table_ru.swift" "$B/strings_en.o" "$B/strings_ru.o" -o "$B/toy"
"$B/toy" warm
for m in cold-swift cold-c cold-cache; do
  for i in $(seq 25); do "$B/toy" $m; done | sort -n |
    awk -v m=$m '{a[NR]=$1} END{printf "%-11s median %.1f us (25 fresh processes)\n", m, a[13]/1000}'
done
