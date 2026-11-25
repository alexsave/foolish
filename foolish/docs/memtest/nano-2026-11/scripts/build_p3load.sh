#!/bin/bash
# Builds p3load from c_base/bench/nano/p3load3.c + lib_client_host.c, as load.sh build does on the nano,
# but with the Homebrew sqlite and openssl paths of this Mac. Output goes outside the worktree.
set -e
W=/Users/alex/Dev/foolish-bench/c_base
OUT=/Users/alex/Dev/foolish-bench-results/bin
mkdir -p "$OUT" /Users/alex/Dev/foolish-bench-results/build_p3
cd "$W"
CAPS=$(make -s --no-print-directory -C ../foolish/c print-human-caps)
SQ=$(brew --prefix sqlite); SSL=$(brew --prefix openssl@3)
F="-std=gnu11 -D_GNU_SOURCE -O2 -g -Icore/include -Icore/src -Iproviders/local/src -Icore/test -I../foolish/c/src -I../shared/c -I$SQ/include -I$SSL/include -pthread $CAPS"
libs=$(find build/obj/cbase -name "*.o" ! -path "*/test/*" ! -name "main_*.o" ! -path "*/tools/*" ! -path "*/bench/*" ! -path "*/migrate/*")
kern=$(find build/obj/kernel -name "*.o")
T=/Users/alex/Dev/foolish-bench-results/build_p3
cc $F -c bench/nano/lib_client_host.c -o $T/lch.o
cc $F -c bench/nano/p3load3.c -o $T/p3load.o
cc $T/p3load.o $T/lch.o $libs $kern -o $OUT/p3load -L$SQ/lib -L$SSL/lib -lsqlite3 -lssl -lcrypto -lpthread -lm
ls -l $OUT/p3load
