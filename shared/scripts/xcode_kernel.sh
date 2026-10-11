#!/bin/bash
# Xcode builds the C kernel itself. Each product's ios/project.yml runs this
# twice over: as the script of its "<Name>Kernel" aggregate target, which the
# framework that links the kernel depends on, so every build starts with it;
# and as xcodegen's preGenCommand, so the library exists before there is a
# project to build.
#
#   shared/scripts/xcode_kernel.sh <the product's c/ directory> [catalyst target] [generator]
#
# It runs `make ios-lib` there. That target does nothing when nothing changed
# (shared/tools/ios_xcframework.mk), so it costs a build under a second, and
# when a .c or a header did change the library is rebuilt and Xcode relinks.
#
# WHAT THIS REPLACES. The xcframework was written by a command a person ran
# (`make -C c ios-lib`), and nothing made them: an app built after an edit to
# the kernel linked the kernel from before it. One product had a build phase
# that compared modification times and refused the build with "run make
# ios-lib"; the others had the comment at the top of project.yml. A build that
# can tell the library is stale can also rebuild it.
#
# A CLEAN ENVIRONMENT, and it is not tidiness. Xcode exports its build settings
# to a script, and several of them are names make and clang read: SDKROOT is
# the iOS SDK, so the host tools this make also builds (structgen, datagen, a
# texture generator) would be compiled against an iPhone's headers; CC, CFLAGS
# and LDFLAGS would silently replace a Makefile's `?=` defaults. So make gets
# HOME, a PATH and the Xcode that is doing the building, and nothing else.
#
# MAC CATALYST: a product whose tests run there keeps a third slice behind its
# own target (the optional second argument, "" for none). Xcode says which
# build this is.
#
# A GENERATOR OF ITS OWN: a product whose generated Swift has a second writer
# besides `make ios-lib` names that script as the third argument, and it runs
# first, in the same clean environment. It has to be one that writes a file
# only when the file's content changed, or every build would recompile
# everything that imports what it writes.
set -euo pipefail

dir=${1:?usage: xcode_kernel.sh <c directory> [catalyst target] [generator]}
target=ios-lib
if [ "${IS_MACCATALYST:-NO}" = "YES" ] && [ -n "${2:-}" ]; then target=$2; fi
generator=${3:-}

# Homebrew first: the generators need its llvm (shared/tools/llvm.mk), and a
# script phase's own PATH does not have it.
path="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

clean() { env -i HOME="$HOME" PATH="$path" ${DEVELOPER_DIR:+DEVELOPER_DIR="$DEVELOPER_DIR"} "$@"; }
[ -z "$generator" ] || clean bash "$generator"
clean make -C "$dir" "$target"
