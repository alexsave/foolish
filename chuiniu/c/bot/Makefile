# Chui Niu - the offline bot and its arena. Never shipped: nothing here is
# in the iOS bridge or the kernel's own Makefile. See ../../docs/BOT.md.
#
#   make -C chuiniu/c/bot bot-test   the bot's tests, built and run
#   make -C chuiniu/c/bot asan       the same under ASan + UBSan
#   make -C chuiniu/c/bot arena      the headline runs: bot vs random at 2 and 4 seats
#   make -C chuiniu/c/bot arena-fast a smoke run of the same
#   make -C chuiniu/c/bot clean
#
# The kernel's sources are compiled in by relative path, so this Makefile
# never touches ../Makefile's targets or build directory.
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -std=c11
SHARED  := ../../../shared/c
KSRC    := ../src/cn.c ../src/cn_dice.c ../src/cn_plan.c $(SHARED)/deal_rng.c $(SHARED)/sha256.c
KHDR    := $(wildcard ../src/*.h) $(SHARED)/deal_rng.h $(SHARED)/sha256.h \
           ../tests/cn_check.h $(SHARED)/test/check.h
BSRC    := cn_bot.c cn_bot_random.c
BHDR    := cn_bot.h
LIBS    := -lm -lpthread

ARENA_GAMES ?= 4000
ARENA_SEED  ?= 1

build:
	@mkdir -p build

build/cn_bot_test: cn_bot_test.c $(BSRC) $(BHDR) $(KSRC) $(KHDR) | build
	$(CC) $(CFLAGS) $(KSRC) $(BSRC) cn_bot_test.c -o $@ $(LIBS)

build/cn_arena: cn_arena.c $(BSRC) $(BHDR) $(KSRC) $(KHDR) | build
	$(CC) $(CFLAGS) $(KSRC) $(BSRC) cn_arena.c -o $@ $(LIBS)

ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
              -Wall -Wextra -Werror -std=c11

build/asan_cn_bot_test: cn_bot_test.c $(BSRC) $(BHDR) $(KSRC) $(KHDR) | build
	$(CC) $(ASAN_FLAGS) $(KSRC) $(BSRC) cn_bot_test.c -o $@ $(LIBS)

bot-test: build/cn_bot_test
	./build/cn_bot_test

asan: build/asan_cn_bot_test
	./build/asan_cn_bot_test

arena: build/cn_arena
	./build/cn_arena --seats=2 --mix=bot,random --games=$(ARENA_GAMES) --seed=$(ARENA_SEED)
	./build/cn_arena --seats=4 --mix=bot,random,random,random --games=$(ARENA_GAMES) --seed=$(ARENA_SEED)

arena-fast: build/cn_arena
	./build/cn_arena --fast --seats=2 --mix=bot,random
	./build/cn_arena --fast --seats=4 --mix=bot,random,random,random

clean:
	rm -rf build

.PHONY: bot-test asan arena arena-fast clean
