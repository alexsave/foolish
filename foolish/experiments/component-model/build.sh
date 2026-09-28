#!/bin/bash
# Builds the three variants of one round trip over one C struct: the repo's
# hand-marshalled baseline, the same state behind a WIT interface as a component
# transpiled by jco, and the same state behind Emscripten's embind.
#
#   npm ci && bash tools.sh && bash build.sh && bash sizes.sh
#   JCO_DIRS=jco-sync,jco-sync-nodebug,jco-1.10-sync node bench.mjs
#
# Needs: a clang that can target wasm32 (homebrew LLVM on this Mac - plain Apple
# clang cannot), wasm-opt and brotli on PATH, and .tools/ from tools.sh.
# Run from anywhere. See docs/CODEGEN_ALTERNATIVES.md.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
CC=${WASM_CC:-/opt/homebrew/opt/llvm/bin/clang}
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) PLAT=aarch64-macos ;;
    Darwin-x86_64) PLAT=x86_64-macos ;;
    Linux-aarch64) PLAT=aarch64-linux ;;
    Linux-x86_64) PLAT=x86_64-linux ;;
    *) echo "unsupported platform $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac
WT=.tools/wasm-tools-1.259.0-$PLAT/wasm-tools
WB=.tools/wit-bindgen-0.62.0-$PLAT/wit-bindgen
JCO="node node_modules/@bytecodealliance/jco/dist/jco.js"
[ -x "$WT" ] && [ -x "$WB" ] || { echo "run 'bash tools.sh' first (missing $WT / $WB)" >&2; exit 1; }
[ -d node_modules/@bytecodealliance/jco ] || { echo "run 'npm ci' in $HERE first" >&2; exit 1; }
mkdir -p out gen

# The repo's wasm flags (c/Makefile WASM_FLAGS), minus the kernel -D knobs.
FLAGS=(--target=wasm32 -Oz -nostdlib -ffreestanding -mbulk-memory -mno-nontrapping-fptoint
       -Wall -Wextra -Wno-unused-parameter
       -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=262144 -Wl,--stack-first -Wl,--strip-all)

# ---- baseline: hand-marshalled IO buffer ----
"$CC" "${FLAGS[@]}" c/baseline.c -o out/baseline.wasm

# ---- component: wit-bindgen c -> core wasm -> embed WIT -> component ----
"$WB" c --no-object-file --out-dir gen wit >/dev/null
# --no-object-file: the WIT is embedded AFTER linking, with `component embed`,
# so this build cannot depend on what a given lld does to custom sections under
# --strip-all. (Re-checked 2026-09-17 with clang 22.1.8: linking the generated
# kernel_component_type.o directly ALSO works, and --strip-all leaves
# `component-type:kernel` in place. The 2026-09-17 prototype believed otherwise.
# The embed route is kept because getting this wrong fails silently: see
# docs/CODEGEN_ALTERNATIVES.md finding 3.)
# Drop the generated force-link stub that points at the omitted .o.
awk '/Ensure that the \*_component_type.o/{exit} {print}' gen/kernel.c > gen/kernel.c.trimmed
mv gen/kernel.c.trimmed gen/kernel.c
"$CC" "${FLAGS[@]}" -Wno-unused-variable -Ic/include -Ic -Igen gen/kernel.c c/component_impl.c -o out/component_core.wasm
"$WT" component embed wit out/component_core.wasm -o out/component_embedded.wasm
"$WT" component new out/component_embedded.wasm -o out/kernel.component.wasm
"$WT" validate out/kernel.component.wasm

# ---- jco transpile (plain and --minify), sync instantiation mode ----
rm -rf out/jco out/jco-min out/jco-sync
$JCO transpile out/kernel.component.wasm -o out/jco --name kernel -q
$JCO transpile out/kernel.component.wasm -o out/jco-min --name kernel --minify -q
$JCO transpile out/kernel.component.wasm -o out/jco-sync --name kernel --instantiation sync -q

# Comparison outputs: pre-async-runtime jco (1.10.2, pinned as npm alias jco110),
# and a DIAGNOSTIC copy of the 1.34 glue with _debugLog's per-call
# process.env.JCO_DEBUG probe stubbed out (not a shippable config).
rm -rf out/jco-1.10-sync out/jco-sync-nodebug
node node_modules/jco110/src/jco.js transpile out/kernel.component.wasm -o out/jco-1.10-sync --name kernel --instantiation sync -q
cp -R out/jco-sync out/jco-sync-nodebug
perl -0pi -e 's/if \(!globalThis\?\.process\?\.env\?\.JCO_DEBUG\) \{ return; \}/return;/' out/jco-sync-nodebug/kernel.js

# ---- third variant: Emscripten embind over the same KGame ----
# Skipped with a note when emcc is not installed; the bench and the size table
# both leave the embind rows out in that case rather than inventing them.
rm -rf out/embind
if command -v emcc >/dev/null; then
    mkdir -p out/embind
    emcc -Oz --bind -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=node \
         -sALLOW_MEMORY_GROWTH=1 -sEXPORT_NAME=createKernel \
         emscripten/embind_impl.cc -o out/embind/kernel.js
else
    echo "emcc not on PATH - skipping the embind variant (install emscripten to measure it)"
fi

# Also run the repo's wasm-opt pass over both cores for a fair size line.
wasm-opt -Oz --enable-bulk-memory out/baseline.wasm -o out/baseline.opt.wasm
