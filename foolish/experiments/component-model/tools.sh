#!/bin/bash
# Fetches the two pinned Bytecode Alliance release binaries into .tools/.
# Nothing here is committed: .tools/ is ignored, and this script re-creates it.
#
#   bash tools.sh          # download if missing
#   bash tools.sh --force  # re-download
#
# If a release archive is gone or your platform has no published build, this
# says so and exits non-zero rather than silently building something else.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

WASM_TOOLS_VERSION=1.259.0
WIT_BINDGEN_VERSION=0.62.0

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) PLAT=aarch64-macos ;;
    Darwin-x86_64) PLAT=x86_64-macos ;;
    Linux-aarch64) PLAT=aarch64-linux ;;
    Linux-x86_64) PLAT=x86_64-linux ;;
    *) echo "no published wasm-tools/wit-bindgen build for $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac

if [ "${1:-}" = "--force" ]; then rm -rf .tools; fi

fetch() {
    local repo=$1 name=$2 version=$3
    local dir="$name-$version-$PLAT"
    if [ -x ".tools/$dir/$name" ]; then return 0; fi
    local url="https://github.com/bytecodealliance/$repo/releases/download/v$version/$dir.tar.gz"
    echo "fetching $url"
    mkdir -p .tools
    if ! curl -sSfL "$url" | tar -xz -C .tools; then
        echo "could not fetch $url - check the release still publishes $PLAT" >&2
        exit 1
    fi
}

fetch wasm-tools wasm-tools "$WASM_TOOLS_VERSION"
fetch wit-bindgen wit-bindgen "$WIT_BINDGEN_VERSION"

".tools/wasm-tools-$WASM_TOOLS_VERSION-$PLAT/wasm-tools" --version
".tools/wit-bindgen-$WIT_BINDGEN_VERSION-$PLAT/wit-bindgen" --version
