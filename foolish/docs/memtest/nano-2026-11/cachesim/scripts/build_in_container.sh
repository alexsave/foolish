#!/bin/bash
# Runs inside the AL2023 aarch64 container. Builds the dynamic glibc cbase serve (CC carries -fno-omit-frame-pointer,
# -O2 -g come from CB_CFLAGS) and the p3load client the way grav/pkg/load.sh build does. Outputs to /out/bin.
set -euo pipefail
mkdir -p /w /out/bin && cp -a /src/. /w/ && cd /w/c_base
make -s -j4 build/cbase CC="gcc -fno-omit-frame-pointer" > /out/out/build_cbase.log 2>&1
cp build/cbase /out/bin/cbase_dyn
CAPS=$(make -s --no-print-directory -C ../foolish/c print-human-caps)
F="-std=gnu11 -D_GNU_SOURCE -O2 -g -Icore/include -Icore/src -Iproviders/local/src -Icore/test -I../foolish/c/src -I../shared/c -pthread $CAPS"
libs=$(find build/obj/cbase -name "*.o" ! -path "*/test/*" ! -name "main_*.o" ! -path "*/tools/*" ! -path "*/bench/*" ! -path "*/migrate/*")
kern=$(find build/obj/kernel -name "*.o")
cc $F -c bench/nano/lib_client_host.c -o /tmp/lch.o
cc $F -c bench/nano/p3load3.c -o /tmp/p3load.o
cc /tmp/p3load.o /tmp/lch.o $libs $kern -o /out/bin/p3load -lsqlite3 -lssl -lcrypto -lpthread -lm
ls -l /out/bin; file /out/bin/cbase_dyn | cut -c1-200; ldd /out/bin/cbase_dyn
echo BUILD_OK
