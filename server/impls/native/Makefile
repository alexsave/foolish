# The native (dedicated-process) Foolish server. Links the C KERNEL directly —
# same c/src/*.c the wasm and iOS builds use — so the game logic is shared and
# this backend only adds a socket + an in-memory store. See README.md.

KDIR   := ../../../c
CFLAGS ?= -O2 -ffast-math -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
          -I$(KDIR)/src -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK
# -framework Accelerate (some kernel strategies use LAPACK) is macOS-only;
# Linux links straight against libm/libpthread. Mirrors c/Makefile's
# `ifeq ($(shell uname),Darwin)` pattern so a plain `make` works on both.
ifeq ($(shell uname),Darwin)
  LDFLAGS ?= -lm -lpthread -framework Accelerate
else
  LDFLAGS ?= -lm -lpthread
endif
# -lsqlite3 (Stage 2, DURABILITY.md): the write-behind persistence engine
# (persist.c) — present as a system library on both platforms this Makefile
# targets, so no vendoring/amalgamation needed. Its own variable, appended
# only to the targets that actually link persist.c (foolish_server and its
# profiling twin) — foolish_hammer is a pure network client that never
# touches the DB, so it stays free of the dependency.
SQLITE_LDFLAGS ?= -lsqlite3

# -lssl -lcrypto (Stage 3, TLS.md): conn.c wraps OpenSSL for BOTH
# foolish_server (server-side TLS listener) and foolish_hammer (client-side
# --tls) — unlike SQLITE_LDFLAGS, this one applies to every target below,
# since conn.c is now part of NATIVE_SRC.
TLS_LDFLAGS ?= -lssl -lcrypto

# Every kernel source except the standalone tools (their own main()).
KERNEL_SRC := $(filter-out $(wildcard $(KDIR)/src/main_*.c), $(wildcard $(KDIR)/src/*.c))

# Native (non-kernel) helpers shared by the server and the load client: the
# RFC 6455 WebSocket handshake (SHA-1 + base64) and frame I/O (ws.c/ws.h),
# and the plaintext-or-TLS Conn I/O abstraction (conn.c/conn.h — Stage 3,
# TLS.md) both of them now read/write through.
NATIVE_SRC := ws.c conn.c

# SQLite write-behind persistence (persist.c/persist.h — Stage 2,
# DURABILITY.md). Only foolish_server / foolish_server_prof link this.
PERSIST_SRC := persist.c

foolish_server: foolish_server.c $(NATIVE_SRC) $(PERSIST_SRC) $(KERNEL_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(SQLITE_LDFLAGS) $(TLS_LDFLAGS)

run: foolish_server
	./foolish_server 8099

# foolish_hammer: the load-test client (server/impls/native/foolish_hammer.c,
# see its header comment). Links the SAME kernel sources as foolish_server —
# it needs awire_encode + view.h/state_get to build/decode real wire frames —
# but never includes foolish_server.c itself: it is a separate process that
# hits the server over the loopback socket, exactly like a real client.
foolish_hammer: foolish_hammer.c $(NATIVE_SRC) $(KERNEL_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(TLS_LDFLAGS)

# Profiling build of the server: same sources, but -g -fno-omit-frame-pointer
# and -O2 (not -O3/-flto) so callgrind/gprof attribute cost to real functions
# instead of folding everything into inlined blobs. See profile.sh + T1 in
# PROFILE_HOTPATH.md.
PROF_CFLAGS := -g -fno-omit-frame-pointer -O2 -ffast-math -Wall -Wextra \
               -Wno-unused-parameter -Wno-deprecated-declarations \
               -I$(KDIR)/src -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK
foolish_server_prof: foolish_server.c $(NATIVE_SRC) $(PERSIST_SRC) $(KERNEL_SRC)
	$(CC) $(PROF_CFLAGS) $^ -o $@ $(LDFLAGS) $(SQLITE_LDFLAGS) $(TLS_LDFLAGS)

# AddressSanitizer + UBSan build, for fuzzing the server (fuzz_client.c): catches
# heap/stack overflows, use-after-free, and undefined behavior even when they
# don't crash outright. -O1 (not -O2/-ffast-math) so reports point at real
# lines. See fuzz_client.c and fuzz_test.sh.
ASAN_CFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
               -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
               -I$(KDIR)/src -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK
foolish_server_asan: foolish_server.c $(NATIVE_SRC) $(PERSIST_SRC) $(KERNEL_SRC)
	$(CC) $(ASAN_CFLAGS) $^ -o $@ $(LDFLAGS) $(SQLITE_LDFLAGS) $(TLS_LDFLAGS)

# The adversarial fuzz client: hammers the server with malformed HTTP, junk
# signups, forged tokens, meta abuse, binary action garbage, seat attacks, and
# malformed WebSocket frames — trying to crash, corrupt, or bypass it.
fuzz_client: fuzz_client.c
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

# QUIC/HTTP3/WebTransport build (foolish_server_quic). Adds a UDP/QUIC listener
# (quic_wt.c) that speaks HTTP/3 + WebTransport onto the SAME in-memory game as
# the TCP front-end, via game_bridge.h. It links Cloudflare quiche
# (libquiche.a, which bundles its own BoringSSL); to avoid a symbol clash with
# that BoringSSL it does NOT link OpenSSL, so conn.c compiles with
# -DFOOLISH_NO_OPENSSL (plaintext TCP — terminate TLS at the edge — while QUIC
# carries TLS 1.3 itself). Build quiche's C FFI first, then point QUICHE_DIR at
# a directory containing include/quiche.h and lib/libquiche.a:
#   git clone --recursive https://github.com/cloudflare/quiche && \
#     cd quiche && cargo build --release --features ffi
#   make foolish_server_quic QUICHE_DIR=/path/to/quiche-ffi
# (-ldl is quiche's only extra system dep beyond -lm -lpthread from LDFLAGS.)
QUICHE_DIR ?= ./quiche
QUIC_SRC   := quic_wt.c
foolish_server_quic: foolish_server.c $(QUIC_SRC) $(NATIVE_SRC) $(PERSIST_SRC) $(KERNEL_SRC)
	$(CC) $(CFLAGS) -DFOOLISH_QUIC -DFOOLISH_NO_OPENSSL -I$(QUICHE_DIR)/include \
	  $^ -o $@ $(LDFLAGS) $(SQLITE_LDFLAGS) $(QUICHE_DIR)/lib/libquiche.a -ldl

# wt_client: the WebTransport smoke-test client (quic_test.sh drives it). Pure
# quiche client — no kernel, no server sources. Same QUICHE_DIR as above.
wt_client: wt_client.c
	$(CC) $(CFLAGS) -I$(QUICHE_DIR)/include $^ -o $@ -lpthread -ldl -lm $(QUICHE_DIR)/lib/libquiche.a

clean:
	rm -f foolish_server foolish_hammer foolish_server_prof foolish_server_quic wt_client

.PHONY: run clean
