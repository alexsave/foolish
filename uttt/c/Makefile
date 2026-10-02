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
# THE WORDS, one table per language (i18n/keys.h). Named, not a wildcard: a
# language whose file goes missing is a link error naming its table, where a
# wildcard would quietly compile one fewer.
LANGS   := en ru ko zh vi es pt fr de it ja pl uk tr id th nl sv da no fi cs ro he ar
I18N    := $(foreach l,$(LANGS),i18n/strings_$(l).c)
SRC     := src/uttt.c src/uttt_code.c src/uttt_bots.c src/uttt_msg.c src/uttt_say.c \
           src/uttt_pen.c src/uttt_draw.c src/uttt_anim.c src/uttt_rule.c src/uttt_lang.c \
           src/uttt_big.c src/uttt_big_msg.c $(I18N) $(SHARED)/sha256.c $(SHARED)/b32.c $(SHARED)/mixrad.c $(SHARED)/text_util/text_util.c \
           $(SHARED)/wire_check/wire_check.c

all: build/uttt_test build/uttt_msg_test build/uttt_arena build/uttt_render build/uttt_mate

build:
	@mkdir -p build

build/uttt_test: $(SRC) tests/uttt_test.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_test.c -o $@ $(LDLIBS)

# The recursive game: depth 2 against the shipped one move for move, depth 5
# played to the end, and the relaxation, undo and adopt corners by hand.
build/uttt_big_test: $(SRC) src/uttt_big.h tests/uttt_big_test.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_big_test.c -o $@ $(LDLIBS)

build/uttt_arena: $(SRC) tests/uttt_arena.c $(SHARED)/stats/stats.c $(SHARED)/stats/stats.h | build
	$(CC) $(CFLAGS) $(SRC) $(SHARED)/stats/stats.c tests/uttt_arena.c -o $@ $(LDLIBS)

build/uttt_msg_test: $(SRC) tests/uttt_msg_test.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_msg_test.c -o $@ $(LDLIBS)

# The big game's message: its bytes, board check, seats and words, and the
# kit's picture path without the JPEG. Links the kit's C (plain C, no
# framework) so its CRC and its picture are the ones held to.
BUBBLE_DATA := ../../shared/swift/BubbleDataKit/Sources/CBubbleData
build/uttt_big_msg_test: $(SRC) $(BUBBLE_DATA)/bubble_data.c tests/uttt_big_msg_test.c | build
	$(CC) $(CFLAGS) $(SRC) $(BUBBLE_DATA)/bubble_data.c tests/uttt_big_msg_test.c -o $@ $(LDLIBS)

# The shared insert-staging decisions (shared/c/msg_stage), header-only, so
# its test is the only thing to build.
MSG_STAGE := ../../shared/c/msg_stage
build/msg_stage_test: $(MSG_STAGE)/msg_stage_test.c $(MSG_STAGE)/msg_stage.h | build
	$(CC) $(CFLAGS) $(MSG_STAGE)/msg_stage_test.c -o $@

# The shared auto-collapse push (shared/c/collapse), header-only, so its test
# is the only thing to build; uttt_collapse_push forwards to it.
COLLAPSE := $(SHARED)/collapse
build/collapse_test: $(COLLAPSE)/collapse_test.c $(COLLAPSE)/collapse.h | build
	$(CC) $(CFLAGS) $(COLLAPSE)/collapse_test.c -o $@ -lm

# The shared text writers (shared/c/text_util): the stepping, the copy and the
# filler this kernel uses, and the column table it does not.
TEXT_UTIL := $(SHARED)/text_util
build/text_util_test: $(TEXT_UTIL)/text_util.c $(TEXT_UTIL)/text_util_test.c $(TEXT_UTIL)/text_util.h | build
	$(CC) $(CFLAGS) $(TEXT_UTIL)/text_util.c $(TEXT_UTIL)/text_util_test.c -o $@

# The shared wire check (shared/c/wire_check): the envelope's truncated
# SHA-256 over head and body, which uttt_msg.c carries UTM_CHECK_LEN bytes of.
WIRE_CHECK := $(SHARED)/wire_check
WIRE_CHECK_SRC := $(SHARED)/sha256.c $(WIRE_CHECK)/wire_check.c $(WIRE_CHECK)/wire_check_test.c
build/wire_check_test: $(WIRE_CHECK_SRC) $(WIRE_CHECK)/wire_check.h $(SHARED)/sha256.h | build
	$(CC) $(CFLAGS) $(WIRE_CHECK_SRC) -o $@

# The shared arena statistics (shared/c/stats): mean and standard error, the
# Wilson interval, and the per-game seed hash.
STATS := $(SHARED)/stats
STATS_SRC := $(STATS)/stats.c $(STATS)/stats_test.c
build/stats_test: $(STATS_SRC) $(STATS)/stats.h $(STATS)/seed_hash.h | build
	$(CC) $(CFLAGS) $(STATS_SRC) -o $@ -lm

# The message: every ply of 10,000 games through bytes and text, the seat
# truth table, two joiners, the tap map and every sentence.
run: build/uttt_test build/uttt_big_test build/uttt_msg_test build/uttt_big_msg_test build/uttt_anim_test build/msg_stage_test build/collapse_test build/text_util_test build/wire_check_test build/stats_test build/uttt_bots_test build/uttt_ink_test build/uttt_lang_test build/uttt_lang_en_test
	./build/uttt_test 1000
	./build/uttt_big_test 2000 10
	./build/uttt_lang_test
	./build/uttt_lang_en_test
	./build/uttt_bots_test 10
	./build/uttt_msg_test 10000
	./build/uttt_big_msg_test 20
	./build/msg_stage_test
	./build/collapse_test
	./build/text_util_test
	./build/wire_check_test
	./build/stats_test
	./build/uttt_anim_test
	./build/uttt_ink_test

# The ink: one polygon per stroke, the pen's shape with no hole, the alpha.
build/uttt_ink_test: $(SRC) tests/uttt_ink_test.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_ink_test.c -o $@ $(LDLIBS)

# The words: every language fills every key, keeps English's placeholders,
# fits each key's width, and every caption it can compose fits one line.
build/uttt_lang_test: $(SRC) i18n/keys.h tests/uttt_lang_test.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_lang_test.c -o $@ $(LDLIBS)

# ...and the replay page's English-only kernel falls back rather than blanks.
SRC_EN := $(filter-out $(I18N),$(SRC)) i18n/strings_en.c
build/uttt_lang_en_test: $(SRC_EN) i18n/keys.h tests/uttt_lang_test.c | build
	$(CC) $(CFLAGS) -DUTTT_LANG_ENGLISH_ONLY $(SRC_EN) tests/uttt_lang_test.c -o $@ $(LDLIBS)

build/uttt_bots_test: $(SRC) tests/uttt_bots_test.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_bots_test.c -o $@ $(LDLIBS)

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

# THE STORE GAME: the one real game every App Store frame is a phase of
# (docs/STORE_SHOTS.md). Plays its fixed opening through the rules, finishes
# it with fountain and prints the ply list and the replay link.
build/uttt_store_game: $(SRC) tools/uttt_store_game.c | build
	$(CC) $(CFLAGS) $(SRC) tools/uttt_store_game.c -o $@ $(LDLIBS)

store-game: build/uttt_store_game
	./build/uttt_store_game 44 5 2000

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

# THE POST-GAME ANALYSER, not part of the app: every ply of a replay code
# held against quill. `make analyse CODE=<code or link> PLAYOUTS=20000`.
ANALYSE_SRC := src/uttt_analyse.c
build/uttt_analyse: $(SRC) $(ANALYSE_SRC) src/uttt_analyse.h tools/uttt_analyse.c | build
	$(CC) $(CFLAGS) $(SRC) $(ANALYSE_SRC) tools/uttt_analyse.c -o $@ $(LDLIBS)

build/uttt_analyse_test: $(SRC) $(ANALYSE_SRC) src/uttt_analyse.h tests/uttt_analyse_test.c | build
	$(CC) $(CFLAGS) $(SRC) $(ANALYSE_SRC) tests/uttt_analyse_test.c -o $@ $(LDLIBS)

PLAYOUTS ?= 20000
analyse: build/uttt_analyse
	@if [ -n "$(CODE)" ]; then ./build/uttt_analyse "$(CODE)" $(PLAYOUTS); fi

# A blunder that throws a forced win is called one, and two runs agree.
analyse-test: build/uttt_analyse_test
	./build/uttt_analyse_test

# The pen against the pen it was copied from. Needs node; not in `all`.
rough-diff: build/uttt_render
	node tests/rough_diff.mjs

asan: $(SRC) tests/uttt_test.c | build
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(SRC) tests/uttt_test.c -o build/uttt_asan $(LDLIBS)
	./build/uttt_asan 400
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(SRC) tests/uttt_big_test.c -o build/uttt_big_asan $(LDLIBS)
	./build/uttt_big_asan 300 1
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(SRC) tests/uttt_msg_test.c -o build/uttt_msg_asan $(LDLIBS)
	./build/uttt_msg_asan 1000
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(SRC) $(BUBBLE_DATA)/bubble_data.c tests/uttt_big_msg_test.c -o build/uttt_big_msg_asan $(LDLIBS)
	./build/uttt_big_msg_asan 3
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(SRC) tests/uttt_anim_test.c -o build/uttt_anim_asan $(LDLIBS)
	./build/uttt_anim_asan
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(SRC) tests/uttt_bots_test.c -o build/uttt_bots_asan $(LDLIBS)
	./build/uttt_bots_asan 3
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(SRC) tests/uttt_ink_test.c -o build/uttt_ink_asan $(LDLIBS)
	./build/uttt_ink_asan
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(SRC) tests/uttt_lang_test.c -o build/uttt_lang_asan $(LDLIBS)
	./build/uttt_lang_asan
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(TEXT_UTIL)/text_util.c $(TEXT_UTIL)/text_util_test.c -o build/text_util_asan
	./build/text_util_asan
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(WIRE_CHECK_SRC) -o build/wire_check_asan
	./build/wire_check_asan
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(STATS_SRC) -o build/stats_asan -lm
	./build/stats_asan
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(COLLAPSE)/collapse_test.c -o build/collapse_asan -lm
	./build/collapse_asan

clean:
	rm -rf build

.PHONY: all run arena play render look analyse analyse-test rough-diff asan clean

# ---- the iOS bridge -----------------------------------------------------
# `make ios-smoke` exercises the identical bridge with the host compiler, so
# every invariant on it is testable WITHOUT a Mac. ios-lib needs Xcode.
IOS_MIN_VERSION := 17.0
IOS_SRC    := $(SRC) ios/uttt_api.c
IOS_CFLAGS := -O2 -Isrc -Iios/include -Wall -Wextra -Werror -std=c11

# The device slice, both simulator slices, the lipo and the xcframework are
# the shared recipe.
include ../../shared/tools/ios_xcframework.mk

ios-lib:
	$(call IOS_XCFRAMEWORK,uttt,$(IOS_SRC),$(IOS_CFLAGS),ios/include,$(IOS_MIN_VERSION),../ios/vendor/Uttt.xcframework)


build/ios_smoke: $(IOS_SRC) ios/uttt_api_smoke.c | build
	$(CC) $(CFLAGS) -Iios/include $(IOS_SRC) ios/uttt_api_smoke.c -o $@ $(LDLIBS)

ios-smoke: build/ios_smoke
	./build/ios_smoke

.PHONY: ios-smoke ios-lib

# THE BIG GAME THROUGH A REAL SEND'S PICTURE CHAIN: one random depth-5 game
# to the end, every ply framed and painted by the kit's C, JPEG'd at q0.50
# and q0.89 with ImageIO (both 4:2:0), read back and decoded. macOS only
# (ImageIO), so NOT in run/asan/all: CI is Linux. ARGS="--plies N", "--every
# K", "--px 3" (robust243).
ifeq ($(shell uname),Darwin)
build/uttt_big_chain: $(SRC) $(BUBBLE_DATA)/bubble_data.c tests/uttt_big_chain.c | build
	$(CC) $(CFLAGS) $(SRC) $(BUBBLE_DATA)/bubble_data.c tests/uttt_big_chain.c -o $@ $(LDLIBS) \
	  -framework ImageIO -framework CoreGraphics -framework CoreFoundation

big-chain: build/uttt_big_chain
	./build/uttt_big_chain $(ARGS)

.PHONY: big-chain
endif

build/uttt_ladder: $(SRC) tests/uttt_ladder.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_ladder.c -o $@ $(LDLIBS)
ladder: build/uttt_ladder
	./build/uttt_ladder 40 8

# ---- the replay page (uttt.live) ------------------------------------------
# The drawing kernel for the browser: uttt/web's replay fills the same display
# list the app does (wasm/uttt_web.c). Freestanding - no libc; the few libc
# and libm calls come from shared/c/wasm - and only the files a replay draws
# with.
# A BUILD OUTPUT, never committed (uttt/web/.gitignore): the uttt-web workflow
# builds it before it builds the site.
# shared/scripts/ci_llvm.sh exports WASM_CC, the repo's pinned clang; locally any
# clang with the wasm32 target and wasm-ld will do.
WEB_WASM_CC  ?= $(if $(WASM_CC),$(WASM_CC),clang)
# ENGLISH ONLY (UTTT_LANG_ENGLISH_ONLY, src/uttt_lang.c): the page is English
# and every visitor would download the other twenty-four tables.
WEB_WASM_SRC := src/uttt.c src/uttt_code.c src/uttt_pen.c src/uttt_draw.c \
                src/uttt_anim.c src/uttt_rule.c src/uttt_say.c src/uttt_lang.c \
                i18n/strings_en.c ../../shared/c/b32.c ../../shared/c/mixrad.c \
                ../../shared/c/text_util/text_util.c \
                ../../shared/c/wasm/libc.c ../../shared/c/wasm/libm.c wasm/uttt_web.c
WEB_WASM_OUT := ../web/public/uttt.wasm

$(WEB_WASM_OUT): $(WEB_WASM_SRC) $(wildcard src/*.h i18n/*.h ../../shared/c/wasm/include/*.h ../../shared/c/text_util/*.h ../../shared/c/collapse/*.h) Makefile
	@mkdir -p $(dir $@)
	$(WEB_WASM_CC) --target=wasm32 -Oz -nostdlib -ffreestanding -mbulk-memory \
	  -mno-nontrapping-fptoint -std=c11 -Wall -Wextra -Werror -DUTTT_LANG_ENGLISH_ONLY \
	  -isystem ../../shared/c/wasm/include -Isrc \
	  -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=131072 -Wl,--strip-all \
	  $(WEB_WASM_SRC) -o $@
	@echo "built $@ ($$(wc -c < $@) B, $$(gzip -9 -c $@ | wc -c) B gz)"

wasm-web: $(WEB_WASM_OUT)

.PHONY: wasm-web

# ---- shared/c's own tests ---------------------------------------------------
# The tests beside shared/c's primitives, run as part of `make run` (this
# kernel links b32, mixrad and the wasm libc; sha256 and deal_rng have no
# other home that runs every shared test in one place). Self-contained: each
# test builds from its own file and the one it covers, so no SRC list changes.
SHARED_C := ../../shared/c
SHARED_TESTS := build/libc_test build/b32_test build/mixrad_test build/sha256_test build/deal_rng_test \
                build/le_bytes_test

build/libc_test: $(SHARED_C)/wasm/libc_test.c $(SHARED_C)/wasm/libc.c $(SHARED_C)/test/check.h | build
	$(CC) $(CFLAGS) $(SHARED_C)/wasm/libc_test.c -o $@

# Header-only, so the pattern rule below (which wants a .c beside the test)
# does not match it.
build/le_bytes_test: $(SHARED_C)/le_bytes_test.c $(SHARED_C)/le_bytes.h $(SHARED_C)/test/check.h | build
	$(CC) $(CFLAGS) $(SHARED_C)/le_bytes_test.c -o $@

build/%_test: $(SHARED_C)/%_test.c $(SHARED_C)/%.c $(SHARED_C)/%.h $(SHARED_C)/test/check.h | build
	$(CC) $(CFLAGS) $(SHARED_C)/$*_test.c $(SHARED_C)/$*.c -o $@

shared-tests: $(SHARED_TESTS)
	@for t in $(SHARED_TESTS); do ./$$t || exit 1; done

run: shared-tests

.PHONY: shared-tests
