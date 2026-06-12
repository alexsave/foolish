CC      ?= clang
# -flto links the engine, legal-move generator and bots as one unit so the
# tiny per-ply leaf calls (can_cover, game_done, should_bot_act, card_eq,
# hand_remove_card — 100M–300M calls in a cordite eval) inline into the rollout
# hot loops instead of being cross-translation-unit calls. ~1.5x faster cordite
# eval, bit-identical play (verified: same mean_finish / win_rate / histogram,
# CD_DIFFTEST clean). The win grows with player count.
CFLAGS  ?= -O3 -ffast-math -flto -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Isrc -DACCELERATE_NEW_LAPACK
LDFLAGS ?= -lm -flto -framework Accelerate

# Parallel eval: `make OMP=1 ...` fans the independent games of cnitro_eval
# across cores (the engine RNG and all cordite scratch state are _Thread_local,
# so games don't race; results are identical to serial). Off by default so a
# plain `make` needs no libomp. On macOS/clang you'll also want libomp
# (brew install libomp) — gcc has it built in.
ifeq ($(OMP),1)
  CFLAGS  += -fopenmp
  LDFLAGS += -fopenmp
endif

# The engine + all heuristic / Monte-Carlo bots. Linked into every binary.
CORE_SRC := src/game.c src/legal.c src/random_strategy.c src/espresso_strategy.c \
            src/handwritten_strategy.c src/robusta_strategy.c \
            src/firecracker_strategy.c src/gunpowder_strategy.c \
            src/blackpowder_strategy.c src/cordite_strategy.c \
            src/cordite_sim.c src/astrolite_strategy.c
CORE_OBJ := $(CORE_SRC:src/%.c=build/%.o)

.PHONY: all clean tests
all: build/cnitro_eval build/cnitro_elo build/cnitro_replay build/cnitro_showcase build/cnitro_tests

# Generic compile rule for sources under src/ (build/%.o: src/%.c).
build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/cnitro_eval: $(CORE_OBJ) build/main_eval.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_elo: $(CORE_OBJ) build/main_elo.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_replay: $(CORE_OBJ) build/main_replay.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_showcase: $(CORE_OBJ) build/main_showcase.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_tests: $(CORE_OBJ) build/tests.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

build/tests.o: tests/tests.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

tests: build/cnitro_tests
	./build/cnitro_tests

# Bitboard-rollout difftests (not part of the default suite). Each plays many
# games and asserts the bitboard engine matches the struct engine.
build/sim_difftest: tests/sim_difftest.c $(CORE_OBJ)
	$(CC) $(CFLAGS) tests/sim_difftest.c $(CORE_OBJ) -o $@ $(LDFLAGS)
build/apply_difftest: tests/apply_difftest.c $(CORE_OBJ)
	$(CC) $(CFLAGS) tests/apply_difftest.c $(CORE_OBJ) -o $@ $(LDFLAGS)
build/solver_difftest: tests/solver_difftest.c $(CORE_OBJ)
	$(CC) $(CFLAGS) tests/solver_difftest.c $(CORE_OBJ) -o $@ $(LDFLAGS)
difftests: build/sim_difftest build/apply_difftest build/solver_difftest
	./build/sim_difftest 4 1000 && ./build/apply_difftest 4 200 && \
	./build/solver_difftest 2 40 "" 200000 && \
	./build/solver_difftest 3 40 "" 200000 && \
	./build/solver_difftest 4 40 "" 200000

clean:
	rm -rf build
