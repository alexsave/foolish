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
           $(SHARED)/text_util/text_util.c $(SHARED)/wire_check/wire_check.c \
           $(SHARED)/msg_seat_tag/msg_seat_tag.c $(SHARED)/msg_lobby_roster/msg_lobby_roster.c
# THE BOTS ARE NOT IN THE APP (DECISION D63): only the bot test and the arena
# link them, so SRC, the wasm objects and the xcframework never carry them.
BOT_SRC := src/pk_belief.c src/pk_bot.c

# One binary per test file; each takes an optional scale argument.
TESTS   := pk_test pk_rules_test pk_plan_test pk_say_test pk_fuzz pk_msg_test pk_twophone_test pk_beats_test pk_arrange_test pk_bot_test
FUZZ_GAMES ?= 2800
# Games per table size that go through the wire after every bubble. The rules
# doc's 10,000 is `./build/pk_msg_test 10000` by hand (DECISION D43).
WIRE_GAMES ?= 30

all: $(addprefix build/,$(TESTS))

# ---- how a file becomes an object, and a program ----------------------------
# shared/tools/c_rules.mk: one directory of objects per compiler-and-flags (a
# "variant"), each object depending on exactly the headers its compile opened
# and on the command line it was built with. There is no list of headers here
# any more: the one there was had to name every header any test could reach,
# and the bridge's besides for the two tests that compile it in
# (tests/pk_twophone_test.c, tests/pk_arrange_test.c).
#
# -Iios/include is for the bridge's own files (ios/...): said per object
# (c_rules.mk, $*), so the kernel is compiled once for the tests and the smoke
# alike.
ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN)
HOST_FLAGS  = $(CFLAGS) $(if $(filter ios/%,$*),-Iios/include)
SAN_FLAGS   = $(ASAN_FLAGS) $(if $(filter ios/%,$*),-Iios/include)

# shared/c's own tests, the ones for what this kernel links: the harness
# (TEST, CHECK, report, STEP, OK), the text writers, the envelope's truncated
# SHA-256 that pk_msg.c carries PK_CHECK_LEN bytes of, the seat identity
# pk_msg_resolve hands its rows to, the lobby rules the lobby adapter and
# pk_msg.c call, the arena's statistics, and the auto-collapse push
# pk_api_collapse_push forwards to. What each is built from is
# shared/tools/shared_tests.mk's.
SHARED_TESTS := check_test rng_test text_util_test wire_check_test msg_seat_tag_test msg_lobby_roster_test \
                stats_test collapse_test

include ../../shared/tools/c_rules.mk
include ../../shared/tools/shared_tests.mk
$(eval $(call C_VARIANT,build/obj/host,CC,HOST_FLAGS))
$(eval $(call C_VARIANT,build/obj/asan,CC,SAN_FLAGS))
$(call SHARED_TESTS_IN,,build/obj/host,CC,CFLAGS)
$(call SHARED_TESTS_IN,asan_,build/obj/asan,CC,ASAN_FLAGS)

# $(call prog,<name>,<what it is besides the kernel>,<more to link>): the
# program build/<name>, and its sanitized twin build/asan_<name>.
define PROG
build/$(1): $$(call c_objs,build/obj/host,$$(SRC) $(2))
	$$(CC) $$(CFLAGS) $$^ $(3) -o $$@
build/asan_$(1): $$(call c_objs,build/obj/asan,$$(SRC) $(2))
	$$(CC) $$(ASAN_FLAGS) $$^ $(3) -o $$@
endef
prog = $(eval $(call PROG,$(1),$(2),$(3)))

# One binary per test file; the bot test links the bots.
$(foreach t,$(filter-out pk_bot_test,$(TESTS)),$(call prog,$(t),tests/$(t).c))
$(call prog,pk_bot_test,$(BOT_SRC) tests/pk_bot_test.c)

run: $(addprefix build/,$(TESTS)) shared-tests build/ios_smoke
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

asan: $(addprefix build/asan_,$(TESTS)) shared-tests-asan build/asan_ios_smoke
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
# WASM_CC, the guard that refuses Apple clang and the `wasm` target itself:
# shared/tools/wasm_cc.mk.
include ../../shared/tools/wasm_cc.mk
$(eval $(call WASM_OBJECT_PROOF,$(SRC)))

# 7.3.7, NATIVE AGAINST WASM (tests/pk_cross.c): the same games played by
# this build of the kernel and by a wasm32 build of it, run in node, must
# end in the same states. Needs a wasm clang with wasm-ld (WASM_CC, which
# defaults to the pinned Homebrew LLVM on a Mac) and node, so it is not in
# `run`. CROSS_GAMES games (the rules doc's 100 by default).
CROSS_GAMES ?= 100
CROSS_WASM_SRC := $(SRC) $(SHARED)/wasm/libc.c tests/pk_cross.c
$(call prog,pk_cross,tests/pk_cross.c)
# What compiles a file and what links the module, apart: the linker's flags
# mean nothing to a compile. The link is still handed every flag the single
# command had, so the driver does there exactly what it did.
CROSS_WASM_CFLAGS  := --target=wasm32 -O2 -nostdlib -ffreestanding -mbulk-memory $(WARN) -DPK_CROSS_WASM \
                      -isystem $(SHARED)/wasm/include
CROSS_WASM_LDFLAGS := -Wl,--no-entry -Wl,--export=pk_cross_run -Wl,--export=pk_cross_hashes -Wl,--export-memory \
                      -Wl,-z,stack-size=1048576
$(eval $(call C_VARIANT,build/obj/cross,WASM_CC,CROSS_WASM_CFLAGS))
build/pk_cross.wasm: $(call c_objs,build/obj/cross,$(CROSS_WASM_SRC)) | wasm-cc-check
	$(WASM_CC) $(CROSS_WASM_CFLAGS) $(CROSS_WASM_LDFLAGS) $^ -o $@
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
IOS_CFLAGS := -O2 -Isrc -Iios -Iios/include $(WARN)

$(call prog,ios_smoke,ios/pk_api.c ios/pk_lay.c ios/pk_api_smoke.c,-lm)

ios-smoke: build/ios_smoke
	./build/ios_smoke

# THE GENERATED SWIFT. Swift never learns a byte layout: every struct pk_api.h
# hands over is read through readers shared/tools/structgen writes from the
# kernel's own headers (ios/layout.args), and every string key through
# shared/tools/datagen. A BUILD OUTPUT, never committed (../ios/Generated is
# ignored), exactly as foolish's sdk/swift/gen is.
#
# THE LIBRARY CARRIES THE LAYOUT IT WAS COMPILED FOR (-DSG_LAYOUT_HASH), and
# the readers are written beside it by the same rule, so the pair cannot come
# apart. The device is arm64 and the simulator arm64 or x86_64; one hash is
# stamped into all three slices, so all three must lay these structs out
# alike. Where the generators are, the layout rule and the slices themselves:
# shared/tools/gen_tools.mk and shared/tools/ios_xcframework.mk.
GEN              := ../ios/Generated
IOS_SG_SPEC      := ios/layout.args
IOS_KERNEL_SWIFT := $(GEN)/PickemupKernel.swift
include ../../shared/tools/gen_tools.mk
include ../../shared/tools/ios_xcframework.mk

structgen: build/layout/ios.hash

# The words: every key by name, and the English table, as Swift. The kernel
# composes every sentence (pk_api_words); these are for a host that names a
# key (pk_api_string) and for its tests.
datagen: $(DATAGEN)
	$(call DATAGEN_I18N,PK,Pickemup,$(GEN)/i18n)

# THE SWIFT SIDE, ON THIS MAC: the readers generated for this machine's
# triple, the kernel compiled for it and stamped with that layout's hash, and
# a Swift program that drives the bridge through them (ios/pk_api_smoke.swift).
# Needs swiftc and libclang, so it is not in `run`; the CI lane runs structgen
# and datagen on Linux, and this on a Mac.
swift-smoke: $(STRUCTGEN) datagen
	$(call IOS_SWIFT_SMOKE,ios/include/pk_api.h,PickemupKernel,$(GEN)/i18n,ios/pk_api_smoke.swift)

# The device slice, both simulator slices, the lipo and the xcframework are
# the shared recipe, and it rebuilds only what changed - which is what lets
# Xcode run this target on every build (shared/scripts/xcode_kernel.sh).
$(eval $(call IOS_XCFRAMEWORK,pickemup,$(IOS_SRC),ios/include,../ios/vendor/Pickemup.xcframework))
ios-lib: datagen $(IOS_XCFRAMEWORKS)
	@echo "layout $$(cat build/layout/ios.hash)"

# THE ARENA (tools/pk_arena.c): line-ups of the bots at 2, 4 and 8 players,
# every seed both ways round, a 95% interval on each side's win share.
# ARENA_GAMES games a line-up across ARENA_JOBS worker processes.
ARENA_GAMES ?= 2000
ARENA_JOBS  ?= 8
$(call prog,pk_arena,$(BOT_SRC) $(SHARED)/stats/stats.c tools/pk_arena.c,-lm)
arena: build/pk_arena
	./build/pk_arena $(ARENA_GAMES) $(ARENA_JOBS)

# THE TERMINAL GAME (tools/pk_play.c): you against five Monte Carlo bots,
# every move through the kernel. PLAY_ARGS="--seed 42 --bots 5 --seat 0".
PLAY_ARGS ?=
$(call prog,pk_play,$(BOT_SRC) tools/pk_play.c)
play: build/pk_play
	./build/pk_play $(PLAY_ARGS)
.PHONY: play

# The timeline of two takes as Markdown tables (docs/MOTION_REPORT.md).
$(call prog,pk_beats_dump,tests/pk_beats_dump.c)
beats-dump: build/pk_beats_dump
	./build/pk_beats_dump

clean:
	rm -rf build

.PHONY: all run asan arena cross ios-smoke beats-dump swift-smoke structgen datagen ios-lib clean
