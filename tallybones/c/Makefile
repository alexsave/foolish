# Tallybones - the kernel, its wire, its iOS bridge and its tests. `make run`
# is the real test target (pickemup/c/Makefile's shape).
#
#   make -C tallybones/c run         every test and the bridge smoke, built and run
#   make -C tallybones/c asan        the same under ASan + UBSan
#   make -C tallybones/c wasm        prove the kernel builds for wasm32, freestanding
#   make -C tallybones/c ios-smoke   every bridge entry point, host compiler, no Mac
#   make -C tallybones/c structgen   the Swift readers  -> ../ios/Generated/ (needs libclang)
#   make -C tallybones/c datagen     the Swift strings  -> ../ios/Generated/i18n/
#   make -C tallybones/c ios-lib     ../ios/vendor/Tallybones.xcframework (needs Xcode)
#   make -C tallybones/c swift-smoke the bridge driven from Swift through the readers (a Mac)
#   make -C tallybones/c clean
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -std=c11
SHARED  := ../../shared/c
SRC     := src/tb.c src/tb_code.c src/tb_plan.c src/tb_lobby.c src/tb_msg.c \
           $(SHARED)/deal_rng.c $(SHARED)/sha256.c $(SHARED)/b32.c $(SHARED)/mixrad.c
HDR     := $(wildcard src/*.h) tests/tb_check.h \
           $(SHARED)/deal_rng.h $(SHARED)/sha256.h $(SHARED)/b32.h $(SHARED)/mixrad.h

TESTS   := tb_test tb_msg_test

all: $(addprefix build/,$(TESTS))

build:
	@mkdir -p build

build/%: tests/%.c $(SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(SRC) $< -o $@

run: $(addprefix build/,$(TESTS))
	./build/tb_test
	./build/tb_msg_test 10

ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
              -Wall -Wextra -Werror -std=c11

build/asan_%: tests/%.c $(SRC) $(HDR) | build
	$(CC) $(ASAN_FLAGS) $(SRC) $< -o $@

asan: $(addprefix build/asan_,$(TESTS))
	./build/asan_tb_test
	./build/asan_tb_msg_test 2

clean:
	rm -rf build

.PHONY: all run asan clean
.SECONDARY:
