#!/bin/bash
# Builds the cdom table into out/: table.wasm and index.html (the shim inlined).
# Run from anywhere. WASM_CC overrides the compiler (homebrew LLVM by default).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
c="$here/.."
shared="$c/../../shared/c"
CC="${WASM_CC:-/opt/homebrew/opt/llvm/bin/clang}"
out="$here/out"
obj="$out/obj"
mkdir -p "$obj"

# The kernel's wasm flags (c/Makefile WASM_FLAGS, WASM_BOT_CFLAGS) without the
# server-only parts, and without -Werror: the kernel is not this app's to lint.
KFLAGS=(--target=wasm32 -Oz -nostdlib -ffreestanding -mbulk-memory -mno-nontrapping-fptoint
  -ffp-contract=off
  -isystem "$c/wasm/include" -isystem "$shared/wasm/include" -I"$c/src"
  -D_Thread_local= -DMAX_LOG_PAIRS=64 -DMAX_LEGAL_MOVES=4096 -DMAX_MOVE_CARDS=28 -DMAX_BATTLES=64
  -DFOOLISH_SEEDED_BOTS_ONLY -Wall -Wextra -Wno-unused-parameter)
# cdom and the app: cdom's own warnings, as errors.
AFLAGS=("${KFLAGS[@]}" -I"$here/vendor" -I"$c" -Werror)

KERNEL=(src/game.c src/legal.c src/view.c src/awire.c src/evwire.c src/anim_plan.c src/client_table.c
  src/card.c src/hand_layout.c src/roster.c src/bot_drive.c src/handwritten_prod_strategy.c)
SHARED=(deal_rng.c wasm/libc.c wasm/libm.c)

objs=()
for f in "${KERNEL[@]}"; do
  o="$obj/$(basename "${f%.c}").o"
  "$CC" "${KFLAGS[@]}" -c "$c/$f" -o "$o"
  objs+=("$o")
done
for f in "${SHARED[@]}"; do
  o="$obj/shared_$(basename "${f%.c}").o"
  "$CC" "${KFLAGS[@]}" -c "$shared/$f" -o "$o"
  objs+=("$o")
done
# cdom's mini.c is not linked: the kernel's libc.c brings memcmp and strlen (mini.c's are weak, so
# either would link). cdom's weak malloc is the one allocator for both.
for f in "$here/vendor/cdom.c" "$here/src/table_app.c" "$here/src/kernel_glue.c" "$here/src/textures.c" "$here/src/crtrig.c" "$here/src/jsmath.c"; do
  o="$obj/app_$(basename "${f%.c}").o"
  "$CC" "${AFLAGS[@]}" -c "$f" -o "$o"
  objs+=("$o")
done

"$CC" --target=wasm32 -nostdlib -Wl,--no-entry -Wl,--export-memory -Wl,--stack-first -Wl,-z,stack-size=65536 \
  -Wl,--strip-all \
  -o "$out/table.wasm" "${objs[@]}"
if command -v wasm-opt > /dev/null; then
  wasm-opt -Oz --enable-bulk-memory --enable-mutable-globals --enable-sign-ext "$out/table.wasm" -o "$out/table.wasm"
fi

# The texture generators alone, for tools/texparity.mjs.
"$CC" --target=wasm32 -O2 -nostdlib -mbulk-memory -ffp-contract=off -Wall -Wextra -Werror -Wl,--no-entry \
  -Wl,--export-memory -Wl,--strip-all -o "$out/tex_test.wasm" "$here/tests/tex_test.c" "$here/src/textures.c" "$here/src/crtrig.c" "$here/src/jsmath.c"

# The network imports' own test module (tests/net_test.mjs): cdom and nothing of the kernel.
"$CC" --target=wasm32 -Oz -nostdlib -mbulk-memory -Wall -Wextra -Werror -I"$here/vendor" -Wl,--no-entry \
  -Wl,--export-memory -Wl,--strip-all -o "$out/net_test.wasm" "$here/tests/net_test.c" "$here/vendor/cdom.c" "$here/vendor/cdom_net.c" "$here/vendor/mini.c"

# The page: the shim minified and inlined (as cdom's bench does), the app's CSS linked.
"$here/tools/node_modules/.bin/terser" "$here/vendor/cdom.js" -c passes=2 -m -o "$out/cdom.min.js" 2> /dev/null \
  || cp "$here/vendor/cdom.js" "$out/cdom.min.js"
sed -e "/\/\*CDOM_SHIM\*\//{r $out/cdom.min.js" -e "d;}" "$here/src/index.html" > "$out/index.html"
printf 'table.wasm: %s bytes, %s brotli, %s gzip\n' "$(wc -c < "$out/table.wasm" | tr -d ' ')" \
  "$(brotli -c -q 11 "$out/table.wasm" | wc -c | tr -d ' ')" "$(gzip -9c "$out/table.wasm" | wc -c | tr -d ' ')"
