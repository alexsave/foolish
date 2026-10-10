CC      ?= clang

# -Werror because this kernel has no second implementation to cross-check it
# against. The fork could catch a mistake here by disagreeing with its wasm
# build; until this one has two engines, the compiler is the only other reader.

# The two primitives are no longer inherited COPIES - they are the monorepo's
# one copy, compiled from ../../shared/c by this product and by foolish alike.
# Kept as a variable rather than spelled at each use so the depth of the path
# appears once.
#
# There is no -I for them and there must not be: the files that include them say
# so relatively ("../../../shared/c/deal_rng.h"), which resolves against the
# including file and therefore needs nothing from any build system. This kernel
# is compiled by five of them.
SHARED  := ../../shared/c

CFLAGS  ?= -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Isrc
LDFLAGS ?=

# The kernel. Two shared primitives (deal_rng, sha256) and five werewolf files,
# and that is the whole domain: roles from a seed, the night, the envelope those
# cross the thread in, which seat this device is on a bubble's roster, and the
# lobby that decides when a seed becomes roles.
CORE_SRC := $(SHARED)/deal_rng.c $(SHARED)/sha256.c \
            src/ww_game.c src/ww_view.c src/ww_wire.c src/ww_seat.c \
            src/ww_lobby.c

.PHONY: all tests tests-asan clean print-core ios-lib ios-smoke
all: build/ww_tests

print-core:
	@echo $(CORE_SRC)

# HOW A FILE BECOMES AN OBJECT is shared/tools/c_rules.mk: one directory of
# objects per compiler-and-flags (a "variant"), each object depending on
# exactly the headers its compile opened and on the command line it was built
# with. Three variants here - the kernel as tested, the same under the
# sanitizers, and the bridge's flags on the host compiler.
include ../../shared/tools/c_rules.mk
$(eval $(call C_VARIANT,build/obj/host,CC,CFLAGS))
$(eval $(call C_VARIANT,build/obj/asan,CC,ASAN_CFLAGS))
$(eval $(call C_VARIANT,build/obj/ioshost,CC,IOS_CFLAGS))

build/ww_tests: $(call c_objs,build/obj/host,tests/tests.c $(CORE_SRC))
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# ONE command is the gate, and it runs the bridge too. The bridge is a second
# reader of the masking, so a leak that only shows up through the flat accessors
# has to be caught by the same `make tests` everything else is judged by -
# a second target is a target somebody forgets.
tests: build/ww_tests build/ios_smoke
	./build/ww_tests
	./build/ios_smoke

# The same suite under ASan+UBSan. Its own objects, in its own variant: the
# sanitizers change codegen, and mixing is how a clean run comes from a stale
# object.
ASAN_CFLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
               -Wall -Wextra -Werror -Wno-unused-parameter -Isrc
build/ww_tests_asan: $(call c_objs,build/obj/asan,$(CORE_SRC) tests/tests.c)
	$(CC) $(ASAN_CFLAGS) $^ -o $@
tests-asan: build/ww_tests_asan
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

# The device slice, both simulator slices, the lipo and the xcframework are
# the shared recipe, and it rebuilds only what changed - which is what lets
# Xcode run this target on every build (shared/scripts/xcode_kernel.sh).
include ../../shared/tools/ios_xcframework.mk
$(eval $(call IOS_XCFRAMEWORK,werewolf,$(IOS_SRC),ios/include,../ios/vendor/Werewolf.xcframework))
ios-lib: $(IOS_XCFRAMEWORKS)

# The bridge, built and run WITHOUT a Mac: the portable proof that
# deal -> night -> view -> seal -> replay runs through the Swift entry points.
build/ios_smoke: $(call c_objs,build/obj/ioshost,$(IOS_SRC) ios/ios_api_smoke.c)
	$(CC) $(IOS_CFLAGS) $^ -o $@

ios-smoke: build/ios_smoke
	./build/ios_smoke

clean:
	rm -rf build
