CC      ?= clang

# -Werror because this kernel has no second implementation to cross-check it
# against. The fork could catch a mistake here by disagreeing with its wasm
# build; until this one has two engines, the compiler is the only other reader.
CFLAGS  ?= -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Isrc
LDFLAGS ?=

# The kernel. Two inherited primitives (deal_rng, sha256) and three werewolf
# files, and that is the whole domain: roles from a seed, the night, and the
# envelope those cross the thread in.
CORE_SRC := src/deal_rng.c src/sha256.c \
            src/ww_game.c src/ww_view.c src/ww_wire.c
CORE_OBJ := $(CORE_SRC:src/%.c=build/%.o)

.PHONY: all tests tests-asan clean print-core ios-lib ios-smoke
all: build/ww_tests

print-core:
	@echo $(CORE_SRC)

build/%.o: src/%.c $(wildcard src/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/ww_tests: $(CORE_OBJ) tests/tests.c $(wildcard src/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) tests/tests.c $(CORE_OBJ) -o $@ $(LDFLAGS)

tests: build/ww_tests
	./build/ww_tests

# The same suite under ASan+UBSan. Its own binary from source rather than the
# objects above, because the sanitizers change codegen and mixing is how a clean
# run comes from a stale object.
ASAN_CFLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
               -Wall -Wextra -Werror -Wno-unused-parameter -Isrc
tests-asan:
	@mkdir -p build
	$(CC) $(ASAN_CFLAGS) $(CORE_SRC) tests/tests.c -o build/ww_tests_asan
	./build/ww_tests_asan

# ---------- iOS static library / xcframework -------------------------------
#
# ios/vendor/Werewolf.xcframework is built from the SAME CORE_SRC the test
# binary uses, plus the Swift-visible bridge. ONE archive, not the fork's two:
# the split there existed to keep the bot strategy ladder out of the extension's
# dead-strip roots, and there are no bots here.
#
# macOS + Xcode only. `make ios-smoke` exercises the identical bridge with the
# host compiler, so the bridge's invariants are testable without a Mac.
IOS_MIN_VERSION := 15.0
IOS_BRIDGE_SRC  := ios/ios_api.c
IOS_SRC         := $(CORE_SRC) $(IOS_BRIDGE_SRC)
IOS_CFLAGS      := -O2 -Isrc -Iios/include -Wall -Wextra -Werror -Wno-unused-parameter

# $(1)=target-triple  $(2)=sdk name  $(3)=output dir
define IOS_SLICE
	@rm -rf $(3)/obj
	@mkdir -p $(3)/obj
	@for s in $(IOS_SRC); do \
	  o=$(3)/obj/$$(basename $$s .c).o; \
	  echo "  CC[$(2)] $$s"; \
	  xcrun -sdk $(2) clang -target $(1) $(IOS_CFLAGS) -c $$s -o $$o || exit 1; \
	done
	libtool -static -o $(3)/libwerewolf.a $(3)/obj/*.o
endef

ios-lib:
	@echo "== device slice (arm64) =="
	$(call IOS_SLICE,arm64-apple-ios$(IOS_MIN_VERSION),iphoneos,build/ios/device)
	@echo "== simulator slices (arm64 + x86_64) =="
	$(call IOS_SLICE,arm64-apple-ios$(IOS_MIN_VERSION)-simulator,iphonesimulator,build/ios/sim-arm64)
	$(call IOS_SLICE,x86_64-apple-ios$(IOS_MIN_VERSION)-simulator,iphonesimulator,build/ios/sim-x86_64)
	@mkdir -p build/ios/sim
	lipo -create build/ios/sim-arm64/libwerewolf.a build/ios/sim-x86_64/libwerewolf.a \
	     -output build/ios/sim/libwerewolf.a
	@rm -rf ../ios/vendor/Werewolf.xcframework
	@mkdir -p ../ios/vendor
	xcodebuild -create-xcframework \
	  -library build/ios/device/libwerewolf.a -headers ios/include \
	  -library build/ios/sim/libwerewolf.a    -headers ios/include \
	  -output ../ios/vendor/Werewolf.xcframework
	@echo "wrote ../ios/vendor/Werewolf.xcframework"

# The bridge, built and run WITHOUT a Mac: the portable proof that
# deal -> night -> view -> seal -> replay runs through the Swift entry points.
ios-smoke:
	@mkdir -p build
	$(CC) $(IOS_CFLAGS) $(IOS_SRC) ios/ios_api_smoke.c -o build/ios_smoke
	./build/ios_smoke

clean:
	rm -rf build
