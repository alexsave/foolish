#!/usr/bin/env bash
# Generic WASM Anatomy — build the interactive page for ANY .wasm file(s).
#
#   wasm-anatomy.sh a.wasm [b.wasm ...] [-o out.html] [-t "Title"]
#
# No build step, no toolchain: each module is parsed and disassembled straight
# from its bytes. Function names come from the wasm's own "name" custom section
# if present (compile with -g or link without --strip-all to keep it); otherwise
# functions are shown by index. Source-file attribution is a cnitro-only extra —
# see generate.sh — so here functions are grouped by name prefix instead.
#
# Requires only: node.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="wasm-anatomy.html"; TITLE="WASM Anatomy"; SUBTITLE=""
WASMS=()
while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUT="$2"; shift 2 ;;
    -t) TITLE="$2"; shift 2 ;;
    -s) SUBTITLE="$2"; shift 2 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) WASMS+=("$1"); shift ;;
  esac
done
[ ${#WASMS[@]} -gt 0 ] || { echo "usage: wasm-anatomy.sh a.wasm [b.wasm ...] [-o out.html] [-t Title]" >&2; exit 1; }

WORK="$(mktemp -d)"
# Build the config from the given files (key = basename, human = filename).
OUT_ABS="$(mkdir -p "$(dirname "$OUT")" && cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
TITLE="$TITLE" SUBTITLE="$SUBTITLE" node - "${WASMS[@]}" > "$WORK/config.json" <<'NODE'
const path = require('path'), fs = require('fs');
const files = process.argv.slice(2);
const seen = {};
const modules = files.map(f => {
  let key = path.basename(f).replace(/\.wasm$/i, '').replace(/[^A-Za-z0-9_.-]/g, '_');
  if (seen[key] != null) key = key + '_' + (++seen[key]); else seen[key] = 0;
  return { key, human: path.basename(f), wasm: path.resolve(f) };
});
console.log(JSON.stringify({ title: process.env.TITLE, subtitle: process.env.SUBTITLE, modules }));
NODE

node "$HERE/analyze.mjs" "$WORK" "$WORK/config.json"
cp "$HERE/app.js" "$WORK/app.js"
node "$HERE/build_html.mjs" "$WORK" "$OUT_ABS" "$WORK/config.json"
rm -rf "$WORK"
echo "done -> $OUT_ABS"
