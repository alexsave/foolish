#!/bin/bash
# Extra jco variants + a size table. Run after build.sh.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
JCO="node node_modules/@bytecodealliance/jco/dist/jco.js"
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) PLAT=aarch64-macos ;;
    Darwin-x86_64) PLAT=x86_64-macos ;;
    Linux-aarch64) PLAT=aarch64-linux ;;
    Linux-x86_64) PLAT=x86_64-linux ;;
    *) echo "unsupported platform $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac
WT=.tools/wasm-tools-1.259.0-$PLAT/wasm-tools
TERSER=node_modules/.bin/terser

rm -rf out/jco-sync-min out/jco-sync-opt
$JCO transpile out/kernel.component.wasm -o out/jco-sync-min --name kernel --instantiation sync \
    --minify --valid-lifting-optimization --no-typescript -q
$JCO transpile out/kernel.component.wasm -o out/jco-sync-opt --name kernel --instantiation sync \
    --minify --optimize --valid-lifting-optimization --no-typescript -q || echo "jco --optimize FAILED"

# Baseline JS glue = the marshal/unmarshal section of bench.mjs, as its own module.
awk '/^\/\/ ---- baseline: hand-written marshal/{on=1} /^\/\/ ---- component via jco/{on=0} on' bench.mjs \
    | grep -v "^const base = new WebAssembly" > out/baseline_glue.js
echo "export { marshal, unmarshal };" >> out/baseline_glue.js
"$TERSER" out/baseline_glue.js -c -m --module -o out/baseline_glue.min.js
"$TERSER" out/jco-sync/kernel.js -c -m --module -o out/jco-sync.kernel.terser.js
"$TERSER" out/jco-1.10-sync/kernel.js -c -m --module -o out/jco-1.10-sync.kernel.terser.js

row() { printf '%-52s %8d %8d %8d\n' "$1" "$(wc -c < "$2")" "$(gzip -9c "$2" | wc -c)" "$(brotli -c -q 11 "$2" 2>/dev/null | wc -c || echo 0)"; }
printf '%-52s %8s %8s %8s\n' file raw gzip-9 brotli-11
row "core: baseline.wasm (repo flags)"               out/baseline.wasm
row "core: component_core.wasm (pre-embed)"           out/component_core.wasm
row "core: jco-sync/kernel.core.wasm (as shipped)"    out/jco-sync/kernel.core.wasm
row "component: kernel.component.wasm"                out/kernel.component.wasm
row "JS: baseline glue (hand)"                         out/baseline_glue.js
row "JS: baseline glue (terser)"                       out/baseline_glue.min.js
row "JS: jco 1.34 sync kernel.js"                      out/jco-sync/kernel.js
row "JS: jco 1.34 sync kernel.js (terser)"             out/jco-sync.kernel.terser.js
row "JS: jco 1.34 sync --minify --valid-lifting-opt"   out/jco-sync-min/kernel.js
[ -f out/jco-sync-opt/kernel.js ] && row "JS: jco 1.34 sync --minify --optimize"  out/jco-sync-opt/kernel.js
[ -f out/jco-sync-opt/kernel.core.wasm ] && row "core: jco --optimize kernel.core.wasm" out/jco-sync-opt/kernel.core.wasm
row "JS: jco 1.34 default async (core inlined b64)"   out/jco/kernel.js
row "JS: jco 1.34 default async --minify"              out/jco-min/kernel.js
row "JS: jco 1.10.2 sync kernel.js"                    out/jco-1.10-sync/kernel.js
row "JS: jco 1.10.2 sync kernel.js (terser)"           out/jco-1.10-sync.kernel.terser.js
if [ -f out/embind/kernel.js ]; then
    row "JS: embind kernel.js (emcc -Oz)"              out/embind/kernel.js
    row "core: embind kernel.wasm (emcc -Oz)"          out/embind/kernel.wasm
fi
echo
echo "core wasm imports/exports (jco-sync/kernel.core.wasm):"
"$WT" print out/jco-sync/kernel.core.wasm | grep -E '^\s*\((import|export)'
echo "baseline.wasm imports/exports:"
"$WT" print out/baseline.wasm | grep -E '^\s*\((import|export)'
if [ -f out/embind/kernel.wasm ]; then
    echo "embind kernel.wasm import count: $("$WT" print out/embind/kernel.wasm | grep -cE '^\s*\(import')"
fi
echo "shim imports in generated JS:"
grep -l "preview2-shim" out/jco*/kernel.js || echo "  none"
