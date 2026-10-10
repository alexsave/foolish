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
           src/uttt_big.c src/uttt_big_msg.c src/uttt_big_diag.c $(I18N) $(SHARED)/sha256.c $(SHARED)/b32.c $(SHARED)/mixrad.c $(SHARED)/text_util/text_util.c \
           $(SHARED)/wire_check/wire_check.c

all: build/uttt_test build/uttt_msg_test build/uttt_arena build/uttt_render build/uttt_mate

# ---- how a file becomes an object, and a program ---------------------------
# shared/tools/c_rules.mk: one directory of objects per compiler-and-flags (a
# "variant"), each object depending on exactly the headers its compile opened
# and on the command line it was built with.
#
# THIS FILE USED TO NAME ITS HEADERS BY HAND, per program, and compile every
# program whole in one command. The lists were wrong in the way such lists
# are: a number that lived only in a header (UTB_HOLD_MS) changed and the
# smoke ran the old binary, green, before the headers were listed. Nothing is
# listed now. A program is its objects; an object knows what it included.
#
# -flto STILL DOES ITS WORK: with it an object is the compiler's bitcode and
# the link is where the cross-file inlining happens, which is what one command
# over every source was doing.
#
# ONE EXTRA FLAG FOR ONE OBJECT (OBJ_FLAGS_<source without .c>, which is $* in
# the compile): the bridge and its smoke need ios/include, a tool needs src.
# Said for the file that needs it, so the kernel is compiled once and not once
# more per include path.
ASAN_FLAGS  := -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11
HOST_FLAGS   = $(CFLAGS) $(OBJ_FLAGS_$*)
SAN_FLAGS    = $(ASAN_FLAGS) $(OBJ_FLAGS_$*)
OBJ_FLAGS_tools/uttt_side    := -Isrc
OBJ_FLAGS_ios/uttt_api       := -Iios/include
OBJ_FLAGS_ios/uttt_api_smoke := -Iios/include
# ...and the replay page's English-only kernel (UTTT_LANG_ENGLISH_ONLY), a
# different compile of the same files.
EN_FLAGS     = $(CFLAGS) -DUTTT_LANG_ENGLISH_ONLY

# The shared/c tests this kernel runs: what it links (b32, mixrad, sha256, the
# wasm libc, text_util, wire_check, stats) and the header-only pieces its app
# calls (msg_stage, collapse, le_bytes); deal_rng has no other home that runs
# every shared test in one place. What each is built from is
# shared/tools/shared_tests.mk's.
SHARED_TESTS      := msg_stage_test collapse_test text_util_test wire_check_test stats_test \
                     libc_test b32_test mixrad_test sha256_test deal_rng_test le_bytes_test
SHARED_ASAN_TESTS := text_util_test wire_check_test stats_test collapse_test

include ../../shared/tools/c_rules.mk
include ../../shared/tools/shared_tests.mk
$(eval $(call C_VARIANT,build/obj/host,CC,HOST_FLAGS))
$(eval $(call C_VARIANT,build/obj/asan,CC,SAN_FLAGS))
$(eval $(call C_VARIANT,build/obj/en,CC,EN_FLAGS))
$(call SHARED_TESTS_IN,,build/obj/host,CC,CFLAGS)
$(call SHARED_TESTS_IN,asan_,build/obj/asan,CC,ASAN_FLAGS)

# $(call prog,<name>,<what it is besides the kernel>,<more to link>): the
# program build/<name>, and its sanitized twin build/asan_<name>.
define PROG
build/$(1): $$(call c_objs,build/obj/host,$$(SRC) $(2))
	$$(CC) $$(CFLAGS) $$^ -o $$@ $$(LDLIBS) $(3)
build/asan_$(1): $$(call c_objs,build/obj/asan,$$(SRC) $(2))
	$$(CC) $$(ASAN_FLAGS) $$^ -o $$@ $$(LDLIBS) $(3)
endef
prog = $(eval $(call PROG,$(1),$(2),$(3)))

$(call prog,uttt_test,tests/uttt_test.c)

# The recursive game: depth 2 against the shipped one move for move, depth 5
# played to the end, and the relaxation, undo and adopt corners by hand.
$(call prog,uttt_big_test,tests/uttt_big_test.c)

# The 243 page's bot (src/uttt_big_bot.h): the weights on hand-built
# positions, a game as its seed, a player against random play, and a whole
# depth-5 game at the page's settings, timed. NOT in SRC: no app links it.
BIG_BOT_SRC := src/uttt_big_bot.c
$(call prog,uttt_big_bot_test,$(BIG_BOT_SRC) tests/uttt_big_bot_test.c)

$(call prog,uttt_arena,$(SHARED)/stats/stats.c tests/uttt_arena.c)

$(call prog,uttt_msg_test,tests/uttt_msg_test.c)

# The big game's message: its bytes, board check, seats and words, and the
# kit's picture path without the JPEG. Links the kit's C (plain C, no
# framework) so its CRC and its picture are the ones held to.
BUBBLE_DATA := ../../shared/swift/BubbleDataKit/Sources/CBubbleData
$(call prog,uttt_big_msg_test,$(BUBBLE_DATA)/bubble_data.c tests/uttt_big_msg_test.c)

# The 243 diagnostics (src/uttt_big_diag.h): the JPEG facts against real
# files (tests/fixtures), the greys against the kit's own bd_sample, the
# history ring's layout and the report's lines. Links the kit's C, as above.
$(call prog,uttt_big_diag_test,$(BUBBLE_DATA)/bubble_data.c tests/uttt_big_diag_test.c)

# The message: every ply of 10,000 games through bytes and text, the seat
# truth table, two joiners, the tap map and every sentence.
run: build/uttt_test build/uttt_big_test build/uttt_msg_test build/uttt_big_msg_test build/uttt_big_diag_test build/uttt_anim_test build/uttt_bots_test build/uttt_ink_test build/uttt_lang_test build/uttt_lang_en_test build/uttt_big_bot_test
	./build/uttt_test 1000
	./build/uttt_big_test 2000 10
	./build/uttt_big_bot_test 40
	./build/uttt_lang_test
	./build/uttt_lang_en_test
	./build/uttt_bots_test 10
	./build/uttt_msg_test 10000
	./build/uttt_big_msg_test 20
	./build/uttt_big_diag_test
	./build/uttt_anim_test
	./build/uttt_ink_test

# ...and shared/c's own tests, as part of the same `make run`.
run: shared-tests

# The ink: one polygon per stroke, the pen's shape with no hole, the alpha.
$(call prog,uttt_ink_test,tests/uttt_ink_test.c)

# The words: every language fills every key, keeps English's placeholders,
# fits each key's width, and every caption it can compose fits one line.
$(call prog,uttt_lang_test,tests/uttt_lang_test.c)

# ...and the replay page's English-only kernel falls back rather than blanks.
SRC_EN := $(filter-out $(I18N),$(SRC)) i18n/strings_en.c
build/uttt_lang_en_test: $(call c_objs,build/obj/en,$(SRC_EN) tests/uttt_lang_test.c)
	$(CC) $(EN_FLAGS) $^ -o $@ $(LDLIBS)

$(call prog,uttt_bots_test,tests/uttt_bots_test.c)

$(call prog,uttt_anim_test,tests/uttt_anim_test.c)

# The board's side per drawer height, for the ruler's scorer (tools/uttt_side.c).
$(call prog,uttt_side,tools/uttt_side.c)

arena: build/uttt_arena
	./build/uttt_arena 200 200

$(call prog,uttt_line,tests/uttt_line.c)

# The 62% hotspot against the version it replaced, over every board there is.
line: build/uttt_line
	./build/uttt_line

$(call prog,uttt_mate,tests/uttt_mate.c)

# The mate search against an exhaustive one. Both the verdict and the
# distance, on positions deep enough to tell a min from a max. And quill's
# tree proofs against the same solver: it may say nothing, never something
# false.
mate: build/uttt_mate
	./build/uttt_mate 2000

$(call prog,uttt_icon,tools/uttt_icon.c)

# THE ICONS THE APP SHIPS, drawn by the app's own pen rather than traced by
# hand. Writes straight into both asset catalogues; commit what it changes.
icons: build/uttt_icon
	./tools/icons.sh

$(call prog,uttt_play,tools/uttt_play.c)

# PLAY THE THING. quill, first move, as hard as it gets without waiting.
ROLLOUTS ?= 400
play: build/uttt_play
	./build/uttt_play $(ROLLOUTS)

# THE STORE GAME: the one real game every App Store frame is a phase of
# (docs/STORE_SHOTS.md). Plays its fixed opening through the rules, finishes
# it with fountain and prints the ply list and the replay link.
$(call prog,uttt_store_game,tools/uttt_store_game.c)

store-game: build/uttt_store_game
	./build/uttt_store_game 44 5 2000

$(call prog,uttt_render,tests/uttt_render.c)

render: build/uttt_render
	./build/uttt_render $(PLIES)
	@sips -s format png build/board.ppm --out build/board.png >/dev/null \
	  && echo "build/board.png"


$(call prog,uttt_look,tools/uttt_look.c)

# The pieces the owner reviews, at a phone's scale, with numbers where a
# number settles it (the door's edge thickness). See tools/uttt_look.c.
look: build/uttt_look

# THE POST-GAME ANALYSER, not part of the app: every ply of a replay code
# held against quill. `make analyse CODE=<code or link> PLAYOUTS=20000`.
ANALYSE_SRC := src/uttt_analyse.c
$(call prog,uttt_analyse,$(ANALYSE_SRC) tools/uttt_analyse.c)

$(call prog,uttt_analyse_test,$(ANALYSE_SRC) tests/uttt_analyse_test.c)

PLAYOUTS ?= 20000
analyse: build/uttt_analyse
	@if [ -n "$(CODE)" ]; then ./build/uttt_analyse "$(CODE)" $(PLAYOUTS); fi

# A blunder that throws a forced win is called one, and two runs agree.
analyse-test: build/uttt_analyse_test
	./build/uttt_analyse_test

# The pen against the pen it was copied from. Needs node; not in `all`.
rough-diff: build/uttt_render
	node tests/rough_diff.mjs

# The same tests under the sanitizers, from their own objects (build/obj/asan):
# the kernel is compiled for them once, where each program used to compile it
# again.
ASAN_PROGS := uttt_test uttt_big_test uttt_big_bot_test uttt_msg_test uttt_big_msg_test \
              uttt_big_diag_test uttt_anim_test uttt_bots_test uttt_ink_test uttt_lang_test
asan: $(addprefix build/asan_,$(ASAN_PROGS)) shared-tests-asan
	./build/asan_uttt_test 400
	./build/asan_uttt_big_test 300 1
	./build/asan_uttt_big_bot_test 20
	./build/asan_uttt_msg_test 1000
	./build/asan_uttt_big_msg_test 3
	./build/asan_uttt_big_diag_test
	./build/asan_uttt_anim_test
	./build/asan_uttt_bots_test 3
	./build/asan_uttt_ink_test
	./build/asan_uttt_lang_test

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
# the shared recipe, and it rebuilds only what changed - which is what lets
# Xcode run this target on every build (shared/scripts/xcode_kernel.sh).
include ../../shared/tools/ios_xcframework.mk
$(eval $(call IOS_XCFRAMEWORK,uttt,$(IOS_SRC),ios/include,../ios/vendor/Uttt.xcframework))

ios-lib: $(IOS_XCFRAMEWORKS)


$(call prog,ios_smoke,ios/uttt_api.c ios/uttt_api_smoke.c)

ios-smoke: build/ios_smoke
	./build/ios_smoke

.PHONY: ios-smoke ios-lib

# THE SEND RULES SIDE BY SIDE (docs/BIG_BOARD_SEND_RULE.md): whole depth-5
# games under one rule, random and greedy bots, and what they decided. A
# measurement, not a test, so not in run. ARGS="<rule 0|1|2> <random games>
# <bot games> [seed]".
$(call prog,uttt_big_rules_sim,$(BIG_BOT_SRC) tests/uttt_big_rules_sim.c)

big-rules-sim: build/uttt_big_rules_sim
	./build/uttt_big_rules_sim $(ARGS)

.PHONY: big-rules-sim

# THE BIG GAME THROUGH A REAL SEND'S PICTURE CHAIN: one random depth-5 game
# to the end, every ply framed and painted by the kit's C at 3 px a cell (the
# bubble's robust243), JPEG'd at q0.50 and q0.89 with ImageIO (both 4:2:0),
# read back and decoded. About 18 minutes for a whole game. macOS only
# (ImageIO), so NOT in run/asan/all: CI is Linux. ARGS="--plies N", "--every
# K", "--px 1" (board243, which a real game's sparse board does NOT survive:
# see the test's header).
ifeq ($(shell uname),Darwin)
IMAGEIO := -framework ImageIO -framework CoreGraphics -framework CoreFoundation
$(call prog,uttt_big_chain,$(BUBBLE_DATA)/bubble_data.c tests/uttt_big_chain.c,$(IMAGEIO))

big-chain: build/uttt_big_chain
	./build/uttt_big_chain $(ARGS)

# THE 243 DIAGNOSTICS' QUALITY TABLES: ImageIO's luma table at every step
# 0.00..1.00, written into src/uttt_jpeg_imageio.inc (commit what it changes).
$(call prog,uttt_jpeg_imageio,tools/uttt_jpeg_imageio.c,$(IMAGEIO))

jpeg-imageio: build/uttt_jpeg_imageio
	./build/uttt_jpeg_imageio src/uttt_jpeg_imageio.inc

# ...and the parser against JPEGs ImageIO writes NOW, at known qualities, and
# the committed tables against what ImageIO writes today. macOS only.
$(call prog,uttt_big_diag_imageio,$(BUBBLE_DATA)/bubble_data.c tests/uttt_big_diag_imageio.c,$(IMAGEIO))

big-diag-imageio: build/uttt_big_diag_imageio
	./build/uttt_big_diag_imageio

.PHONY: big-chain jpeg-imageio big-diag-imageio
endif

$(call prog,uttt_ladder,tests/uttt_ladder.c)
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
# What compiles a file and what links the module, apart: an object is compiled
# once (the variant below) and the linker's flags mean nothing to a compile.
# The link is still handed every flag the single command had, so the driver
# does there exactly what it did.
WEB_WASM_CFLAGS  := --target=wasm32 -Oz -nostdlib -ffreestanding -mbulk-memory \
                    -mno-nontrapping-fptoint -std=c11 -Wall -Wextra -Werror -DUTTT_LANG_ENGLISH_ONLY \
                    -isystem ../../shared/c/wasm/include -Isrc
WEB_WASM_LDFLAGS := -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=131072 -Wl,--strip-all
$(eval $(call C_VARIANT,build/obj/web,WEB_WASM_CC,WEB_WASM_CFLAGS))

$(WEB_WASM_OUT): $(call c_objs,build/obj/web,$(WEB_WASM_SRC)) Makefile
	@mkdir -p $(dir $@)
	$(WEB_WASM_CC) $(WEB_WASM_CFLAGS) $(WEB_WASM_LDFLAGS) $(filter %.o,$^) -o $@
	@echo "built $@ ($$(wc -c < $@) B, $$(gzip -9 -c $@ | wc -c) B gz)"

wasm-web: $(WEB_WASM_OUT)

# THE 243 PAGE (uttt.live/243): two greedy bots playing the depth-5 game
# (wasm/uttt_243_web.c, src/uttt_big_bot.h). Its own module, so the replay
# page does not download the big game and the big page does not download the
# pen. A build output like uttt.wasm, never committed.
#
# THE PAGE NEVER LEARNS A BYTE LAYOUT. What it reads besides the byte arrays
# (wasm/uttt_243_web.h's structs and UA_ constants) it reads through readers
# shared/tools/structgen writes from that header, into ../web/lib/gen/ (a
# build output, ignored), and the module is stamped with the layout's hash
# (-DSG_LAYOUT_HASH, ua_layout_hash) which lib/arena.ts holds against the
# generated LAYOUT_HASH at load. structgen is a libclang program: CI's
# shared/scripts/ci_llvm.sh installs libclang; on a Mac, Homebrew's llvm.
#
# THE MVP FEATURE SET, plus bulk memory and sign extension, and nothing else
# (-mcpu=mvp): the compiler's default wasm32 also turns on SIMD, multivalue
# and reference types, and an older Safari or an in-app browser refuses a
# module that carries any of them - the page then cannot start at all.
#
# ALWAYS REBUILT (FORCE): generating and linking take about a second, and a
# module and a reader that could come from two different headers - make's
# whole-second mtimes, a checkout moving a header backwards - is the one
# failure the hash exists to catch. Rebuilding every time is cheaper than
# being clever about it.
include ../../shared/tools/gen_tools.mk
ARENA_FLAGS    := -isystem ../../shared/c/wasm/include -Isrc
ARENA_SG_ARGS  := --cwd . --header wasm/uttt_243_web.h \
                  --root UaStatus --root UaConfig --root UaBox \
                  --snapshot UaStatus --snapshot UaConfig --snapshot UaBox --snapshot-only \
                  --const UA_ --build 'web243=$(ARENA_FLAGS)'
ARENA_GEN      := ../web/lib/gen
ARENA_WASM_SRC := src/uttt.c src/uttt_big.c src/uttt_big_bot.c \
                  ../../shared/c/wasm/libc.c wasm/uttt_243_web.c
ARENA_WASM_OUT := ../web/public/uttt243.wasm

wasm-243: $(STRUCTGEN)
	@mkdir -p build $(ARENA_GEN) $(dir $(ARENA_WASM_OUT))
	@$(STRUCTGEN) $(ARENA_SG_ARGS) --ts $(ARENA_GEN)/arena_layout.ts \
	  --hash-ts $(ARENA_GEN)/arena_layout_hash.ts --print-hash > build/arena_layout.hash
	$(WEB_WASM_CC) --target=wasm32 -mcpu=mvp -mbulk-memory -msign-ext -O3 -nostdlib -ffreestanding \
	  -mno-nontrapping-fptoint -std=c11 -Wall -Wextra -Werror $(ARENA_FLAGS) \
	  -DSG_LAYOUT_HASH=$$(cat build/arena_layout.hash) \
	  -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=131072 -Wl,--strip-all \
	  $(ARENA_WASM_SRC) -o $(ARENA_WASM_OUT)
	@echo "built $(ARENA_WASM_OUT) ($$(wc -c < $(ARENA_WASM_OUT)) B, $$(gzip -9 -c $(ARENA_WASM_OUT) | wc -c) B gz), layout $$(cat build/arena_layout.hash)"

.PHONY: wasm-web wasm-243
