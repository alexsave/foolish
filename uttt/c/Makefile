CC      ?= cc
# -O3 AND LINK-TIME OPTIMISATION, and it is the -flto that does the work.
# Measured on quill at 40 rollouts, three runs each, idle machine:
#
#     -O2                                   1.807 ms a move
#     -Os                                   1.807
#     -O3                                   1.670
#     -O3 -mcpu=native                      1.670    (nothing)
#     -O3 -funroll-loops                    1.670    (nothing)
#     -O3 -ffast-math                       1.660    (nothing, and see below)
#     -O3 -flto                             1.483    <- 1.22x over -O2
#     -Ofast -mcpu=native -flto             1.470
#
# The whole win is cross-file inlining: the hot functions live in uttt.c and
# their callers in uttt_bots.c, so without -flto the compiler cannot see a
# table lookup on the other side of a call.
#
# NO FAST MATH. It is worth 0.9% and this codebase cannot pay for it: the pen
# is a transcription of rough.js and `make rough-diff` holds it to six
# decimal places, because two phones looking at one bubble have to produce
# the same sheet stroke for stroke. Reassociating float arithmetic is exactly
# how that stops being true, quietly, on one compiler and not another.
CFLAGS  ?= -O3 -flto -Wall -Wextra -Werror -std=c11
LDLIBS  := -lm
SHARED  := ../../shared/c
SRC     := src/uttt.c src/uttt_code.c src/uttt_bots.c src/uttt_msg.c src/uttt_say.c \
           src/uttt_pen.c src/uttt_draw.c src/uttt_anim.c src/uttt_rule.c \
           $(SHARED)/sha256.c $(SHARED)/b32.c

all: build/uttt_test build/uttt_msg_test build/uttt_arena build/uttt_render build/uttt_mate

build:
	@mkdir -p build

build/uttt_test: $(SRC) tests/uttt_test.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_test.c -o $@ $(LDLIBS)

build/uttt_arena: $(SRC) tests/uttt_arena.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_arena.c -o $@ $(LDLIBS)

build/uttt_msg_test: $(SRC) tests/uttt_msg_test.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_msg_test.c -o $@ $(LDLIBS)

# The shared insert-staging decisions (shared/c/msg_stage), header-only, so
# its test is the only thing to build.
MSG_STAGE := ../../shared/c/msg_stage
build/msg_stage_test: $(MSG_STAGE)/msg_stage_test.c $(MSG_STAGE)/msg_stage.h | build
	$(CC) $(CFLAGS) $(MSG_STAGE)/msg_stage_test.c -o $@

# The message: every ply of 10,000 games through bytes and text, the seat
# truth table, two joiners, the tap map and every sentence.
run: build/uttt_test build/uttt_msg_test build/uttt_anim_test build/msg_stage_test
	./build/uttt_test 1000
	./build/uttt_msg_test 10000
	./build/msg_stage_test
	./build/uttt_anim_test

build/uttt_anim_test: $(SRC) tests/uttt_anim_test.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_anim_test.c -o $@ $(LDLIBS)

# The board's side per drawer height, for the ruler's scorer (tools/uttt_side.c).
build/uttt_side: $(SRC) tools/uttt_side.c | build
	$(CC) $(CFLAGS) -Isrc $(SRC) tools/uttt_side.c -o $@ $(LDLIBS)

arena: build/uttt_arena
	./build/uttt_arena 200 200

build/uttt_line: $(SRC) tests/uttt_line.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_line.c -o $@ $(LDLIBS)

# The 62% hotspot against the version it replaced, over every board there is.
line: build/uttt_line
	./build/uttt_line

build/uttt_mate: $(SRC) tests/uttt_mate.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_mate.c -o $@ $(LDLIBS)

# The mate search against an exhaustive one. Both the verdict and the
# distance, on positions deep enough to tell a min from a max. And quill's
# tree proofs against the same solver: it may say nothing, never something
# false.
mate: build/uttt_mate
	./build/uttt_mate 2000

build/uttt_icon: $(SRC) tools/uttt_icon.c | build
	$(CC) $(CFLAGS) $(SRC) tools/uttt_icon.c -o $@ $(LDLIBS)

# THE ICONS THE APP SHIPS, drawn by the app's own pen rather than traced by
# hand. Writes straight into both asset catalogues; commit what it changes.
icons: build/uttt_icon
	./tools/icons.sh

build/uttt_play: $(SRC) tools/uttt_play.c | build
	$(CC) $(CFLAGS) $(SRC) tools/uttt_play.c -o $@ $(LDLIBS)

# PLAY THE THING. quill, first move, as hard as it gets without waiting.
ROLLOUTS ?= 400
play: build/uttt_play
	./build/uttt_play $(ROLLOUTS)

build/uttt_render: $(SRC) tests/uttt_render.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_render.c -o $@ $(LDLIBS)

render: build/uttt_render
	./build/uttt_render $(PLIES)
	@sips -s format png build/board.ppm --out build/board.png >/dev/null \
	  && echo "build/board.png"


build/uttt_look: $(SRC) tools/uttt_look.c | build
	$(CC) $(CFLAGS) $(SRC) tools/uttt_look.c -o $@ $(LDLIBS)

# The pieces the owner reviews, at a phone's scale, with numbers where a
# number settles it (the door's edge thickness). See tools/uttt_look.c.
look: build/uttt_look

# The pen against the pen it was copied from. Needs node; not in `all`.
rough-diff: build/uttt_render
	node tests/rough_diff.mjs

asan: $(SRC) tests/uttt_test.c | build
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(SRC) tests/uttt_test.c -o build/uttt_asan $(LDLIBS)
	./build/uttt_asan 400
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(SRC) tests/uttt_msg_test.c -o build/uttt_msg_asan $(LDLIBS)
	./build/uttt_msg_asan 1000
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(SRC) tests/uttt_anim_test.c -o build/uttt_anim_asan $(LDLIBS)
	./build/uttt_anim_asan

clean:
	rm -rf build

.PHONY: all run arena play render look rough-diff asan clean

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
