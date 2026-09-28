#!/usr/bin/env bash
# Regenerate docs/wasm-anatomy.html from the current cnitro WASM sources.
#
# Pipeline:
#   1. build the three production modules (rules/guards/bots) with the real
#      Makefile flags  (-Oz, --strip-all) — these are byte-identical to what
#      ships in sdk/ts/wasm/
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
# Force-relink the wasm outputs from the (cached) objects: make is timestamp-
# based and won't rebuild them when only a recipe VARIABLE changed (e.g. a prior
# `make wasm-bots WASM_BOTS_POSTOPT=…` left a differently-optimized build/bots.wasm
# on disk). Removing the linked artifacts guarantees they reflect the DEFAULT
# flags; the objects stay cached so this is cheap. The [2/5] guard would catch a
# mismatch anyway, but this prevents the abort in the common stale-build case.
rm -f build/bots.wasm build/named/bots.named.wasm build/web.wasm build/named/web.named.wasm
make build/bots.wasm build/web.wasm >/dev/null

echo "[2/5] building name-preserving companions"
mkdir -p "$NAMED"
# The module builds from separate OBJECTS, so the companion is produced by
# the Makefile from the IDENTICAL objects + link + wasm-opt as the shipped module
# — the only difference is no -Wl,--strip-all and wasm-opt --debuginfo to keep the
# function name section. (A hand-rolled relink would skip the wasm-opt pass and
# its inlining/DCE would shift every name off the optimized bytes.)
make build/named/bots.named.wasm build/named/web.named.wasm >/dev/null
# Guard: for EACH module, every function body must be byte-identical to the
# shipped module's function at the same index — that per-function identity is what
# lets the name section map 1:1 onto the shipped bytes. (wasm-opt --debuginfo
# leaves harmless framing differences in the CODE section as a whole, so compare
# bodies by index, not the raw section.) Fail loudly on any drift.
for m in bots web; do
  node -e '
    const fs=require("fs");
    const bodies=p=>{const b=fs.readFileSync(p);let i=8;const out=[];while(i<b.length){const id=b[i++];let sh=0,ln=0,by;do{by=b[i++];ln|=(by&127)<<sh;sh+=7}while(by&128);if(id===10){let j=i;let s2=0,cnt=0,c;do{c=b[j++];cnt|=(c&127)<<s2;s2+=7}while(c&128);for(let f=0;f<cnt;f++){let sz=0,ss=0,cc;do{cc=b[j++];sz|=(cc&127)<<ss;ss+=7}while(cc&128);out.push(b.slice(j,j+sz));j+=sz;}}i+=ln;}return out;};
    const c=bodies(process.argv[1]), s=bodies(process.argv[2]), m=process.argv[3];
    if(c.length!==s.length){console.error("ERROR: "+m+".named companion has "+c.length+" funcs vs shipped "+s.length+" — names would mis-map. Aborting.");process.exit(1);}
    for(let k=0;k<s.length;k++) if(Buffer.compare(c[k],s[k])!==0){console.error("ERROR: "+m+".named companion function #"+k+" differs from shipped build/"+m+".wasm — names would mis-map. Aborting.");process.exit(1);}
  ' "$NAMED/$m.named.wasm" "build/$m.wasm" "$m" || exit 1
done

echo "[3/5] symbol -> source-file map"
# The module compiles to per-file objects, so every symbol has a home object.
: > "$WORK/symfile.tsv"
for o in build/botobj/*.o; do
  base="$(basename "$o" .o)"
  llvm-nm --defined-only "$o" 2>/dev/null | awk -v f="$base" '$2 ~ /[TtWw]/ {print $3"\t"f}'
done >> "$WORK/symfile.tsv"
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
    { key:'web', human:'web.wasm', wasm:`${B}/web.wasm`, named:`${B}/named/web.named.wasm`,
      blurb:'What a browser downloads. The same object files as bots.wasm under a smaller export allow-list, so wasm-ld drops everything no browser call site reaches: the client slot, the animation plan, the replay reader and the message decode stay; the C Table and every Monte-Carlo brain go. Compare the two pages below - the difference is the whole argument for linking per call site.' },
    { key:'bots', human:'bots.wasm', wasm:`${B}/bots.wasm`, named:`${B}/named/bots.named.wasm`,
      blurb:'The server-side link, read off disk by the Supabase edge functions and by Node: the rules, the C Table the server plays, the web client slot, the replay and message codecs, and every algorithmic bot strategy.' },
  ],
}));
NODE
node "$HERE/analyze.mjs" "$WORK" "$WORK/config.json"

echo "[5/5] render HTML"
cp "$HERE/app.js" "$WORK/app.js"
node "$HERE/build_html.mjs" "$WORK" "$OUT" "$WORK/config.json"

rm -rf "$WORK"
echo "done -> $OUT"
