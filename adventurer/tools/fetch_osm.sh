#!/bin/bash
# fetch_osm.sh - the OSM extract the graph is built from, into adventurer/data (gitignored).
#
#   tools/fetch_osm.sh [URL]
#
# Downloads a Geofabrik extract to data/region.osm.pbf (Rhode Island unless a URL is given) and
# skips the download when the file is already there and non-empty, so `make graph` can be
# re-run without touching the network. Then: make -C adventurer/server graph.
set -euo pipefail

URL="${1:-https://download.geofabrik.de/north-america/us/rhode-island-latest.osm.pbf}"
HERE="$(cd "$(dirname "$0")" && pwd)"
DATA="$HERE/../data"
OUT="$DATA/region.osm.pbf"

mkdir -p "$DATA"
if [ -s "$OUT" ]; then
    echo "fetch_osm: $OUT exists ($(stat -f %z "$OUT" 2>/dev/null || stat -c %s "$OUT") bytes), not downloading"
    exit 0
fi

echo "fetch_osm: $URL -> $OUT"
# Into a temp name, renamed when complete, so a killed download never leaves a short file behind
# that the size check above would accept.
curl -L --fail --retry 3 --progress-bar -o "$OUT.part" "$URL"
mv "$OUT.part" "$OUT"
echo "fetch_osm: $(stat -f %z "$OUT" 2>/dev/null || stat -c %s "$OUT") bytes"
