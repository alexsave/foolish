#!/usr/bin/env bash
# CI: install the LLVM this repo's C tooling needs, and export it for the job.
#
# Two things in this repo are libclang programs or wasm32 links, and both are
# part of an ordinary build rather than something a human remembers to run:
#
#   tools/structgen        reads C struct layouts through libclang and writes
#                          the modules sdk/, src/, server/ and e2e/ import
#                          (tools/structgen/gen.sh)
#   c/Makefile wasm-*      compiles the kernel for wasm32
#                          (scripts/wasm_build.sh)
#
# So a lane that type-checks, tests, builds the web or deploys the functions
# needs this, not only a lane that builds a module.
#
# ONE TOOLCHAIN, AND IT IS PINNED - clang 22 from apt.llvm.org plus binaryen 130
# from its GitHub release. It used to be Ubuntu's own clang 18 with apt's
# binaryen, and raising it is the price of the wasm modules becoming build
# outputs instead of committed files.
#
# WHY THE PIN MATTERS NOW, when it did not before. While the three .wasm.gz were
# committed, whatever clang this script installed only ever built the throwaway
# TEST module, so its version was free. Now a lane builds the bytes a visitor
# DOWNLOADS, and the version is a kilobyte and a half. Measured on one tree,
# c/build/bots.wasm and sdk/ts/wasm/bots.wasm.gz:
#
#   clang 22.1.8 + binaryen 130   191,485 B raw   the pin
#   clang 22.1.8 + binaryen 108   191,729 B raw   +244 B
#   clang 18.1.3 + binaryen 108   194,997 B raw   +3,512 B, ~+1,706 B gzipped
#
# AND THE PIN IS WHAT MAKES THE MODULE REPRODUCIBLE, which is the property the
# committed-artifact arrangement assumed was unobtainable. With clang 22.1.8 and
# binaryen 130, c/build/bots.wasm comes out byte-identical on macOS arm64
# (Homebrew), Linux arm64 and Linux x86_64 (apt.llvm.org, and x86_64 is what
# ubuntu-latest runs) - all md5 ac53b4dc5484aabad381d2d7bc451088, 191,485 B. The
# same holds for oracle.wasm and oracle-mt.wasm. scripts/wasm_build.sh --check
# is the gate that keeps it true.
#
# WHY ONE SCRIPT AND NOT TWO. The obvious shape was to leave this at clang 18
# for libclang and add a second script pinning 22 for the wasm build. Both
# export WASM_CC - tools/structgen/gen.sh reads it as "the libclang to generate
# with" and c/Makefile reads it as "the compiler to target wasm32 with" - so two
# scripts writing one variable means the LAST step to run decides which clang
# built the shipped module, silently, for 1.7 KB of download. That is the exact
# class of drift this whole change is removing, so there is one script.
#
# Nothing is lost by generating with 22 instead of 18: the generated TEXT is
# measured byte-identical across Homebrew clang 22.1.8, apt.llvm.org clang
# 22.1.8 and Ubuntu's clang 18.1.3, and the layout hash is independent of the
# libclang version by construction (shared/tools/structgen/test/hash.sh).
# .github/workflows/wasm.yml has pinned 22 from apt.llvm.org for the structgen
# job since before this, for the same reason.
#
# apt.llvm.org IS reachable from this project's environments - verified from the
# owner's Mac (HTTP 200) and from inside clean ubuntu:24.04 containers on both
# arm64 and x86_64, where llvm.sh installs 22 and the modules build.
#
# Explicit LLVM paths, not `clang`: the swift:6.2.4-noble image already has
# Swift's own clang at /usr/bin/clang, and plain ubuntu-latest has gcc as cc.
#
# Idempotent: safe to run again in a job that has already run it, and safe to
# run after scripts/ci_bots_test_wasm.sh.
set -euo pipefail

LLVM_VERSION="${LLVM_VERSION:-22}"
BINARYEN_VERSION="${BINARYEN_VERSION:-130}"
LLVM="/usr/lib/llvm-$LLVM_VERSION"
BINARYEN="/opt/binaryen-version_$BINARYEN_VERSION"

if [ ! -f "$LLVM/include/clang-c/Index.h" ] || [ ! -x "$LLVM/bin/wasm-ld" ]; then
  SUDO="$(command -v sudo || true)"
  $SUDO apt-get update -qq
  # gcc, for `cc`: shared/tools/structgen/Makefile builds the generator with
  # $(CC), which defaults to cc. ubuntu-latest has it; the swift:6.2.4-noble
  # image this also runs in has Swift's clang at /usr/bin/clang and no cc at
  # all, and a container that is merely `make`-equipped has neither. Name it
  # rather than inherit it. wget/gnupg/lsb-release are what llvm.sh itself
  # needs.
  $SUDO apt-get install -y --no-install-recommends \
      make gcc gzip wget gnupg lsb-release software-properties-common ca-certificates
  $SUDO wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
  $SUDO bash /tmp/llvm.sh "$LLVM_VERSION"
  $SUDO apt-get install -y --no-install-recommends \
      "clang-$LLVM_VERSION" "lld-$LLVM_VERSION" "libclang-$LLVM_VERSION-dev"
fi

# binaryen from the GitHub release, not apt: Ubuntu 24.04 ships 108 and the pin
# is 130, and wasm-opt's version is worth 244 raw bytes of shipped module.
if [ ! -x "$BINARYEN/bin/wasm-opt" ]; then
  # The asset is named by the KERNEL arch, not apt's: aarch64-linux and
  # x86_64-linux. `arm64` is the macOS spelling and 404s here.
  case "$(uname -m)" in
    aarch64|arm64) ba=aarch64 ;;
    x86_64|amd64)  ba=x86_64 ;;
    *) echo "::error::no binaryen $BINARYEN_VERSION release for $(uname -m)" >&2; exit 1 ;;
  esac
  SUDO="$(command -v sudo || true)"
  wget -qO /tmp/binaryen.tar.gz \
    "https://github.com/WebAssembly/binaryen/releases/download/version_$BINARYEN_VERSION/binaryen-version_$BINARYEN_VERSION-$ba-linux.tar.gz"
  $SUDO tar -xzf /tmp/binaryen.tar.gz -C /opt
  rm -f /tmp/binaryen.tar.gz
fi

# binaryen FIRST, so its wasm-opt beats any the distro left on PATH.
export PATH="$BINARYEN/bin:$LLVM/bin:$PATH"
export WASM_CC="$LLVM/bin/clang"
export LLVM_PREFIX="$LLVM"
# For the REST of the job, not just this step: a GitHub step is its own shell.
if [ -n "${GITHUB_ENV:-}" ]; then
  echo "WASM_CC=$LLVM/bin/clang" >> "$GITHUB_ENV"
  echo "LLVM_PREFIX=$LLVM" >> "$GITHUB_ENV"
fi
if [ -n "${GITHUB_PATH:-}" ]; then
  { echo "$BINARYEN/bin"; echo "$LLVM/bin"; } >> "$GITHUB_PATH"
fi
"$LLVM/bin/clang" --version | head -1
"$LLVM/bin/wasm-ld" --version
"$BINARYEN/bin/wasm-opt" --version
