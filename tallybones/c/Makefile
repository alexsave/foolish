# Tallybones - the kernel, its wire, its iOS bridge and its tests. `make run`
# is the real test target. The shape is pickemup/c/Makefile's.
#
#   make -C tallybones/c run         every test and the bridge smoke, built and run
#   make -C tallybones/c asan        the same under ASan + UBSan
#   make -C tallybones/c wasm        prove the kernel builds for wasm32, freestanding
#   make -C tallybones/c ios-smoke   every bridge entry point, host compiler, no Mac
#   make -C tallybones/c structgen   the Swift readers  -> ../ios/Generated/ (needs libclang)
#   make -C tallybones/c datagen     the Swift strings  -> ../ios/Generated/i18n/
#   make -C tallybones/c ios-lib     ../ios/vendor/Tallybones.xcframework (needs Xcode)
#   make -C tallybones/c ios-lib-catalyst   the same plus a Mac Catalyst slice, for the Swift tests
#   make -C tallybones/c swift-smoke the bridge driven from Swift through the readers (a Mac)
#   make -C tallybones/c bot-test    the exact solver's checks (bot/, bot/README.md); bot-asan, bot-solve
#   make -C tallybones/c clean
CC      ?= cc
# THE WARNINGS ARE ONE LIST for every build below.
WARN    := -Wall -Wextra -Werror -std=c11
CFLAGS  ?= -O2 $(WARN)
SHARED  := ../../shared/c
SRC     := src/tb.c src/tb_code.c src/tb_plan.c src/tb_view.c src/tb_lobby.c src/tb_msg.c \
           src/tb_say.c src/tb_beats.c i18n/strings_en.c \
           $(SHARED)/deal_rng.c $(SHARED)/sha256.c $(SHARED)/b32.c $(SHARED)/mixrad.c \
           $(SHARED)/text_util/text_util.c $(SHARED)/wire_check/wire_check.c \
           $(SHARED)/msg_seat_tag/msg_seat_tag.c $(SHARED)/msg_lobby_roster/msg_lobby_roster.c

# One binary per test file; each takes an optional scale argument.
TESTS   := tb_test tb_msg_test tb_fuzz tb_say_test tb_beats_test tb_twophone_test
FUZZ_GAMES ?= 1400
WIRE_GAMES ?= 20

all: $(addprefix build/,$(TESTS))

# ---- how a file becomes an object, and a program ----------------------------
# shared/tools/c_rules.mk: one directory of objects per compiler-and-flags (a
# "variant"), each object depending on exactly the headers its compile opened
# and on the command line it was built with. There is no list of headers here
# any more: the one there was had to name every header any test could reach,
# and tb_twophone_test's (it compiles the bridge in) besides.
#
# -Iios/include is for the bridge's own files (ios/...): said per object
# (c_rules.mk, $*), so the kernel is compiled once for the tests and the smoke
# alike.
ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN)
HOST_FLAGS  = $(CFLAGS) $(if $(filter ios/%,$*),-Iios/include)
SAN_FLAGS   = $(ASAN_FLAGS) $(if $(filter ios/%,$*),-Iios/include)

# shared/c's own tests, the ones for what this kernel links: the harness
# (TEST, CHECK, report, STEP, OK), the text writers, the envelope's truncated
# SHA-256 that tb_msg.c carries TB_CHECK_LEN bytes of, the seat identity
# tb_msg_resolve hands its rows to, and the lobby rules the lobby adapter and
# tb_msg.c call. What each is built from is shared/tools/shared_tests.mk's.
SHARED_TESTS := check_test rng_test text_util_test wire_check_test msg_seat_tag_test msg_lobby_roster_test

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

# One binary per test file.
$(foreach t,$(TESTS),$(call prog,$(t),tests/$(t).c))

run: $(addprefix build/,$(TESTS)) shared-tests build/ios_smoke
	./build/tb_test
	./build/tb_msg_test $(WIRE_GAMES)
	./build/tb_fuzz $(FUZZ_GAMES)
	./build/tb_say_test
	./build/tb_beats_test
	./build/tb_twophone_test
	./build/ios_smoke

asan: $(addprefix build/asan_,$(TESTS)) shared-tests-asan build/asan_ios_smoke
	./build/asan_tb_test
	./build/asan_tb_msg_test 2
	./build/asan_tb_fuzz 140
	./build/asan_tb_say_test
	./build/asan_tb_beats_test
	./build/asan_tb_twophone_test
	./build/asan_ios_smoke

# THE SAME FILES FOR wasm32, freestanding (-nostdlib, shared/c/wasm's libc):
# the proof that nothing here reaches past memcpy / memset / memcmp / strlen /
# strncmp. Objects only.
# WASM_CC, the guard that refuses Apple clang and the `wasm` target itself:
# shared/tools/wasm_cc.mk.
include ../../shared/tools/wasm_cc.mk
$(eval $(call WASM_OBJECT_PROOF,$(SRC)))

# ---- the iOS bridge -----------------------------------------------------------
# ios-smoke runs the identical bridge with the host compiler, so every
# invariant on it is testable WITHOUT a Mac; ios-lib needs Xcode. The shape is
# uttt's (uttt/c/Makefile ios-lib), and the layout stamp is foolish's
# (foolish/c/Makefile build/layout/ios.hash).
IOS_MIN_VERSION := 17.0
IOS_SRC    := $(SRC) ios/tb_api.c ios/tb_lay.c
IOS_CFLAGS := -O2 -Isrc -Iios -Iios/include $(WARN)

$(call prog,ios_smoke,ios/tb_api.c ios/tb_lay.c ios/tb_api_smoke.c,-lm)

ios-smoke: build/ios_smoke
	./build/ios_smoke

# THE GENERATED SWIFT. Swift never learns a byte layout: every struct tb_api.h
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
IOS_KERNEL_SWIFT := $(GEN)/TallybonesKernel.swift
include ../../shared/tools/gen_tools.mk
include ../../shared/tools/ios_xcframework.mk

structgen: build/layout/ios.hash

# The words: every key by name, and the English table, as Swift. The kernel
# composes every sentence (tb_api_words); these are for a host that names a
# key (tb_api_string) and for its tests.
datagen: $(DATAGEN)
	$(call DATAGEN_I18N,TB,Tallybones,$(GEN)/i18n)

# THE SWIFT SIDE, ON THIS MAC: the readers generated for this machine's
# triple, the kernel compiled for it and stamped with that layout's hash, and
# a Swift program that drives the bridge through them (ios/tb_api_smoke.swift).
# Needs swiftc and libclang, so it is not in `run`; the CI lane runs structgen
# and datagen on Linux, and this on a Mac.
swift-smoke: $(STRUCTGEN) datagen
	$(call IOS_SWIFT_SMOKE,ios/include/tb_api.h,TallybonesKernel,$(GEN)/i18n,ios/tb_api_smoke.swift)

# The device slice, both simulator slices, the lipo and the xcframework are
# the shared recipe, and it rebuilds only what changed - which is what lets
# Xcode run this target on every build (shared/scripts/xcode_kernel.sh).
XCF := ../ios/vendor/Tallybones.xcframework
$(eval $(call IOS_XCFRAMEWORK,tallybones,$(IOS_SRC),ios/include,$(XCF)))
ios-lib: datagen $(IOS_XCFRAMEWORKS)
	@echo "layout $$(cat build/layout/ios.hash)"

# THE SAME XCFRAMEWORK WITH A MAC CATALYST SLICE, for the Swift tests on a Mac
# with no simulator free (DECISIONS T56, T63): an arm64 macabi slice of the
# same sources stamped with the same hash, and the xcframework rewritten with
# all three libraries. The macabi triple is checked to lay the structs out as
# the iOS slices do, or the stamp would lie. Never shipped.
#
# ITS OWN FILE STANDS FOR IT (the slice's library inside the xcframework), and
# not Info.plist, because `ios-lib` and this write the same directory: when
# `ios-lib` rewrites the xcframework with two libraries that file is gone, and
# the next `ios-lib-catalyst` puts the third back; when this one wrote it last,
# `ios-lib` finds its Info.plist current and leaves all three alone.
CATALYST_TRIPLE := arm64-apple-ios$(IOS_MIN_VERSION)-macabi
IOS_CC_catalyst  = xcrun -sdk macosx clang -target $(CATALYST_TRIPLE)
$(eval $(call IOS_SLICE,catalyst))
$(eval $(call IOS_SLICE_LIB,tallybones,catalyst,$(IOS_SRC)))
CATALYST_LIB := $(XCF)/ios-arm64-maccatalyst/libtallybones.a
$(CATALYST_LIB): build/ios/device/libtallybones.a build/ios/sim/libtallybones.a build/ios/catalyst/libtallybones.a $(shell find ios/include -type f)
	@h=$$($(STRUCTGEN) --cwd . $(IOS_SG_ARGS) --build 'ios=$(strip $(IOS_CFLAGS))' --target $(CATALYST_TRIPLE) --print-hash) \
	  && [ "$$h" = "$$(cat build/layout/ios.hash)" ] \
	  || { echo "layout catalyst: $$h is not the iOS slices' $$(cat build/layout/ios.hash)"; exit 1; }
	@rm -rf $(XCF)
	xcodebuild -create-xcframework \
	  -library build/ios/device/libtallybones.a -headers ios/include \
	  -library build/ios/sim/libtallybones.a -headers ios/include \
	  -library build/ios/catalyst/libtallybones.a -headers ios/include \
	  -output $(XCF)
	@echo "wrote $(XCF) (device, simulator, Mac Catalyst)"
ios-lib-catalyst: datagen $(CATALYST_LIB)
	@echo "layout $$(cat build/layout/ios.hash)"

# ---- THE EXACT SINGLE-PLAYER SOLVER (bot/, DECISIONS.md T30 onwards) -----------
# An internal simulation and evaluation tool; nothing here ships: bot/ is in no
# SRC list, and it is not in `run` or `asan` (no workflow ran it before it was
# built here, and none does now; its test alone is about 35 s on 8 cores).
#   make -C tallybones/c bot-test    build and run every check (tb_bot_test, 200000 games)
#   make -C tallybones/c bot-asan    the same under ASan + UBSan (a smaller simulation)
#   make -C tallybones/c bot-solve   build/tb_solve, then print the exact EV and play 200000 games
#
# IT HAD ITS OWN MAKEFILE (bot/Makefile), which listed the kernel's headers by
# hand and compiled the kernel again from source for each program. Its flags
# were this file's, both sets, so its programs are built from this file's own
# objects now (build/obj/host, build/obj/asan): the bot scores with the one
# tb_score_of the kernel's tests are run against, object for object.
#
# THE KERNEL FILES IT LINKS ARE A SUBSET (BOT_KSRC), as they always were: the
# rules, the code and the plan. Not SRC - the solver has no wire and no words.
BOT_KSRC := src/tb.c src/tb_code.c src/tb_plan.c $(SHARED)/deal_rng.c $(SHARED)/sha256.c $(SHARED)/mixrad.c
BOT_SRC  := bot/tb_bot.c $(SHARED)/stats/stats.c
BOT_LIBS := -lpthread -lm

build/tb_solve: $(call c_objs,build/obj/host,$(BOT_SRC) $(BOT_KSRC) bot/tb_solve.c)
	$(CC) $(CFLAGS) $^ -o $@ $(BOT_LIBS)
build/tb_bot_test: $(call c_objs,build/obj/host,$(BOT_SRC) $(BOT_KSRC) bot/tb_bot_test.c)
	$(CC) $(CFLAGS) $^ -o $@ $(BOT_LIBS)
build/asan_tb_bot_test: $(call c_objs,build/obj/asan,$(BOT_SRC) $(BOT_KSRC) bot/tb_bot_test.c)
	$(CC) $(ASAN_FLAGS) $^ -o $@ $(BOT_LIBS)

bot-test: build/tb_bot_test build/tb_solve
	./build/tb_bot_test 200000

bot-solve: build/tb_solve
	./build/tb_solve play 200000

bot-asan: build/asan_tb_bot_test
	./build/asan_tb_bot_test 20000

.PHONY: bot-test bot-solve bot-asan

clean:
	rm -rf build

.PHONY: all run asan ios-smoke swift-smoke structgen datagen ios-lib ios-lib-catalyst clean
