# The native (dedicated-process) Foolish server. Links the C KERNEL directly —
# same c/src/*.c the wasm and iOS builds use — so the game logic is shared and
# this backend only adds a socket + an in-memory store. See README.md.

KDIR   := ../../../c
# A bare `make` builds the server (README.md), not the first rule below.
.DEFAULT_GOAL := foolish_server
UNAME  := $(shell uname)
CFLAGS ?= -O2 -ffast-math -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
          -I$(KDIR)/src -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK

# PLATFORM: the server itself is LINUX-ONLY since Stage 6 replaced
# thread-per-connection with an epoll event loop per game-worker shard
# (SERVER_SCALING.md "Stage 6") - `sys/epoll.h` has no macOS equivalent and no
# kqueue port was attempted. Rather than let that surface as a mystery
# "'sys/epoll.h' file not found" three screens into a kernel build, the
# Linux-only targets depend on `require-linux`, which says so and points at the
# container one-liner in README.md. The kernel-only targets (sem_fuzz,
# sem_fuzz_asan) and fuzz_client carry no epoll and still build on macOS.
require-linux:
	@if [ "$(UNAME)" != "Linux" ]; then \
	  echo "server/impls/native: this target needs Linux (epoll, Stage 6 - see SERVER_SCALING.md)."; \
	  echo "  Build and run it in a container instead - see README.md 'Build & run':"; \
	  echo "    docker run --rm -it -v \"$$(cd ../../.. && pwd)\":/repo ubuntu:24.04 bash"; \
	  echo "  On macOS only the kernel-linking targets build natively: make sem_fuzz"; \
	  exit 1; \
	fi
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
SHARED := $(KDIR)/../shared/c
# …AND THE SHARED HALF. This used to be one wildcard over $(KDIR)/src, which is
# why it broke silently when sha256.c and deal_rng.c moved to shared/: a wildcard
# does not fail when a file leaves it, it just compiles less. Both directories are
# wildcarded so a NEW file in either is picked up the way one in src/ always was.
KERNEL_SRC := $(filter-out $(wildcard $(KDIR)/src/main_*.c), $(wildcard $(KDIR)/src/*.c)) \
              $(wildcard $(SHARED)/*.c)

# Native (non-kernel) helpers shared by the server and its clients: the
# RFC 6455 WebSocket handshake (SHA-1 + base64) and frame I/O (ws.c/ws.h),
# the plaintext-or-TLS Conn I/O abstraction (conn.c/conn.h - Stage 3,
# TLS.md) both of them now read/write through, and the packed control-plane
# wire (ctl_wire.c/ctl_wire.h) the server and every client that talks to it
# encode and decode with - ONE codec, so nothing scrapes anything.
NATIVE_SRC := ws.c conn.c ctl_wire.c

# The server itself, split by what each part is ABOUT (see foolish_server.c's
# header comment for the map). foolish_server.c is the entry point: the CLI,
# the bring-up order, and the accept loop it hands off to.
SERVER_SRC := foolish_server.c registry.c session.c lobby.c play.c bots.c \
              snapshot.c metrics.c httpd.c wsconn.c shard.c push.c srvthread.c

# SQLite write-behind persistence (persist.c/persist.h — Stage 2,
# DURABILITY.md). Only foolish_server / foolish_server_prof link this.
PERSIST_SRC := persist.c

foolish_server: $(SERVER_SRC) $(NATIVE_SRC) $(PERSIST_SRC) $(KERNEL_SRC) | require-linux
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(SQLITE_LDFLAGS) $(TLS_LDFLAGS)

run: foolish_server
	./foolish_server 8099

# foolish_hammer: the load-test client (server/impls/native/foolish_hammer.c,
# see its header comment). Links the SAME kernel sources as foolish_server —
# it needs awire_encode + view.h/state_get to build/decode real wire frames —
# but never includes foolish_server.c itself: it is a separate process that
# hits the server over the loopback socket, exactly like a real client.
foolish_hammer: foolish_hammer.c $(NATIVE_SRC) $(KERNEL_SRC) | require-linux
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(TLS_LDFLAGS)

# Profiling build of the server: same sources, but -g -fno-omit-frame-pointer
# and -O2 (not -O3/-flto) so callgrind/gprof attribute cost to real functions
# instead of folding everything into inlined blobs. See profile.sh + T1 in
# PROFILE_HOTPATH.md.
PROF_CFLAGS := -g -fno-omit-frame-pointer -O2 -ffast-math -Wall -Wextra \
               -Wno-unused-parameter -Wno-deprecated-declarations \
               -I$(KDIR)/src -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK
foolish_server_prof: $(SERVER_SRC) $(NATIVE_SRC) $(PERSIST_SRC) $(KERNEL_SRC) | require-linux
	$(CC) $(PROF_CFLAGS) $^ -o $@ $(LDFLAGS) $(SQLITE_LDFLAGS) $(TLS_LDFLAGS)

# AddressSanitizer + UBSan build, for fuzzing the server (fuzz_client.c): catches
# heap/stack overflows, use-after-free, and undefined behavior even when they
# don't crash outright. -O1 (not -O2/-ffast-math) so reports point at real
# lines. See fuzz_client.c and fuzz_test.sh.
ASAN_CFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
               -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
               -I$(KDIR)/src -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK
foolish_server_asan: $(SERVER_SRC) $(NATIVE_SRC) $(PERSIST_SRC) $(KERNEL_SRC) | require-linux
	$(CC) $(ASAN_CFLAGS) $^ -o $@ $(LDFLAGS) $(SQLITE_LDFLAGS) $(TLS_LDFLAGS)

# ws_frame_test: the RFC 6455 frame header against a hostile peer (ws.c's
# 8-byte length form). Built under UBSan with -fno-sanitize-recover ON PURPOSE:
# the defect it pins is undefined behaviour that a debug build happens not to
# miscompile, so a return-value assertion alone would go green against it - see
# the file's header comment. Kernel-free and socket-free, so it builds and runs
# on macOS too (no epoll, and -DFOOLISH_NO_OPENSSL keeps conn.c off OpenSSL).
WS_TEST_CFLAGS := -fsanitize=undefined -fno-sanitize-recover=undefined \
                  -fno-omit-frame-pointer -g -O1 \
                  -Wall -Wextra -Wno-unused-parameter -DFOOLISH_NO_OPENSSL
ws_frame_test: ws_frame_test.c ws.c conn.c
	$(CC) $(WS_TEST_CFLAGS) $^ -o $@
.PHONY: ws-frame-test
ws-frame-test: ws_frame_test
	./ws_frame_test

# ctl_wire_test: the packed control-plane wire (ctl_wire.c) against itself and
# against hostile bytes - round trip in one direction, refusal in the other.
# Built under UBSan so a length that lies is caught as undefined behaviour and
# not just as a wrong answer. Kernel-free and socket-free, so it builds and
# runs on macOS too (no epoll).
CTL_TEST_CFLAGS := -fsanitize=undefined -fno-sanitize-recover=undefined \
                   -fno-omit-frame-pointer -g -O1 -Wall -Wextra -Wno-unused-parameter
ctl_wire_test: ctl_wire_test.c ctl_wire.c
	$(CC) $(CTL_TEST_CFLAGS) $^ -o $@
.PHONY: ctl-wire-test
ctl-wire-test: ctl_wire_test
	./ctl_wire_test

# The adversarial fuzz client: hammers the server with malformed HTTP, junk
# signups, forged tokens, meta abuse, binary action garbage, seat attacks, and
# malformed WebSocket frames — trying to crash, corrupt, or bypass it.
fuzz_client: fuzz_client.c ctl_wire.c
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

# sem_fuzz: the SEMANTIC anti-cheat fuzzer. Links the kernel DIRECTLY (no server,
# no socket — like foolish_hammer but purely in-process) and fuzzes awire_apply,
# the legality engine, with full ground truth: it deals real games, plays them
# with bot_drive, and fires well-formed ILLEGAL moves between turns, asserting no
# card-not-in-hand is ever accepted, card conservation holds, rejects never
# mutate state, and every enumerated legal move is accepted. See sem_fuzz.c.
sem_fuzz: sem_fuzz.c $(KERNEL_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# ASan/UBSan build of the semantic fuzzer (memory-safety of the legality engine
# under hostile-but-well-formed moves, on top of the logical invariants).
sem_fuzz_asan: sem_fuzz.c $(KERNEL_SRC)
	$(CC) $(ASAN_CFLAGS) $^ -o $@ $(LDFLAGS)

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
# -ldl, and -lstdc++ after libquiche.a: the BoringSSL quiche bundles is C++ now
# (its ssl/ objects reference the C++ runtime), and the link failed without it.
QUICHE_DIR ?= ./quiche
QUIC_SRC   := quic_wt.c game_bridge.c
foolish_server_quic: $(SERVER_SRC) $(QUIC_SRC) $(NATIVE_SRC) $(PERSIST_SRC) $(KERNEL_SRC) | require-linux
	$(CC) $(CFLAGS) -DFOOLISH_QUIC -DFOOLISH_NO_OPENSSL -I$(QUICHE_DIR)/include \
	  $^ -o $@ $(LDFLAGS) $(SQLITE_LDFLAGS) $(QUICHE_DIR)/lib/libquiche.a -ldl -lstdc++

# wt_client: the WebTransport smoke-test client (quic_test.sh drives it). Pure
# quiche client — no kernel, no server sources. Same QUICHE_DIR as above.
wt_client: wt_client.c
	$(CC) $(CFLAGS) -I$(QUICHE_DIR)/include $^ -o $@ -lpthread -ldl -lm $(QUICHE_DIR)/lib/libquiche.a -lstdc++

clean:
	rm -f foolish_server foolish_hammer foolish_server_prof foolish_server_quic wt_client \
	      foolish_server_asan fuzz_client ws_frame_test ctl_wire_test sem_fuzz sem_fuzz_asan

.PHONY: run clean require-linux
