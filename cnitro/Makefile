CC      ?= clang
CFLAGS  ?= -O3 -ffast-math -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Isrc -DACCELERATE_NEW_LAPACK
LDFLAGS ?= -lm -framework Accelerate

CORE_SRC := src/game.c src/legal.c src/random_strategy.c src/espresso_strategy.c \
            src/handwritten_strategy.c src/robusta_strategy.c \
            src/firecracker_strategy.c src/gunpowder_strategy.c \
            src/tokenize.c src/nn.c src/nn_thread.c src/nitro_strategy.c
CORE_OBJ := $(CORE_SRC:src/%.c=build/%.o)

GRPO_SRC := src/grpo_encode.c src/grpo_net.c src/grpo_format.c src/grpo_collect.c src/grpo_train.c src/dynamite_strategy.c src/grpo_pool.c src/grpo_rl.c
GRPO_OBJ := $(GRPO_SRC:src/%.c=build/%.o)

.PHONY: all clean tests
all: build/cnitro_collect build/cnitro_train build/cnitro_eval build/cnitro_tests build/cnitro_cross_check build/cnitro_inspect build/cnitro_replay build/cnitro_grpo

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/cnitro_collect: $(CORE_OBJ) build/main_collect.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_train: $(CORE_OBJ) build/main_train.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_eval: $(CORE_OBJ) $(GRPO_OBJ) build/main_eval.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_tests: $(CORE_OBJ) build/tests.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_cross_check: $(CORE_OBJ) build/cross_check.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_inspect: $(CORE_OBJ) build/main_inspect.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_replay: $(CORE_OBJ) build/main_replay.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_grpo: $(CORE_OBJ) $(GRPO_OBJ) build/main_grpo.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

build/main_collect.o: src/main_collect.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
build/main_train.o: src/main_train.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
build/main_eval.o: src/main_eval.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
build/main_inspect.o: src/main_inspect.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
build/main_replay.o: src/main_replay.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
build/main_grpo.o: src/main_grpo.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
build/tests.o: tests/tests.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
build/cross_check.o: tests/cross_check.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

tests: build/cnitro_tests
	./build/cnitro_tests

clean:
	rm -rf build
