#!/usr/bin/env bash
# Regenerate the call atlas from the current sources.
#
# Pipeline:
#   1. parse each language  (clang / tsc / syn / a Swift lexer / a SQL replayer)
#   2. merge the four graphs, wire the cross-language edges, tag every function
#      with a group                                                    (merge.py)
#   3. lay out the file graph by force, once per toggle combination    (layout.py)
#   4. pack it columnar                                                  (pack.py)
#   5. inline everything into one standalone HTML file             (build_html.py)
#
# Requires: clang, node (with typescript installed - the repo's node_modules is
# enough), cargo, python3 with numpy. Swift needs no toolchain: that analyzer is
# a lexer. Run from anywhere.
#
#   tools/callgraph/generate.sh [out.html]
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$REPO/docs/call-atlas.html}"
WORK="${CALLGRAPH_WORK:-$(mktemp -d)}"
mkdir -p "$WORK"
echo "repo=$REPO  work=$WORK  out=$OUT"

echo "[1/5] parsing C  (clang AST, every TU in two build configurations)"
python3 "$HERE/analyze_c.py" "$REPO" "$WORK/c.json"

echo "[1/5] parsing TypeScript  (compiler API + checker)"
node "$HERE/analyze_ts.mjs" "$REPO" "$WORK/ts.json"

echo "[1/5] parsing Swift  (lexical, no toolchain needed)"
python3 "$HERE/analyze_swift.py" "$REPO" "$WORK/swift.json" "$WORK/c.json"

echo "[1/5] parsing Rust  (syn AST)"
cargo build --release --quiet --manifest-path "$HERE/analyze_rust/Cargo.toml"
# shellcheck disable=SC2046  # the file list is deliberately word-split
"$HERE/analyze_rust/target/release/rsparse" "$REPO" "$WORK/rust.json" \
    $(cd "$REPO" && git ls-files '*.rs')

echo "[1/5] reading SQL  (migrations replayed in order)"
python3 "$HERE/analyze_sql.py" "$REPO" "$WORK/sql.json"

echo "[2/5] merging + cross-language wiring"
python3 "$HERE/merge.py" "$WORK"

echo "[3/5] laying out (four toggle combinations, ~2 min)"
python3 "$HERE/layout.py" "$WORK"

echo "[4/5] packing"
python3 "$HERE/pack.py" "$WORK"

echo "[5/5] rendering"
python3 "$HERE/build_html.py" "$WORK" "$OUT"

if [ -z "${CALLGRAPH_WORK:-}" ]; then rm -rf "$WORK"; fi
echo "done -> $OUT"
