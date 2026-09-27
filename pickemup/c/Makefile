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
#   make -C pickemup/c clean
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
# Games per table size that go through the wire after every bubble. The rules
# doc's 10,000 is `./build/pk_msg_test 10000` by hand (DECISION D43).
WIRE_GAMES ?= 30

all: $(addprefix build/,$(TESTS))

build:
	@mkdir -p build

build/%: tests/%.c $(SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(SRC) $< -o $@

run: $(addprefix build/,$(TESTS)) build/ios_smoke
	./build/pk_test
	./build/pk_plan_test
	./build/pk_say_test
	./build/pk_fuzz $(FUZZ_GAMES)
	./build/pk_msg_test $(WIRE_GAMES)
	./build/ios_smoke

ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
              -Wall -Wextra -Werror -std=c11

build/asan_%: tests/%.c $(SRC) $(HDR) | build
	$(CC) $(ASAN_FLAGS) $(SRC) $< -o $@

asan: $(addprefix build/asan_,$(TESTS)) build/asan_ios_smoke
	./build/asan_pk_test
	./build/asan_pk_plan_test
	./build/asan_pk_say_test
	./build/asan_pk_fuzz 2000
	./build/asan_pk_msg_test 5 100
	./build/asan_ios_smoke

# THE SAME FILES FOR wasm32, freestanding (-nostdlib, shared/c/wasm's libc):
# the proof that nothing here reaches past memcpy / memset / memcmp / strlen /
# strncmp. Objects only; the replay page's module is linked by whoever builds
# that page.
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

# ---- the iOS bridge -----------------------------------------------------------
# ios-smoke runs the identical bridge with the host compiler, so every
# invariant on it is testable WITHOUT a Mac; ios-lib needs Xcode. The shape is
# uttt's (uttt/c/Makefile ios-lib), and the layout stamp is foolish's
# (foolish/c/Makefile build/layout/ios.hash).
IOS_MIN_VERSION := 17.0
IOS_SRC    := $(SRC) ios/pk_api.c ios/pk_lay.c
IOS_HDR    := $(HDR) ios/include/pk_api.h ios/pk_api_layout.h
IOS_CFLAGS := -O2 -Isrc -Iios -Iios/include -Wall -Wextra -Werror -std=c11

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

# $(1)=target-triple  $(2)=sdk  $(3)=out dir
define IOS_SLICE
	@rm -rf $(3)/obj
	@mkdir -p $(3)/obj
	@for s in $(IOS_SRC); do \
	  o=$(3)/obj/$$(basename $$s .c).o; \
	  echo "  CC[$(2)] $$s"; \
	  xcrun -sdk $(2) clang -target $(1) $(IOS_CFLAGS) -c $$s -o $$o || exit 1; \
	done
	libtool -static -o $(3)/libpickemup.a $(3)/obj/*.o
endef

# THE LIBRARY CARRIES THE LAYOUT IT WAS COMPILED FOR, and the readers are
# written beside it in the same run, so the pair cannot come apart.
ios-lib: IOS_CFLAGS += -DSG_LAYOUT_HASH=$$(cat build/layout/ios.hash)
ios-lib: build/layout/ios.hash datagen
	@echo "== device slice (arm64) =="
	$(call IOS_SLICE,arm64-apple-ios$(IOS_MIN_VERSION),iphoneos,build/ios/device)
	@echo "== simulator slices (arm64 + x86_64) =="
	# BOTH, because `-destination 'generic/platform=iOS Simulator'` builds both
	# and a one-arch library fails the link with "found architecture arm64,
	# required architecture x86_64" (uttt/c/Makefile).
	$(call IOS_SLICE,arm64-apple-ios$(IOS_MIN_VERSION)-simulator,iphonesimulator,build/ios/sim-arm64)
	$(call IOS_SLICE,x86_64-apple-ios$(IOS_MIN_VERSION)-simulator,iphonesimulator,build/ios/sim-x86_64)
	@mkdir -p build/ios/sim
	lipo -create build/ios/sim-arm64/libpickemup.a build/ios/sim-x86_64/libpickemup.a \
	     -output build/ios/sim/libpickemup.a
	@rm -rf ../ios/vendor/Pickemup.xcframework
	@mkdir -p ../ios/vendor
	xcodebuild -create-xcframework \
	  -library build/ios/device/libpickemup.a -headers ios/include \
	  -library build/ios/sim/libpickemup.a    -headers ios/include \
	  -output ../ios/vendor/Pickemup.xcframework
	@echo "wrote ../ios/vendor/Pickemup.xcframework (layout $$(cat build/layout/ios.hash))"

clean:
	rm -rf build

.PHONY: all run asan wasm ios-smoke swift-smoke structgen datagen ios-lib clean FORCE
FORCE:
.SECONDARY:
