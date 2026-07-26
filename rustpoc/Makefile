# C-vs-Rust POC benchmarks over the REAL kernel.
#
#   make gen       # build + run the workload generator (states.bin, envelopes.bin)
#   make bench-c   # build + run the three C benchmarks
#   make bench-rs  # build + run the three Rust benchmarks (cargo --release)
#
# The C benches compile the actual kernel sources from ../c/src with the same
# native flags production uses (c/Makefile: -O3 -ffast-math -flto, clang).
CC     ?= clang
CFLAGS ?= -O3 -ffast-math -flto -Wall -Wextra -Wno-unused-parameter -I../c/src

KSRC_LEGAL   = ../c/src/game.c ../c/src/deal_rng.c ../c/src/legal.c
KSRC_PLAYOUT = $(KSRC_LEGAL) ../c/src/cordite_sim.c
# The solver bench runs at the SHIPPED TT configuration (bots.wasm's).
TT_FLAGS     = -DCD_TT_BITS=12 -DCD_TT_2WAY -DCD_TT_PACK8
KSRC_WIRE    = ../c/src/game.c ../c/src/deal_rng.c ../c/src/legal.c \
               ../c/src/replay.c ../c/src/replay_steps.c ../c/src/view.c \
               ../c/src/awire.c ../c/src/evwire.c ../c/src/json_out.c \
               ../c/src/sha256.c ../c/src/msg_wire.c

build:
	mkdir -p build

build/gen_states: gen_states.c $(KSRC_WIRE) | build
	$(CC) $(CFLAGS) $^ -o $@ -lm

build/bench_legal: bench_legal.c $(KSRC_LEGAL) | build
	$(CC) $(CFLAGS) $^ -o $@ -lm

build/bench_playout: bench_playout.c $(KSRC_PLAYOUT) | build
	$(CC) $(CFLAGS) $^ -o $@ -lm

build/bench_wire: bench_wire.c $(KSRC_WIRE) | build
	$(CC) $(CFLAGS) $^ -o $@ -lm

build/bench_solve: bench_solve.c $(KSRC_PLAYOUT) | build
	$(CC) $(CFLAGS) $(TT_FLAGS) $^ -o $@ -lm

gen: build/gen_states
	cd build && ./gen_states states.bin envelopes.bin

bench-c: build/bench_legal build/bench_playout build/bench_wire build/bench_solve
	cd build && ./bench_legal states.bin 40 && ./bench_playout states.bin 40 600 && ./bench_wire envelopes.bin 300 && ./bench_solve states.bin 5

bench-rs:
	cd rs && cargo build --release 2>/dev/null || cargo build --release
	cd build && ../rs/target/release/bench_legal states.bin 40 && \
	  ../rs/target/release/bench_playout states.bin 40 600 && \
	  ../rs/target/release/bench_wire envelopes.bin 300

build-gcc:
	mkdir -p build-gcc

build-gcc/bench_legal: bench_legal.c $(KSRC_LEGAL) | build-gcc
	gcc $(CFLAGS) $^ -o $@ -lm

build-gcc/bench_playout: bench_playout.c $(KSRC_PLAYOUT) | build-gcc
	gcc $(CFLAGS) $^ -o $@ -lm

build-gcc/bench_wire: bench_wire.c $(KSRC_WIRE) | build-gcc
	gcc $(CFLAGS) $^ -o $@ -lm

build-gcc/bench_solve: bench_solve.c $(KSRC_PLAYOUT) | build-gcc
	gcc $(CFLAGS) $(TT_FLAGS) $^ -o $@ -lm

bench-c-gcc: build-gcc/bench_legal build-gcc/bench_playout build-gcc/bench_wire build-gcc/bench_solve
	cd build && ../build-gcc/bench_legal states.bin 40 && ../build-gcc/bench_playout states.bin 40 600 && ../build-gcc/bench_wire envelopes.bin 300 && ../build-gcc/bench_solve states.bin 5

clean:
	rm -rf build build-gcc rs/target

.PHONY: gen bench-c bench-c-gcc bench-rs clean
