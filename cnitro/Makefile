CC      ?= clang
CFLAGS  ?= -O3 -ffast-math -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Isrc -DACCELERATE_NEW_LAPACK
LDFLAGS ?= -lm -framework Accelerate

# The engine + all heuristic / Monte-Carlo bots. Linked into every binary.
CORE_SRC := src/game.c src/legal.c src/random_strategy.c src/espresso_strategy.c \
            src/handwritten_strategy.c src/robusta_strategy.c \
            src/firecracker_strategy.c src/gunpowder_strategy.c \
            src/blackpowder_strategy.c src/cordite_strategy.c
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

clean:
	rm -rf build
