#!/usr/bin/env bash
# Regenerate docs/wasm-anatomy.html from the current cnitro WASM sources.
#
# Pipeline:
#   1. build the three production modules (rules/guards/bots) with the real
#      Makefile flags  (-Oz, --strip-all) — these are byte-identical to what
#      ships in supabase/functions/_shared/wasm/
#   2. build name-preserving companions (same flags minus --strip-all) so the
#      disassembler can recover function names over the identical code bytes
#   3. build a symbol -> source-file map from the per-file object symbol tables
#   4. parse + disassemble every module  (analyze.mjs)
#   5. emit the single self-contained HTML  (build_html.mjs -> app.js + CSS)
#
# Requires: clang (wasm32 target), wasm-ld, llvm-nm, node.  Run from anywhere.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CNITRO="$(cd "$HERE/../.." && pwd)"          # tools/wasm_anatomy -> cnitro
REPO="$(cd "$CNITRO/.." && pwd)"
BUILD="$CNITRO/build"
NAMED="$BUILD/named"
WORK="$(mktemp -d)"
OUT="${1:-$REPO/docs/wasm-anatomy.html}"
WASM_CC="${WASM_CC:-clang}"

echo "cnitro=$CNITRO  out=$OUT"
cd "$CNITRO"

echo "[1/5] building stripped production modules"
make build/rules.wasm build/guards.wasm build/bots.wasm >/dev/null

echo "[2/5] building name-preserving companions"
mkdir -p "$NAMED"
# strip --strip-all from each real link command, redirect output into named/
for m in rules guards; do
  make --always-make -n "build/$m.wasm" 2>/dev/null | grep -E "clang.* -o build/$m.wasm" \
    | sed "s/-Wl,--strip-all //; s#-o build/$m.wasm#-o build/named/$m.named.wasm#" | bash
done
# bots: relink the existing objects (same exports) without --strip-all
EXPORTS=$(make --always-make -n build/bots.wasm 2>/dev/null | tr ' ' '\n' | grep '^-Wl,--export=' | sort -u | tr '\n' ' ')
$WASM_CC --target=wasm32 -nostdlib -Wl,--no-entry -Wl,--export-memory \
  -Wl,-z,stack-size=262144 -Wl,--stack-first $EXPORTS build/botobj/*.o -o "$NAMED/bots.named.wasm"

echo "[3/5] symbol -> source-file map"
: > "$WORK/symfile.tsv"
for o in build/botobj/*.o; do
  base="$(basename "$o" .o)"
  llvm-nm --defined-only "$o" 2>/dev/null | awk -v f="$base" '$2 ~ /[TtWw]/ {print $3"\t"f}'
done >> "$WORK/symfile.tsv"
# guards api object is not in botobj — compile just for its symbol names
$WASM_CC --target=wasm32 -Oz -nostdlib -ffreestanding -mbulk-memory -isystem wasm/include -Isrc \
  -D_Thread_local= -DDEAL_RNG_DISABLED -DMAX_LOG_PAIRS=64 -DMAX_BATTLES=64 -DMAX_LOGS=64 \
  -c wasm/wasm_guards_api.c -o "$WORK/wasm_guards_api.o" 2>/dev/null
llvm-nm --defined-only "$WORK/wasm_guards_api.o" 2>/dev/null | awk '$2 ~ /[TtWw]/ {print $3"\twasm_guards_api"}' >> "$WORK/symfile.tsv"
sort -u "$WORK/symfile.tsv" -o "$WORK/symfile.tsv"

echo "[4/5] write config + parse + disassemble"
# The config is what makes analyze/build_html generic — cnitro just fills in the
# rich fields (named companions, symfile, per-module blurbs).
WORK="$WORK" BUILD="$BUILD" node - > "$WORK/config.json" <<'NODE'
const W = process.env.WORK, B = process.env.BUILD;
console.log(JSON.stringify({
  title: 'WASM Anatomy · foolish / cnitro',
  subtitle: 'foolish · cnitro rules kernel',
  symfile: `${W}/symfile.tsv`,
  modules: [
    { key:'rules', human:'rules.wasm', wasm:`${B}/rules.wasm`, named:`${B}/named/rules.named.wasm`,
      blurb:'The production rules kernel: engine + legal-move generator + replay codec, compiled freestanding. Shipped base64-embedded in rules_wasm.ts and imported by the Deno edge functions AND the browser (replay decode).' },
    { key:'guards', human:'guards.wasm', wasm:`${B}/guards.wasm`, named:`${B}/named/guards.named.wasm`,
      blurb:'The smallest kernel: game.c only — no move enumeration, no replay codec. Backs the browser UI move-gates (validate-only) and optimistic apply. One engine, not two.' },
    { key:'bots', human:'bots.wasm', wasm:`${B}/bots.wasm`, named:`${B}/named/bots.named.wasm`,
      blurb:'The rules kernel PLUS every algorithmic bot strategy and the choose-move bridge. A superset of rules.wasm, loaded only where bots run. Ships as a gzip static asset, not a base64 embed.' },
  ],
}));
NODE
node "$HERE/analyze.mjs" "$WORK" "$WORK/config.json"

echo "[5/5] render HTML"
cp "$HERE/app.js" "$WORK/app.js"
node "$HERE/build_html.mjs" "$WORK" "$OUT" "$WORK/config.json"

rm -rf "$WORK"
echo "done -> $OUT"
