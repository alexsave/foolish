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

# Every kernel source except the standalone tools (their own main()).
KERNEL_SRC := $(filter-out $(wildcard $(KDIR)/src/main_*.c), $(wildcard $(KDIR)/src/*.c))

# Native (non-kernel) helpers shared by the server and the load client: the
# RFC 6455 WebSocket handshake (SHA-1 + base64) and frame I/O. See ws.h.
NATIVE_SRC := ws.c

# SQLite write-behind persistence (persist.c/persist.h — Stage 2,
# DURABILITY.md). Only foolish_server / foolish_server_prof link this.
PERSIST_SRC := persist.c

foolish_server: foolish_server.c $(NATIVE_SRC) $(PERSIST_SRC) $(KERNEL_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(SQLITE_LDFLAGS)

run: foolish_server
	./foolish_server 8099

# foolish_hammer: the load-test client (server/impls/native/foolish_hammer.c,
# see its header comment). Links the SAME kernel sources as foolish_server —
# it needs awire_encode + view.h/state_get to build/decode real wire frames —
# but never includes foolish_server.c itself: it is a separate process that
# hits the server over the loopback socket, exactly like a real client.
foolish_hammer: foolish_hammer.c $(NATIVE_SRC) $(KERNEL_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Profiling build of the server: same sources, but -g -fno-omit-frame-pointer
# and -O2 (not -O3/-flto) so callgrind/gprof attribute cost to real functions
# instead of folding everything into inlined blobs. See profile.sh + T1 in
# PROFILE_HOTPATH.md.
PROF_CFLAGS := -g -fno-omit-frame-pointer -O2 -ffast-math -Wall -Wextra \
               -Wno-unused-parameter -Wno-deprecated-declarations \
               -I$(KDIR)/src -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK
foolish_server_prof: foolish_server.c $(NATIVE_SRC) $(PERSIST_SRC) $(KERNEL_SRC)
	$(CC) $(PROF_CFLAGS) $^ -o $@ $(LDFLAGS) $(SQLITE_LDFLAGS)

clean:
	rm -f foolish_server foolish_hammer foolish_server_prof

.PHONY: run clean
