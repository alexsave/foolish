#!/usr/bin/env bash
# The generated-accessor performance gate, three ways: tsx (unbundled, how e2e
# runs), node's own type stripping (unbundled ESM), esbuild --bundle --minify.
# Every row runs in its own fresh process, REPS times (default 3); each process
# reports a median over RUNS x N calls and the table shows the median of those.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$here/build/bench"
REPS="${REPS:-3}"
mkdir -p "$out"
cd "$root"
# The test build (the shipped objects, plus the resident deal the bench starts
# from). The bundle moves the module out of test/, so it is told where it is.
make -s -C "$root/c" WASM_CC="${WASM_CC:-/opt/homebrew/opt/llvm/bin/clang}" wasm-bots-test >/dev/null
export BENCH_WASM="$root/c/build/bots_test.wasm"
node_modules/.bin/esbuild "$here/test/bench.ts" --bundle --minify --format=esm --platform=node \
  --outfile="$out/bench.min.mjs" --log-level=error
res="$out/bench.tsv"; : > "$res"
n="$(BENCH_LIST=1 node --no-warnings "$here/test/bench.ts" | wc -l | tr -d ' ')"
for ((rep = 0; rep < REPS; rep++)); do
  for ((i = 0; i < n; i++)); do
    BENCH_ONLY=$i BENCH_MODE=tsx node --import tsx "$here/test/bench.ts" >> "$res"
    BENCH_ONLY=$i BENCH_MODE=strip node --no-warnings "$here/test/bench.ts" >> "$res"
    BENCH_ONLY=$i BENCH_MODE=bundle node "$out/bench.min.mjs" >> "$res"
  done
done
node -e '
const rows = new Map();
for (const l of require("fs").readFileSync(process.argv[1], "utf8").trim().split("\n")) {
  const [mode, name, ns] = l.split("\t");
  if (!rows.has(name)) rows.set(name, { tsx: [], strip: [], bundle: [] });
  rows.get(name)[mode].push(Number(ns));
}
const med = a => a.sort((x, y) => x - y)[a.length >> 1].toFixed(1);
const w = Math.max(...[...rows.keys()].map(k => k.length));
console.log(`${"ns/op (median of fresh processes)".padEnd(w)}  ${"tsx".padStart(7)} ${"strip".padStart(7)} ${"bundle".padStart(7)}`);
for (const [name, r] of rows) console.log(`${name.padEnd(w)}  ${med(r.tsx).padStart(7)} ${med(r.strip).padStart(7)} ${med(r.bundle).padStart(7)}`);
' "$res"
