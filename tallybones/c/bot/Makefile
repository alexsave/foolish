# Tallybones - the exact single-player solver (DECISIONS.md T30 onwards).
# An internal simulation and evaluation tool; nothing here ships.
#
#   make -C tallybones/c/bot run      build and run every check (tb_bot_test)
#   make -C tallybones/c/bot asan     the same under ASan + UBSan (a smaller simulation)
#   make -C tallybones/c/bot solve    build/tb_solve, then print the exact EV and play 200000 games
#   make -C tallybones/c/bot clean
#
# The kernel sources are compiled in as the kernel's own Makefile compiles
# them, so the bot scores with the game's one tb_score_of and drives the real
# tb_replay in the kernel test; nothing in ../src or ../tests is edited here.
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -std=c11
LDLIBS  := -lpthread -lm
SHARED  := ../../../shared/c
KERNEL  := ../src/tb.c ../src/tb_code.c ../src/tb_plan.c \
           $(SHARED)/deal_rng.c $(SHARED)/sha256.c $(SHARED)/mixrad.c
KHDR    := $(wildcard ../src/*.h) $(SHARED)/deal_rng.h $(SHARED)/sha256.h $(SHARED)/mixrad.h
BOT     := tb_bot.c
HDR     := tb_bot.h $(KHDR)

all: build/tb_solve build/tb_bot_test

build:
	@mkdir -p build

build/tb_solve: tb_solve.c $(BOT) $(KERNEL) $(HDR) | build
	$(CC) $(CFLAGS) $(BOT) $(KERNEL) $< -o $@ $(LDLIBS)

build/tb_bot_test: tb_bot_test.c $(BOT) $(KERNEL) $(HDR) | build
	$(CC) $(CFLAGS) $(BOT) $(KERNEL) $< -o $@ $(LDLIBS)

run: build/tb_bot_test build/tb_solve
	./build/tb_bot_test 200000

solve: build/tb_solve
	./build/tb_solve play 200000

ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
              -Wall -Wextra -Werror -std=c11

build/asan_tb_bot_test: tb_bot_test.c $(BOT) $(KERNEL) $(HDR) | build
	$(CC) $(ASAN_FLAGS) $(BOT) $(KERNEL) $< -o $@ $(LDLIBS)

asan: build/asan_tb_bot_test
	./build/asan_tb_bot_test 20000

clean:
	rm -rf build

.PHONY: all run solve asan clean
.SECONDARY:
