#!/bin/bash
# SessionStart hook — ensure binaryen (wasm-opt) is available.
#
# foolish/c/Makefile runs `wasm-opt` on every linked kernel module by default
# (WASM_BOTS_POSTOPT), so `npm run wasm` — which builds the shipped
# foolish/sdk/ts/wasm/web.wasm.gz and the Oracle's two — needs it on PATH. Everything
# else the repo builds with (clang/wasm-ld, node, postgres) is already
# provisioned in the web environment; binaryen is the only gap.
#
# Idempotent: no-op once wasm-opt is present. Web-only: local machines manage
# their own toolchain.
set -euo pipefail

# Only run in Claude Code on the web; local dev manages its own toolchain.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

# Already installed → nothing to do (keeps warm-container restarts instant).
if command -v wasm-opt >/dev/null 2>&1; then
  exit 0
fi

echo "[session-start] installing binaryen (wasm-opt) for c wasm rebuilds…"
if command -v apt-get >/dev/null 2>&1; then
  if sudo apt-get install -y binaryen >/dev/null 2>&1 || apt-get install -y binaryen >/dev/null 2>&1; then
    echo "[session-start] binaryen installed: $(wasm-opt --version 2>/dev/null || echo unknown)"
  else
    echo "[session-start] WARN: could not install binaryen; \`npm run wasm\` will fail until it is on PATH" >&2
  fi
else
  echo "[session-start] WARN: no apt-get available; install binaryen manually for wasm rebuilds" >&2
fi
