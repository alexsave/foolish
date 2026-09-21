CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -std=c11
LDLIBS  := -lm
SRC     := src/uttt.c src/uttt_code.c src/uttt_bots.c \
           src/uttt_pen.c src/uttt_draw.c src/uttt_rule.c

all: build/uttt_test build/uttt_arena build/uttt_render build/uttt_mate

build:
	@mkdir -p build

build/uttt_test: $(SRC) tests/uttt_test.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_test.c -o $@ $(LDLIBS)

build/uttt_arena: $(SRC) tests/uttt_arena.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_arena.c -o $@ $(LDLIBS)

run: build/uttt_test
	./build/uttt_test 1000

arena: build/uttt_arena
	./build/uttt_arena 200 200

build/uttt_mate: $(SRC) tests/uttt_mate.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_mate.c -o $@ $(LDLIBS)

# The mate search against an exhaustive one. Both the verdict and the
# distance, on positions deep enough to tell a min from a max.
mate: build/uttt_mate
	./build/uttt_mate 2000

build/uttt_icon: $(SRC) tools/uttt_icon.c | build
	$(CC) $(CFLAGS) $(SRC) tools/uttt_icon.c -o $@ $(LDLIBS)

# THE ICONS THE APP SHIPS, drawn by the app's own pen rather than traced by
# hand. Writes straight into both asset catalogues; commit what it changes.
icons: build/uttt_icon
	./tools/icons.sh

build/uttt_render: $(SRC) tests/uttt_render.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_render.c -o $@ $(LDLIBS)

render: build/uttt_render
	./build/uttt_render $(PLIES)
	@sips -s format png build/board.ppm --out build/board.png >/dev/null \
	  && echo "build/board.png"


# The pen against the pen it was copied from. Needs node; not in `all`.
rough-diff: build/uttt_render
	node tests/rough_diff.mjs

asan: $(SRC) tests/uttt_test.c | build
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(SRC) tests/uttt_test.c -o build/uttt_asan $(LDLIBS)
	./build/uttt_asan 400

clean:
	rm -rf build

.PHONY: all run arena render rough-diff asan clean

# ---- the iOS bridge -----------------------------------------------------
# `make ios-smoke` exercises the identical bridge with the host compiler, so
# every invariant on it is testable WITHOUT a Mac. ios-lib needs Xcode.
IOS_MIN_VERSION := 17.0
IOS_SRC    := $(SRC) ios/uttt_api.c
IOS_CFLAGS := -O2 -Isrc -Iios/include -Wall -Wextra -Werror -std=c11

# $(1)=target-triple  $(2)=sdk  $(3)=out dir
define IOS_SLICE
	@rm -rf $(3)/obj
	@mkdir -p $(3)/obj
	@for s in $(IOS_SRC); do \
	  o=$(3)/obj/$$(basename $$s .c).o; \
	  echo "  CC[$(2)] $$s"; \
	  xcrun -sdk $(2) clang -target $(1) $(IOS_CFLAGS) -c $$s -o $$o || exit 1; \
	done
	libtool -static -o $(3)/libuttt.a $(3)/obj/*.o
endef

ios-lib:
	@echo "== device slice (arm64) =="
	$(call IOS_SLICE,arm64-apple-ios$(IOS_MIN_VERSION),iphoneos,build/ios/device)
	@echo "== simulator slices (arm64 + x86_64) =="
	# BOTH, because `-destination 'generic/platform=iOS Simulator'` builds both
	# and a one-arch library fails the link with "ignoring file ... found
	# architecture arm64, required architecture x86_64" - which reads like a
	# missing symbol and is not.
	$(call IOS_SLICE,arm64-apple-ios$(IOS_MIN_VERSION)-simulator,iphonesimulator,build/ios/sim-arm64)
	$(call IOS_SLICE,x86_64-apple-ios$(IOS_MIN_VERSION)-simulator,iphonesimulator,build/ios/sim-x86_64)
	@mkdir -p build/ios/sim
	lipo -create build/ios/sim-arm64/libuttt.a build/ios/sim-x86_64/libuttt.a \
	     -output build/ios/sim/libuttt.a
	@rm -rf ../ios/vendor/Uttt.xcframework
	@mkdir -p ../ios/vendor
	xcodebuild -create-xcframework \
	  -library build/ios/device/libuttt.a -headers ios/include \
	  -library build/ios/sim/libuttt.a    -headers ios/include \
	  -output ../ios/vendor/Uttt.xcframework
	@echo "wrote ../ios/vendor/Uttt.xcframework"


build/ios_smoke: $(IOS_SRC) ios/uttt_api_smoke.c | build
	$(CC) $(CFLAGS) -Iios/include $(IOS_SRC) ios/uttt_api_smoke.c -o $@ $(LDLIBS)

ios-smoke: build/ios_smoke
	./build/ios_smoke

.PHONY: ios-smoke ios-lib
