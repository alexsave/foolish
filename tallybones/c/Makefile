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
#   make -C tallybones/c clean
CC      ?= cc
# THE WARNINGS ARE ONE LIST for every build below.
WARN    := -Wall -Wextra -Werror -std=c11
CFLAGS  ?= -O2 $(WARN)
SHARED  := ../../shared/c
SRC     := src/tb.c src/tb_code.c src/tb_plan.c src/tb_view.c src/tb_lobby.c src/tb_msg.c \
           src/tb_say.c src/tb_beats.c i18n/strings_en.c \
           $(SHARED)/deal_rng.c $(SHARED)/sha256.c $(SHARED)/b32.c $(SHARED)/mixrad.c \
           $(SHARED)/text_util/text_util.c
HDR     := $(wildcard src/*.h) i18n/keys.h tests/tb_check.h \
           $(SHARED)/deal_rng.h $(SHARED)/sha256.h $(SHARED)/b32.h $(SHARED)/mixrad.h \
           $(SHARED)/test/check.h $(SHARED)/test/twophone.h $(SHARED)/text_util/text_util.h

# One binary per test file; each takes an optional scale argument.
TESTS   := tb_test tb_msg_test tb_fuzz tb_say_test tb_beats_test tb_twophone_test
FUZZ_GAMES ?= 1400
WIRE_GAMES ?= 20

all: $(addprefix build/,$(TESTS))

# tb_twophone_test compiles the bridge in.
build/tb_twophone_test build/asan_tb_twophone_test: ios/tb_api.c ios/tb_lay.c ios/include/tb_api.h ios/tb_api_layout.h

build:
	@mkdir -p build

build/%: tests/%.c $(SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(SRC) $< -o $@

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
	./build/tb_test
	./build/tb_msg_test $(WIRE_GAMES)
	./build/tb_fuzz $(FUZZ_GAMES)
	./build/tb_say_test
	./build/tb_beats_test
	./build/tb_twophone_test
	./build/ios_smoke

ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN)

build/asan_%: tests/%.c $(SRC) $(HDR) | build
	$(CC) $(ASAN_FLAGS) $(SRC) $< -o $@

build/asan_check_test: $(CHECK_TEST) | build
	$(CC) $(ASAN_FLAGS) $(SHARED)/test/check_test.c -o $@

build/asan_text_util_test: $(TEXT_UTIL) | build
	$(CC) $(ASAN_FLAGS) $(SHARED)/text_util/text_util.c $(SHARED)/text_util/text_util_test.c -o $@

asan: $(addprefix build/asan_,$(TESTS)) build/asan_check_test build/asan_text_util_test build/asan_ios_smoke
	./build/asan_check_test
	./build/asan_text_util_test
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

# ---- the iOS bridge -----------------------------------------------------------
# ios-smoke runs the identical bridge with the host compiler, so every
# invariant on it is testable WITHOUT a Mac; ios-lib needs Xcode. The shape is
# uttt's (uttt/c/Makefile ios-lib), and the layout stamp is foolish's
# (foolish/c/Makefile build/layout/ios.hash).
IOS_MIN_VERSION := 17.0
IOS_SRC    := $(SRC) ios/tb_api.c ios/tb_lay.c
IOS_HDR    := $(HDR) ios/include/tb_api.h ios/tb_api_layout.h
IOS_CFLAGS := -O2 -Isrc -Iios -Iios/include $(WARN)

build/ios_smoke: $(IOS_SRC) $(IOS_HDR) ios/tb_api_smoke.c | build
	$(CC) $(CFLAGS) -Iios/include $(IOS_SRC) ios/tb_api_smoke.c -lm -o $@

build/asan_ios_smoke: $(IOS_SRC) $(IOS_HDR) ios/tb_api_smoke.c | build
	$(CC) $(ASAN_FLAGS) -Iios/include $(IOS_SRC) ios/tb_api_smoke.c -lm -o $@

ios-smoke: build/ios_smoke
	./build/ios_smoke

# THE GENERATED SWIFT. Swift never learns a byte layout: every struct tb_api.h
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
	  --swift $@.swift && $(call sync,$@.swift,$(GEN)/TallybonesKernel.swift)

structgen: build/layout/ios.hash

# The words: every key by name, and the English table, as Swift. The kernel
# composes every sentence (tb_api_words); these are for a host that names a
# key (tb_api_string) and for its tests.
datagen: $(DATAGEN) i18n/keys.h i18n/strings_en.c | build
	@mkdir -p build/i18n
	@$(DATAGEN) --cwd i18n --header keys.h --table TB_KEY_NAME --require-complete \
	  --name TallybonesStringKeys --swift build/i18n/TallybonesStringKeys.swift \
	  && $(call sync,build/i18n/TallybonesStringKeys.swift,$(GEN)/i18n/TallybonesStringKeys.swift)
	@$(DATAGEN) --cwd i18n --header strings_en.c --table TB_STRINGS_EN --labels TB_STRINGS_EN.0=TB_KEY_NAME \
	  --require-complete --name TallybonesStringsEn --swift build/i18n/TallybonesStringsEn.swift \
	  && $(call sync,build/i18n/TallybonesStringsEn.swift,$(GEN)/i18n/TallybonesStringsEn.swift)

# THE SWIFT SIDE, ON THIS MAC: the readers generated for this machine's
# triple, the kernel compiled for it and stamped with that layout's hash, and
# a Swift program that drives the bridge through them (ios/tb_api_smoke.swift).
# Needs swiftc and libclang, so it is not in `run`; the CI lane runs structgen
# and datagen on Linux, and this on a Mac.
SWIFT_TRIPLE = $(shell swiftc -print-target-info 2>/dev/null | sed -n 's/.*"unversionedTriple": *"\([^"]*\)".*/\1/p' | head -1)
swift-smoke: $(STRUCTGEN) datagen | build
	@rm -rf build/swift && mkdir -p build/swift/obj
	@$(STRUCTGEN) --cwd . $(SG_ARGS) --build 'host=$(strip $(IOS_CFLAGS))' --target $(SWIFT_TRIPLE) \
	  --swift build/swift/TallybonesKernel.swift --print-hash > build/swift/hash
	@for s in $(IOS_SRC); do \
	  $(CC) $(IOS_CFLAGS) -DSG_LAYOUT_HASH=$$(cat build/swift/hash) -c $$s -o build/swift/obj/$$(basename $$s .c).o || exit 1; \
	done
	swiftc -parse-as-library -import-objc-header ios/include/tb_api.h build/swift/TallybonesKernel.swift \
	  $(GEN)/i18n/*.swift ios/tb_api_smoke.swift build/swift/obj/*.o -o build/swift/smoke
	./build/swift/smoke

# The device slice, both simulator slices, the lipo and the xcframework are
# the shared recipe (lifted from uttt, REUSE_AUDIT S4).
include ../../shared/tools/ios_xcframework.mk

# THE LIBRARY CARRIES THE LAYOUT IT WAS COMPILED FOR, and the readers are
# written beside it in the same run, so the pair cannot come apart.
ios-lib: IOS_CFLAGS += -DSG_LAYOUT_HASH=$$(cat build/layout/ios.hash)
ios-lib: build/layout/ios.hash datagen
	$(call IOS_XCFRAMEWORK,tallybones,$(IOS_SRC),$(IOS_CFLAGS),ios/include,$(IOS_MIN_VERSION),../ios/vendor/Tallybones.xcframework)
	@echo "layout $$(cat build/layout/ios.hash)"

# THE SAME XCFRAMEWORK WITH A MAC CATALYST SLICE, for the Swift tests on a Mac
# with no simulator free (DECISIONS T56, T63): `ios-lib`, then an arm64 macabi
# slice of the same sources stamped with the same hash, and the xcframework
# rewritten with all three libraries. The macabi triple is checked to lay the
# structs out as the iOS slices do, or the stamp would lie. Never shipped.
CATALYST_TRIPLE := arm64-apple-ios$(IOS_MIN_VERSION)-macabi
ios-lib-catalyst: IOS_CFLAGS += -DSG_LAYOUT_HASH=$$(cat build/layout/ios.hash)
ios-lib-catalyst: ios-lib
	@h=$$($(STRUCTGEN) --cwd . $(SG_ARGS) --build 'ios=$(strip $(IOS_CFLAGS))' --target $(CATALYST_TRIPLE) --print-hash) \
	  && [ "$$h" = "$$(cat build/layout/ios.hash)" ] \
	  || { echo "layout catalyst: $$h is not the iOS slices' $$(cat build/layout/ios.hash)"; exit 1; }
	$(call IOS_XCF_SLICE,$(CATALYST_TRIPLE),macosx,build/ios/catalyst,tallybones,$(IOS_SRC),$(IOS_CFLAGS))
	@rm -rf ../ios/vendor/Tallybones.xcframework
	xcodebuild -create-xcframework \
	  -library build/ios/device/libtallybones.a -headers ios/include \
	  -library build/ios/sim/libtallybones.a -headers ios/include \
	  -library build/ios/catalyst/libtallybones.a -headers ios/include \
	  -output ../ios/vendor/Tallybones.xcframework
	@echo "wrote ../ios/vendor/Tallybones.xcframework (device, simulator, Mac Catalyst)"

clean:
	rm -rf build

.PHONY: all run asan wasm ios-smoke swift-smoke structgen datagen ios-lib ios-lib-catalyst clean FORCE
FORCE:
.SECONDARY:
