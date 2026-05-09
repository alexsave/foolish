CC      ?= clang
CFLAGS  ?= -O3 -ffast-math -Wall -Wextra -Wno-unused-parameter -Isrc
LDFLAGS ?= -lm

CORE_SRC := src/game.c src/legal.c src/random_strategy.c src/espresso_strategy.c \
            src/tokenize.c src/nn.c src/nitro_strategy.c
CORE_OBJ := $(CORE_SRC:src/%.c=build/%.o)

.PHONY: all clean tests
all: build/cnitro_collect build/cnitro_train build/cnitro_eval build/cnitro_tests build/cnitro_cross_check

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/cnitro_collect: $(CORE_OBJ) build/main_collect.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_train: $(CORE_OBJ) build/main_train.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_eval: $(CORE_OBJ) build/main_eval.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_tests: $(CORE_OBJ) build/tests.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_cross_check: $(CORE_OBJ) build/cross_check.o
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
