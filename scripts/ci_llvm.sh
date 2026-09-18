#!/usr/bin/env bash
# CI: install the LLVM this repo's C tooling needs, and export it for the job.
#
# Two things in this repo are libclang programs or wasm32 links, and both are
# now part of an ordinary build rather than something a human remembers to run:
#
#   tools/structgen        reads C struct layouts through libclang and writes
#                          the modules sdk/, src/, server/ and e2e/ import
#                          (tools/structgen/gen.sh)
#   c/Makefile wasm-*      compiles the kernel for wasm32
#
# So a lane that type-checks, tests, builds the web or deploys the functions
# needs this, not only a lane that builds a module. It used to be installed
# inline by scripts/ci_bots_test_wasm.sh alone, which meant a lane that needed
# the compiler but not the test module had to build the test module to get it.
#
# Explicit LLVM 18 paths, not `clang`: the swift:6.2.4-noble image already has
# Swift's own clang at /usr/bin/clang, and plain ubuntu-latest has gcc as cc.
# Neither the layout hash (tools/structgen/test/hash.sh) nor the generated TEXT
# depends on the libclang version - measured across Homebrew clang 22.1.8 on
# macOS, apt.llvm.org clang 22.1.8 on Linux and Ubuntu's own clang 18.1.3, all
# byte-identical - so the distro LLVM agrees with the committed modules' hash
# and with what a Mac generates.
#
# Idempotent: safe to run again in a job that has already run it, and safe to
# run after scripts/ci_bots_test_wasm.sh.
set -euo pipefail
LLVM=/usr/lib/llvm-18
if [ ! -f "$LLVM/include/clang-c/Index.h" ]; then
  SUDO="$(command -v sudo || true)"
  $SUDO apt-get update -qq
  # gcc, for `cc`: tools/structgen/Makefile builds the generator with $(CC),
  # which defaults to cc. ubuntu-latest has it; the swift:6.2.4-noble image
  # this also runs in has Swift's clang at /usr/bin/clang and no cc at all, and
  # a container that is merely `make`-equipped has neither. Name it rather than
  # inherit it.
  $SUDO apt-get install -y --no-install-recommends clang-18 lld-18 libclang-18-dev binaryen make gcc
fi
export PATH="$LLVM/bin:$PATH"
export WASM_CC="$LLVM/bin/clang"
export LLVM_PREFIX="$LLVM"
# For the REST of the job, not just this step: a GitHub step is its own shell.
if [ -n "${GITHUB_ENV:-}" ]; then
  echo "WASM_CC=$LLVM/bin/clang" >> "$GITHUB_ENV"
  echo "LLVM_PREFIX=$LLVM" >> "$GITHUB_ENV"
fi
if [ -n "${GITHUB_PATH:-}" ]; then echo "$LLVM/bin" >> "$GITHUB_PATH"; fi
"$LLVM/bin/clang" --version | head -1
