# Pick 'Em Up - the kernel, its wire, its iOS bridge and its tests. `make run`
# is the real test target.
#
#   make -C pickemup/c run         every test and the bridge smoke, built and run
#   make -C pickemup/c asan        the same under ASan + UBSan
#   make -C pickemup/c wasm        prove the kernel builds for wasm32, freestanding
#   make -C pickemup/c ios-smoke   every bridge entry point, host compiler, no Mac
#   make -C pickemup/c structgen   the Swift readers  -> ../ios/Generated/ (needs libclang)
#   make -C pickemup/c datagen     the Swift strings  -> ../ios/Generated/i18n/
#   make -C pickemup/c ios-lib     ../ios/vendor/Pickemup.xcframework (needs Xcode)
#   make -C pickemup/c swift-smoke the bridge driven from Swift through the readers (a Mac)
#   make -C pickemup/c arena       the bots against each other, 2,000 games a line-up (docs/BOT_REPORT.md)
#   make -C pickemup/c clean
CC      ?= cc
# THE WARNINGS ARE ONE LIST for every build below (DECISION D58): proven clean
# under Apple clang and under Linux gcc 13 (the CI lane's compiler), whose
# -Wconversion warns on narrowing that clang does not.
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -std=c11
CFLAGS  ?= -O2 $(WARN)
SHARED  := ../../shared/c
SRC     := src/pk.c src/pk_deck.c src/pk_plan.c src/pk_view.c src/pk_lobby.c src/pk_say.c \
           src/pk_code.c src/pk_msg.c src/pk_beats.c src/pk_arrange.c i18n/strings_en.c \
           $(SHARED)/deal_rng.c $(SHARED)/sha256.c $(SHARED)/b32.c $(SHARED)/mixrad.c \
           $(SHARED)/text_util/text_util.c
# THE BOTS ARE NOT IN THE APP (DECISION D63): only the bot test and the arena
# link them, so SRC, the wasm objects and the xcframework never carry them.
BOT_SRC := src/pk_belief.c src/pk_bot.c
HDR     := $(wildcard src/*.h) i18n/keys.h tests/pk_check.h tests/pk_bot.h \
           $(SHARED)/deal_rng.h $(SHARED)/sha256.h $(SHARED)/b32.h $(SHARED)/mixrad.h \
           $(SHARED)/test/check.h $(SHARED)/test/twophone.h $(SHARED)/text_util/text_util.h

# One binary per test file; each takes an optional scale argument.
TESTS   := pk_test pk_rules_test pk_plan_test pk_say_test pk_fuzz pk_msg_test pk_twophone_test pk_beats_test pk_arrange_test pk_bot_test
FUZZ_GAMES ?= 2800
# Games per table size that go through the wire after every bubble. The rules
# doc's 10,000 is `./build/pk_msg_test 10000` by hand (DECISION D43).
WIRE_GAMES ?= 30

all: $(addprefix build/,$(TESTS))

# pk_twophone_test compiles the bridge in (tests/pk_twophone_test.c).
build/pk_twophone_test build/asan_pk_twophone_test build/pk_arrange_test build/asan_pk_arrange_test: ios/pk_api.c ios/include/pk_api.h ios/pk_api_layout.h

build:
	@mkdir -p build

build/%: tests/%.c $(SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(SRC) $< -o $@

build/pk_bot_test: tests/pk_bot_test.c $(SRC) $(BOT_SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(SRC) $(BOT_SRC) $< -o $@

# The shared harness's own test (shared/c/test): TEST, CHECK, report, STEP, OK.
CHECK_TEST := $(SHARED)/test/check_test.c $(SHARED)/test/check.h $(SHARED)/test/twophone.h
build/check_test: $(CHECK_TEST) | build
	$(CC) $(CFLAGS) $(SHARED)/test/check_test.c -o $@

# The shared text writers' own test (shared/c/text_util): UTF-8, columns, the
# buffer writers at their capacity.
TEXT_UTIL := $(SHARED)/text_util/text_util.c $(SHARED)/text_util/text_util_test.c $(SHARED)/text_util/text_util.h
build/text_util_test: $(TEXT_UTIL) | build
	$(CC) $(CFLAGS) $(SHARED)/text_util/text_util.c $(SHARED)/text_util/text_util_test.c -o $@

run: $(addprefix build/,$(TESTS)) build/check_test build/text_util_test build/ios_smoke
	./build/check_test
	./build/text_util_test
	./build/pk_test
	./build/pk_rules_test
	./build/pk_plan_test
	./build/pk_say_test
	./build/pk_fuzz $(FUZZ_GAMES)
	./build/pk_msg_test $(WIRE_GAMES)
	./build/pk_twophone_test
	./build/pk_beats_test
	./build/pk_arrange_test
	./build/pk_bot_test
	./build/ios_smoke

ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN)

build/asan_%: tests/%.c $(SRC) $(HDR) | build
	$(CC) $(ASAN_FLAGS) $(SRC) $< -o $@

build/asan_pk_bot_test: tests/pk_bot_test.c $(SRC) $(BOT_SRC) $(HDR) | build
	$(CC) $(ASAN_FLAGS) $(SRC) $(BOT_SRC) $< -o $@

build/asan_check_test: $(CHECK_TEST) | build
	$(CC) $(ASAN_FLAGS) $(SHARED)/test/check_test.c -o $@

build/asan_text_util_test: $(TEXT_UTIL) | build
	$(CC) $(ASAN_FLAGS) $(SHARED)/text_util/text_util.c $(SHARED)/text_util/text_util_test.c -o $@

asan: $(addprefix build/asan_,$(TESTS)) build/asan_check_test build/asan_text_util_test build/asan_ios_smoke
	./build/asan_check_test
	./build/asan_text_util_test
	./build/asan_pk_test
	./build/asan_pk_rules_test
	./build/asan_pk_plan_test
	./build/asan_pk_say_test
	./build/asan_pk_fuzz 2000
	./build/asan_pk_msg_test 5 100
	./build/asan_pk_twophone_test
	./build/asan_pk_beats_test 60
	./build/asan_pk_arrange_test
	./build/asan_pk_bot_test 1
	./build/asan_ios_smoke

# THE SAME FILES FOR wasm32, freestanding (-nostdlib, shared/c/wasm's libc):
# the proof that nothing here reaches past memcpy / memset / memcmp / strlen /
# strncmp. Objects only; the replay page's module is linked by whoever builds
# that page.
# WASM_CC and the guard that refuses Apple clang: shared/tools/wasm_cc.mk.
include ../../shared/tools/wasm_cc.mk
WASM_SRC := $(SRC)
wasm: wasm-cc-check | build
	@mkdir -p build/wasm
	@for s in $(WASM_SRC); do \
	  o=build/wasm/$$(basename $$s .c).o; \
	  $(WASM_CC) --target=wasm32 -Oz -nostdlib -ffreestanding $(WARN) \
	    -isystem $(SHARED)/wasm/include -c $$s -o $$o || exit 1; \
	done
	@echo "wasm32 objects in build/wasm/"

# 7.3.7, NATIVE AGAINST WASM (tests/pk_cross.c): the same games played by
# this build of the kernel and by a wasm32 build of it, run in node, must
# end in the same states. Needs a wasm clang with wasm-ld (WASM_CC, which
# defaults to the pinned Homebrew LLVM on a Mac) and node, so it is not in
# `run`. CROSS_GAMES games (the rules doc's 100 by default).
CROSS_GAMES ?= 100
CROSS_WASM_SRC := $(SRC) $(SHARED)/wasm/libc.c tests/pk_cross.c
build/pk_cross: tests/pk_cross.c $(SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(SRC) tests/pk_cross.c -o $@
build/pk_cross.wasm: $(CROSS_WASM_SRC) $(HDR) | build wasm-cc-check
	$(WASM_CC) --target=wasm32 -O2 -nostdlib -ffreestanding -mbulk-memory $(WARN) -DPK_CROSS_WASM \
	  -isystem $(SHARED)/wasm/include \
	  -Wl,--no-entry -Wl,--export=pk_cross_run -Wl,--export=pk_cross_hashes -Wl,--export-memory \
	  -Wl,-z,stack-size=1048576 $(CROSS_WASM_SRC) -o $@
cross: build/pk_cross build/pk_cross.wasm
	./build/pk_cross $(CROSS_GAMES) > build/cross_native.txt
	node tests/pk_cross.mjs build/pk_cross.wasm $(CROSS_GAMES) > build/cross_wasm.txt
	@cmp build/cross_native.txt build/cross_wasm.txt \
	  && echo "cross: $$(wc -l < build/cross_native.txt | tr -d ' ') games, native and wasm32 end alike"

# ---- the iOS bridge -----------------------------------------------------------
# ios-smoke runs the identical bridge with the host compiler, so every
# invariant on it is testable WITHOUT a Mac; ios-lib needs Xcode. The shape is
# uttt's (uttt/c/Makefile ios-lib), and the layout stamp is foolish's
# (foolish/c/Makefile build/layout/ios.hash).
IOS_MIN_VERSION := 17.0
IOS_SRC    := $(SRC) ios/pk_api.c ios/pk_lay.c
IOS_HDR    := $(HDR) ios/include/pk_api.h ios/pk_api_layout.h
IOS_CFLAGS := -O2 -Isrc -Iios -Iios/include $(WARN)

build/ios_smoke: $(IOS_SRC) $(IOS_HDR) ios/pk_api_smoke.c | build
	$(CC) $(CFLAGS) -Iios/include $(IOS_SRC) ios/pk_api_smoke.c -lm -o $@

build/asan_ios_smoke: $(IOS_SRC) $(IOS_HDR) ios/pk_api_smoke.c | build
	$(CC) $(ASAN_FLAGS) -Iios/include $(IOS_SRC) ios/pk_api_smoke.c -lm -o $@

ios-smoke: build/ios_smoke
	./build/ios_smoke

# THE GENERATED SWIFT. Swift never learns a byte layout: every struct pk_api.h
# hands over is read through readers shared/tools/structgen writes from the
# kernel's own headers (ios/layout.args), and every string key through
# shared/tools/datagen. A BUILD OUTPUT, never committed (../ios/Generated is
# ignored), exactly as foolish's sdk/swift/gen is.
GEN          := ../ios/Generated
STRUCTGEN_DIR := ../../shared/tools/structgen
DATAGEN_DIR  := ../../shared/tools/datagen
STRUCTGEN    := $(STRUCTGEN_DIR)/build/structgen
DATAGEN      := $(DATAGEN_DIR)/build/datagen
SG_SPEC      := ios/layout.args
SG_ARGS       = $(shell grep -v '^[[:space:]]*\#' $(SG_SPEC))
# The device is arm64 and the simulator arm64 or x86_64; one hash is stamped
# into all three slices, so all three must lay these structs out alike.
IOS_LAYOUT_TRIPLE  := arm64-apple-ios$(IOS_MIN_VERSION)
IOS_LAYOUT_TRIPLES := $(IOS_LAYOUT_TRIPLE) arm64-apple-ios$(IOS_MIN_VERSION)-simulator \
                      x86_64-apple-ios$(IOS_MIN_VERSION)-simulator

# Every source of the tool, not one file: a stale structgen bakes a hash for a
# layout that is no longer the layout (foolish/c/Makefile has the story).
$(STRUCTGEN): $(wildcard $(STRUCTGEN_DIR)/*.c) $(wildcard $(STRUCTGEN_DIR)/*.h) \
              $(wildcard $(STRUCTGEN_DIR)/../sgcommon/*.c) $(wildcard $(STRUCTGEN_DIR)/../sgcommon/*.h) \
              $(STRUCTGEN_DIR)/Makefile $(STRUCTGEN_DIR)/../llvm.mk
	$(MAKE) -C $(STRUCTGEN_DIR) build/structgen
$(DATAGEN): $(DATAGEN_DIR)/datagen.c $(wildcard $(DATAGEN_DIR)/../sgcommon/*.c) \
            $(wildcard $(DATAGEN_DIR)/../sgcommon/*.h) $(DATAGEN_DIR)/Makefile $(DATAGEN_DIR)/../llvm.mk
	$(MAKE) -C $(DATAGEN_DIR) build/datagen

# cmp-then-copy: a generated file's mtime moves only when its content does, so
# a rebuild that changes no layout does not make Xcode recompile the kit.
sync = mkdir -p $(dir $(2)) && { cmp -s $(1) $(2) || { cp $(1) $(2); echo "wrote $(2)"; }; } && rm -f $(1)

build/layout/ios.hash: $(STRUCTGEN) $(SG_SPEC) $(IOS_HDR) FORCE | build
	@mkdir -p build/layout
	@for t in $(IOS_LAYOUT_TRIPLES); do \
	  $(STRUCTGEN) --cwd . $(SG_ARGS) --build 'ios=$(strip $(IOS_CFLAGS))' --target $$t --print-hash || exit 1; \
	done | sort -u > $@.new
	@[ "$$(grep -c . $@.new)" = 1 ] || { echo "layout ios: the iOS slices do not share one layout:"; cat $@.new; exit 1; }
	@{ cmp -s $@.new $@ || { echo "layout ios: $$(cat $@ 2>/dev/null || echo none) -> $$(cat $@.new)"; mv $@.new $@; }; }; rm -f $@.new
	@$(STRUCTGEN) --cwd . $(SG_ARGS) --build 'ios=$(strip $(IOS_CFLAGS))' --target $(IOS_LAYOUT_TRIPLE) \
	  --swift $@.swift && $(call sync,$@.swift,$(GEN)/PickemupKernel.swift)

structgen: build/layout/ios.hash

# The words: every key by name, and the English table, as Swift. The kernel
# composes every sentence (pk_api_words); these are for a host that names a
# key (pk_api_string) and for its tests.
datagen: $(DATAGEN) i18n/keys.h i18n/strings_en.c | build
	@mkdir -p build/i18n
	@$(DATAGEN) --cwd i18n --header keys.h --table PK_KEY_NAME --require-complete \
	  --name PickemupStringKeys --swift build/i18n/PickemupStringKeys.swift \
	  && $(call sync,build/i18n/PickemupStringKeys.swift,$(GEN)/i18n/PickemupStringKeys.swift)
	@$(DATAGEN) --cwd i18n --header strings_en.c --table PK_STRINGS_EN --labels PK_STRINGS_EN.0=PK_KEY_NAME \
	  --require-complete --name PickemupStringsEn --swift build/i18n/PickemupStringsEn.swift \
	  && $(call sync,build/i18n/PickemupStringsEn.swift,$(GEN)/i18n/PickemupStringsEn.swift)

# THE SWIFT SIDE, ON THIS MAC: the readers generated for this machine's
# triple, the kernel compiled for it and stamped with that layout's hash, and
# a Swift program that drives the bridge through them (ios/pk_api_smoke.swift).
# Needs swiftc and libclang, so it is not in `run`; the CI lane runs structgen
# and datagen on Linux, and this on a Mac.
SWIFT_TRIPLE = $(shell swiftc -print-target-info 2>/dev/null | sed -n 's/.*"unversionedTriple": *"\([^"]*\)".*/\1/p' | head -1)
swift-smoke: $(STRUCTGEN) datagen | build
	@rm -rf build/swift && mkdir -p build/swift/obj
	@$(STRUCTGEN) --cwd . $(SG_ARGS) --build 'host=$(strip $(IOS_CFLAGS))' --target $(SWIFT_TRIPLE) \
	  --swift build/swift/PickemupKernel.swift --print-hash > build/swift/hash
	@for s in $(IOS_SRC); do \
	  $(CC) $(IOS_CFLAGS) -DSG_LAYOUT_HASH=$$(cat build/swift/hash) -c $$s -o build/swift/obj/$$(basename $$s .c).o || exit 1; \
	done
	swiftc -parse-as-library -import-objc-header ios/include/pk_api.h build/swift/PickemupKernel.swift \
	  $(GEN)/i18n/*.swift ios/pk_api_smoke.swift build/swift/obj/*.o -o build/swift/smoke
	./build/swift/smoke

# The device slice, both simulator slices, the lipo and the xcframework are
# the shared recipe (lifted from uttt, REUSE_AUDIT S4).
include ../../shared/tools/ios_xcframework.mk

# THE LIBRARY CARRIES THE LAYOUT IT WAS COMPILED FOR, and the readers are
# written beside it in the same run, so the pair cannot come apart.
ios-lib: IOS_CFLAGS += -DSG_LAYOUT_HASH=$$(cat build/layout/ios.hash)
ios-lib: build/layout/ios.hash datagen
	$(call IOS_XCFRAMEWORK,pickemup,$(IOS_SRC),$(IOS_CFLAGS),ios/include,$(IOS_MIN_VERSION),../ios/vendor/Pickemup.xcframework)
	@echo "layout $$(cat build/layout/ios.hash)"

# THE ARENA (tools/pk_arena.c): line-ups of the bots at 2, 4 and 8 players,
# every seed both ways round, a 95% interval on each side's win share.
# ARENA_GAMES games a line-up across ARENA_JOBS worker processes.
ARENA_GAMES ?= 2000
ARENA_JOBS  ?= 8
build/pk_arena: tools/pk_arena.c $(SRC) $(BOT_SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(SRC) $(BOT_SRC) tools/pk_arena.c -lm -o $@
arena: build/pk_arena
	./build/pk_arena $(ARENA_GAMES) $(ARENA_JOBS)

# THE TERMINAL GAME (tools/pk_play.c): you against five Monte Carlo bots,
# every move through the kernel. PLAY_ARGS="--seed 42 --bots 5 --seat 0".
PLAY_ARGS ?=
build/pk_play: tools/pk_play.c $(SRC) $(BOT_SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(SRC) $(BOT_SRC) tools/pk_play.c -o $@
play: build/pk_play
	./build/pk_play $(PLAY_ARGS)
.PHONY: play

# The timeline of two takes as Markdown tables (docs/MOTION_REPORT.md).
beats-dump: build/pk_beats_dump
	./build/pk_beats_dump

clean:
	rm -rf build

.PHONY: all run asan arena wasm cross ios-smoke beats-dump swift-smoke structgen datagen ios-lib clean FORCE
FORCE:
.SECONDARY:
