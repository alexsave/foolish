#!/bin/bash
# fetch_osm.sh - the OSM extract the graph is built from, into adventurer/data (gitignored).
#
#   tools/fetch_osm.sh            New York City and its neighbours (below), from Geofabrik's state
#                                 extracts, cut with osmium
#   tools/fetch_osm.sh URL        one extract as it is (Rhode Island:
#                                 https://download.geofabrik.de/north-america/us/rhode-island-latest.osm.pbf,
#                                 then `make graph IMPORT_ARGS="--admin-levels 8"`)
#
# THE REGION is the five boroughs with Nassau, Westchester, Hudson and Bergen around them: the
# city, the suburbs people ride out to on both sides of the Hudson, and nothing the fence
# (server/Makefile IMPORT_ARGS --within) then throws away but a margin. Geofabrik has no city
# extract, so the two state files (about 500 and 165 MB) are downloaded once to data/osm/, each
# cut to the box below, and the two cuts merged into data/region.osm.pbf. The cut is osmium's
# "smart" strategy with every relation type complete, so a boundary that crosses the box's edge
# (a county's, a town's) still closes and the importer's fence can test against it.
#
# A file already there and non-empty is not downloaded or cut again, so `make graph` can be
# re-run without the network. Needs osmium (brew install osmium-tool; apt install osmium-tool).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DATA="$HERE/../data"
OUT="$DATA/region.osm.pbf"
# lon_min,lat_min,lon_max,lat_max: Staten Island's west to Nassau's east, Staten Island's south
# to Westchester's north, with a few km around.
BBOX="${ADV_BBOX:--74.30,40.48,-73.42,41.38}"
STATES="${ADV_STATES:-new-york new-jersey}"

mkdir -p "$DATA"
if [ -s "$OUT" ]; then
    echo "fetch_osm: $OUT exists ($(stat -f %z "$OUT" 2>/dev/null || stat -c %s "$OUT") bytes), not downloading"
    exit 0
fi

# Into a temp name, renamed when complete, so a killed download never leaves a short file behind
# that the size check above would accept.
fetch() {
    local url=$1 out=$2
    if [ -s "$out" ]; then echo "fetch_osm: $out exists, not downloading"; return; fi
    echo "fetch_osm: $url -> $out"
    curl -L --fail --retry 3 --progress-bar -o "$out.part" "$url"
    mv "$out.part" "$out"
}

if [ $# -ge 1 ]; then
    fetch "$1" "$OUT"
    echo "fetch_osm: $(stat -f %z "$OUT" 2>/dev/null || stat -c %s "$OUT") bytes"
    exit 0
fi

command -v osmium > /dev/null || { echo "fetch_osm: osmium is needed to cut the state extracts (brew install osmium-tool)" >&2; exit 1; }
mkdir -p "$DATA/osm"
cuts=()
for s in $STATES; do
    fetch "https://download.geofabrik.de/north-america/us/$s-latest.osm.pbf" "$DATA/osm/$s-latest.osm.pbf"
    cut="$DATA/osm/cut-$s.osm.pbf"
    echo "fetch_osm: cutting $s to $BBOX"
    osmium extract -b "$BBOX" -s smart -S types=any --overwrite -o "$cut" "$DATA/osm/$s-latest.osm.pbf"
    cuts+=("$cut")
done
osmium merge --overwrite -o "$OUT.part" "${cuts[@]}"
mv "$OUT.part" "$OUT"
echo "fetch_osm: $(stat -f %z "$OUT" 2>/dev/null || stat -c %s "$OUT") bytes"
