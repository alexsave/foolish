#!/usr/bin/env bash
# CI: install the PINNED wasm toolchain and export it for the rest of the job.
# Installs only - see scripts/ci_wasm.sh for the script that then builds.
#
# WHY A SECOND TOOLCHAIN SCRIPT, next to scripts/ci_llvm.sh. They answer
# different questions and only one of them decides what ships.
#
#   ci_llvm.sh   libclang, for tools/structgen. The generated TEXT is measured
#                byte-identical across Homebrew clang 22.1.8, apt.llvm.org
#                clang 22.1.8 and Ubuntu's own clang 18.1.3, and the layout hash
#                is independent of the libclang version by construction
#                (shared/tools/structgen/test/hash.sh). So the distro LLVM is
#                enough there and that script is left exactly as it was.
#
#   this script  the compiler that produces bytes a visitor DOWNLOADS. Here the
#                version is not a detail, it is a kilobyte and a half. Measured
#                on one tree, sdk/ts/wasm/bots.wasm.gz:
#
#                  clang 22.1.8 + binaryen 130   191,485 B raw
#                  clang 22.1.8 + binaryen 108   191,729 B raw   +244 B
#                  clang 18.1.3 + binaryen 108   194,997 B raw   +3,512 B
#
#                clang 18 is the one ci_llvm.sh installs, and it costs 3.5 KB of
#                kernel and ~1.7 KB of download on the module every visitor
#                fetches. A lane that builds a shipped module therefore uses
#                this pin, and no lane builds one with whatever clang it found.
#
# THE PIN IS WHAT MAKES THE MODULE REPRODUCIBLE, which is the property the old
# committed-artifact arrangement assumed was unobtainable. With clang 22.1.8 and
# binaryen 130, `c/build/bots.wasm` comes out byte-identical on macOS arm64
# (Homebrew), Linux arm64 and Linux x86_64 (apt.llvm.org, and x86_64 is what
# ubuntu-latest runs) - all md5 ac53b4dc5484aabad381d2d7bc451088, 191,485 B.
# Same for oracle.wasm and oracle-mt.wasm. See scripts/wasm_build.sh.
#
# WHAT IS *NOT* REPRODUCIBLE, and must not be gated on: the .gz. gzip -9 -n over
# that one identical 191,485 B module gives
#
#   Apple gzip 487.0.1 (macOS)   81,892 B
#   GNU gzip 1.12 (ubuntu)       82,043 B    +151 B
#   node 26 zlib level 9         81,892 B
#   node 20 zlib level 9         82,468 B    +576 B
#
# so "how big is the module" and "which gzip ran" are different questions, and
# only the first is a fact about this repo. e2e/mem/wasm_memory.test.ts gates the
# raw size for that reason and gives the .gz a ceiling wide enough to cover that
# 576 B spread. No compressor is pinned here: pinning one would mean a new
# dependency on every machine to make a number stable that nothing needs to be
# stable, since the bytes a visitor gets are whatever the ONE deploy lane wrote.
#
# apt.llvm.org IS reachable from this project's environments - verified from the
# owner's Mac (HTTP 200) and from inside a clean ubuntu:24.04 container, where
# llvm.sh installs 22 and builds the modules. .github/workflows/wasm.yml has
# also been pinning LLVM 22 from it for the structgen job since before this.
#
# binaryen comes from the GitHub release tarball, not apt: Ubuntu 24.04 ships
# binaryen 108 and the pin is 130.
#
# Idempotent, and safe to run after ci_llvm.sh or ci_bots_test_wasm.sh.
#
# Usage:
#   . scripts/ci_wasm_toolchain.sh   # install and export WASM_CC/LLVM_PREFIX/PATH
#
# Sourced by scripts/ci_wasm.sh (which then builds the shipped modules) and by
# scripts/ci_bots_test_wasm.sh (which builds the uncommitted test module). ONE
# toolchain behind both, so no lane ends up holding a shipped module from one
# clang and a test module from another.
set -euo pipefail

WASM_LLVM_VERSION="${WASM_LLVM_VERSION:-22}"
WASM_BINARYEN_VERSION="${WASM_BINARYEN_VERSION:-130}"
LLVM="/usr/lib/llvm-$WASM_LLVM_VERSION"
BINARYEN="/opt/binaryen-version_$WASM_BINARYEN_VERSION"

if [ ! -x "$LLVM/bin/clang" ]; then
  SUDO="$(command -v sudo || true)"
  $SUDO apt-get update -qq
  # gcc, for `cc`: structgen's Makefile builds the generator with $(CC), which
  # defaults to cc, and every wasm target runs structgen first for the layout
  # hash it compiles in. Named rather than inherited, same as ci_llvm.sh.
  $SUDO apt-get install -y --no-install-recommends wget gnupg lsb-release \
      software-properties-common ca-certificates make gcc gzip
  wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
  $SUDO bash /tmp/llvm.sh "$WASM_LLVM_VERSION"
  $SUDO apt-get install -y --no-install-recommends \
      "libclang-$WASM_LLVM_VERSION-dev" "lld-$WASM_LLVM_VERSION"
fi

if [ ! -x "$BINARYEN/bin/wasm-opt" ]; then
  # The asset is named by the KERNEL arch, not by apt's: aarch64-linux and
  # x86_64-linux. `arm64` is the macOS spelling and 404s here, which is a
  # five-minute detour every time.
  case "$(uname -m)" in
    aarch64|arm64) ba=aarch64 ;;
    x86_64|amd64)  ba=x86_64 ;;
    *) echo "::error::no binaryen $WASM_BINARYEN_VERSION release for $(uname -m)" >&2; exit 1 ;;
  esac
  url="https://github.com/WebAssembly/binaryen/releases/download/version_$WASM_BINARYEN_VERSION/binaryen-version_$WASM_BINARYEN_VERSION-$ba-linux.tar.gz"
  SUDO="$(command -v sudo || true)"
  wget -qO /tmp/binaryen.tar.gz "$url"
  $SUDO tar -xzf /tmp/binaryen.tar.gz -C /opt
  rm -f /tmp/binaryen.tar.gz
fi

export PATH="$BINARYEN/bin:$LLVM/bin:$PATH"
export WASM_CC="$LLVM/bin/clang"
export LLVM_PREFIX="$LLVM"
export CC="$LLVM/bin/clang"

# For the REST of the job, not just this step: a GitHub step is its own shell.
if [ -n "${GITHUB_ENV:-}" ]; then
  {
    echo "WASM_CC=$LLVM/bin/clang"
    echo "LLVM_PREFIX=$LLVM"
  } >> "$GITHUB_ENV"
fi
if [ -n "${GITHUB_PATH:-}" ]; then
  { echo "$BINARYEN/bin"; echo "$LLVM/bin"; } >> "$GITHUB_PATH"
fi

echo "--- the pinned wasm toolchain ---"
"$LLVM/bin/clang" --version | head -1
"$LLVM/bin/wasm-ld" --version
"$BINARYEN/bin/wasm-opt" --version
