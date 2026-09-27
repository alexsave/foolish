# Pick 'Em Up - the kernel and its tests. `make run` is the real test target.
#
#   make -C pickemup/c run     every test, built and run
#   make -C pickemup/c asan    the same under ASan + UBSan
#   make -C pickemup/c wasm    prove the kernel builds for wasm32, freestanding
#   make -C pickemup/c clean
#
# The wire (pk_code, pk_msg), the iOS bridge (ios/pk_api.c, ios-lib,
# ios-smoke) and the CI lane are the envelope's and land beside these.
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -std=c11
SHARED  := ../../shared/c
SRC     := src/pk.c src/pk_deck.c src/pk_plan.c src/pk_view.c src/pk_lobby.c src/pk_say.c \
           src/pk_code.c src/pk_msg.c i18n/strings_en.c \
           $(SHARED)/deal_rng.c $(SHARED)/sha256.c $(SHARED)/b32.c $(SHARED)/mixrad.c
HDR     := $(wildcard src/*.h) i18n/keys.h tests/pk_check.h \
           $(SHARED)/deal_rng.h $(SHARED)/sha256.h $(SHARED)/b32.h $(SHARED)/mixrad.h

# One binary per test file; each takes an optional scale argument.
TESTS   := pk_test pk_plan_test pk_say_test pk_fuzz pk_msg_test
FUZZ_GAMES ?= 2800
WIRE_GAMES ?= 30

all: $(addprefix build/,$(TESTS))

build:
	@mkdir -p build

build/%: tests/%.c $(SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(SRC) $< -o $@

run: $(addprefix build/,$(TESTS))
	./build/pk_test
	./build/pk_plan_test
	./build/pk_say_test
	./build/pk_fuzz $(FUZZ_GAMES)
	./build/pk_msg_test $(WIRE_GAMES)

ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
              -Wall -Wextra -Werror -std=c11

build/asan_%: tests/%.c $(SRC) $(HDR) | build
	$(CC) $(ASAN_FLAGS) $(SRC) $< -o $@

asan: $(addprefix build/asan_,$(TESTS))
	./build/asan_pk_test
	./build/asan_pk_plan_test
	./build/asan_pk_say_test
	./build/asan_pk_fuzz 2000
	./build/asan_pk_msg_test 5 100

# THE SAME FILES FOR wasm32, freestanding (-nostdlib, shared/c/wasm's libc):
# the proof that nothing here reaches past memcpy / memset / strlen. Objects
# only; the replay page's module is linked by whoever builds that page.
WASM_CC ?= clang
WASM_SRC := $(SRC)
wasm: | build
	@mkdir -p build/wasm
	@for s in $(WASM_SRC); do \
	  o=build/wasm/$$(basename $$s .c).o; \
	  $(WASM_CC) --target=wasm32 -Oz -nostdlib -ffreestanding -std=c11 -Wall -Wextra -Werror \
	    -isystem $(SHARED)/wasm/include -c $$s -o $$o || exit 1; \
	done
	@echo "wasm32 objects in build/wasm/"

clean:
	rm -rf build

.PHONY: all run asan wasm clean
.SECONDARY:
