#!/bin/bash
# fetch.sh repo listfile destdir : downloads listed paths from raw.githubusercontent (data only, never executed)
repo="$1"; list="$2"; dest="$3"
while IFS= read -r p; do
  out="$dest/$p"; [ -s "$out" ] && continue
  mkdir -p "$(dirname "$out")"
  enc=$(python3 -I -c 'import sys,urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$p")
  echo "$enc|$out"
done < "$list" | xargs -P 6 -I{} bash -c 'u="${1%%|*}"; o="${1#*|}"; curl -sfL -m 300 --retry 3 -o "$o" "https://raw.githubusercontent.com/'"$repo"'/master/$u" || echo "FAIL $u"' _ {}
