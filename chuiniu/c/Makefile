# Chui Niu - the kernel, its wire, its iOS bridge and its tests. `make run`
# is the real test target. The shape is pickemup/c/Makefile's.
#
#   make -C chuiniu/c run         every test and the bridge smoke, built and run
#   make -C chuiniu/c asan        the same under ASan + UBSan
#   make -C chuiniu/c wasm        prove the kernel builds for wasm32, freestanding
#   make -C chuiniu/c wasm-roll   the throw (cn_roll.c) as a browser module, build/wasm/cn_roll.wasm
#   make -C chuiniu/c docs-roll   embed that module in ../docs/UI.html (the study plays it)
#   make -C chuiniu/c bench       the renderer's speed and memory (cn_scene_bench; not a test)
#   make -C chuiniu/c ios-smoke   every bridge entry point, host compiler, no Mac
#   make -C chuiniu/c structgen   the Swift readers  -> ../ios/Generated/ (needs libclang)
#   make -C chuiniu/c datagen     the Swift strings  -> ../ios/Generated/i18n/
#   make -C chuiniu/c ios-lib     ../ios/vendor/Chuiniu.xcframework (needs Xcode)
#   make -C chuiniu/c swift-smoke the bridge driven from Swift through the readers (a Mac)
#   make -C chuiniu/c clean
CC      ?= cc
# THE WARNINGS ARE ONE LIST for every build below. -ffp-contract=off is in it
# because cn_roll.c's floating point must come out the same on every machine
# (a fused multiply-add rounds once where two operations round twice).
WARN    := -Wall -Wextra -Werror -std=c11 -ffp-contract=off
CFLAGS  ?= -O2 $(WARN)
SHARED  := ../../shared/c
SRC     := src/cn.c src/cn_dice.c src/cn_plan.c src/cn_view.c src/cn_lobby.c src/cn_say.c \
           src/cn_code.c src/cn_msg.c src/cn_beats.c src/cn_roll.c src/cn_scene.c src/cn_geom.c src/cn_cam.c src/cn_lay.c src/cn_tex.c src/cn_stage.c i18n/strings_en.c \
           $(SHARED)/deal_rng.c $(SHARED)/sha256.c $(SHARED)/b32.c $(SHARED)/mixrad.c \
           $(SHARED)/text_util/text_util.c $(SHARED)/wire_check/wire_check.c \
           $(SHARED)/msg_seat_tag/msg_seat_tag.c $(SHARED)/msg_lobby_roster/msg_lobby_roster.c
HDR     := $(wildcard src/*.h) i18n/keys.h tests/cn_check.h \
           $(SHARED)/deal_rng.h $(SHARED)/sha256.h $(SHARED)/b32.h $(SHARED)/mixrad.h \
           $(SHARED)/test/check.h $(SHARED)/test/twophone.h $(SHARED)/text_util/text_util.h \
           $(SHARED)/wire_check/wire_check.h $(SHARED)/stats/seed_hash.h \
           $(SHARED)/msg_seat_tag/msg_seat_tag.h $(SHARED)/msg_lobby_roster/msg_lobby_roster.h

# One binary per test file; each takes an optional scale argument.
TESTS   := cn_test cn_dice_test cn_plan_test cn_say_test cn_fuzz cn_msg_test cn_twophone_test cn_roll_test cn_scene_test cn_geom_test cn_cam_test cn_lay_test cn_stage_test
FUZZ_GAMES ?= 3000
WIRE_GAMES ?= 40

all: $(addprefix build/,$(TESTS))

# The tests that drive the bridge compile it in (#include "../ios/cn_api.c").
BRIDGE_TESTS := cn_dice_test cn_twophone_test
$(addprefix build/,$(BRIDGE_TESTS)) $(addprefix build/asan_,$(BRIDGE_TESTS)): ios/cn_api.c ios/include/cn_api.h ios/cn_api_layout.h

build:
	@mkdir -p build

build/%: tests/%.c $(SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(SRC) $< -lm -o $@

# The shared harness's own test (shared/c/test): TEST, CHECK, report, STEP, OK.
CHECK_TEST := $(SHARED)/test/check_test.c $(SHARED)/test/check.h $(SHARED)/test/twophone.h
build/check_test: $(CHECK_TEST) | build
	$(CC) $(CFLAGS) $(SHARED)/test/check_test.c -o $@

# The shared text writers' own test (shared/c/text_util): UTF-8, columns, the
# buffer writers at their capacity.
TEXT_UTIL := $(SHARED)/text_util/text_util.c $(SHARED)/text_util/text_util_test.c $(SHARED)/text_util/text_util.h
build/text_util_test: $(TEXT_UTIL) | build
	$(CC) $(CFLAGS) $(SHARED)/text_util/text_util.c $(SHARED)/text_util/text_util_test.c -o $@

# The shared wire check's own test (shared/c/wire_check): the envelope's
# truncated SHA-256 over head and body, which cn_msg.c carries CN_CHECK_LEN bytes of.
WIRE_CHECK_SRC := $(SHARED)/sha256.c $(SHARED)/wire_check/wire_check.c $(SHARED)/wire_check/wire_check_test.c
WIRE_CHECK := $(WIRE_CHECK_SRC) $(SHARED)/wire_check/wire_check.h $(SHARED)/sha256.h
build/wire_check_test: $(WIRE_CHECK) | build
	$(CC) $(CFLAGS) $(WIRE_CHECK_SRC) -o $@

# The shared seat identity's own test (shared/c/msg_seat_tag): the roster row,
# the nickname verdict and the record / tag / sender / name resolver that
# cn_msg.c's cn_msg_resolve hands its rows to.
SEAT_TAG_SRC := $(SHARED)/msg_seat_tag/msg_seat_tag.c $(SHARED)/msg_seat_tag/msg_seat_tag_test.c
SEAT_TAG := $(SEAT_TAG_SRC) $(SHARED)/msg_seat_tag/msg_seat_tag.h
build/msg_seat_tag_test: $(SEAT_TAG) | build
	$(CC) $(CFLAGS) $(SEAT_TAG_SRC) -o $@

# The shared lobby rules' own test (shared/c/msg_lobby_roster): capacity,
# join, leave, the one control each viewer is offered, the start through the
# game's constructor, and the roster's changes, all of which the lobby
# adapter and cn_msg.c call.
LOBBY_ROSTER_SRC := $(SHARED)/msg_lobby_roster/msg_lobby_roster.c $(SHARED)/msg_lobby_roster/msg_lobby_roster_test.c
LOBBY_ROSTER := $(LOBBY_ROSTER_SRC) $(SHARED)/msg_lobby_roster/msg_lobby_roster.h
build/msg_lobby_roster_test: $(LOBBY_ROSTER) | build
	$(CC) $(CFLAGS) $(LOBBY_ROSTER_SRC) -o $@

# The shared arena statistics' own test (shared/c/stats): mean and standard
# error, the Wilson interval, and the per-game seed hash bot/cn_arena.c seeds with.
STATS_SRC := $(SHARED)/stats/stats.c $(SHARED)/stats/stats_test.c
STATS := $(STATS_SRC) $(SHARED)/stats/stats.h $(SHARED)/stats/seed_hash.h
build/stats_test: $(STATS) | build
	$(CC) $(CFLAGS) $(STATS_SRC) -lm -o $@

run: $(addprefix build/,$(TESTS)) build/check_test build/text_util_test build/wire_check_test build/msg_seat_tag_test build/msg_lobby_roster_test build/stats_test build/ios_smoke
	./build/check_test
	./build/text_util_test
	./build/wire_check_test
	./build/msg_seat_tag_test
	./build/msg_lobby_roster_test
	./build/stats_test
	./build/cn_test
	./build/cn_dice_test
	./build/cn_plan_test
	./build/cn_say_test
	./build/cn_fuzz $(FUZZ_GAMES)
	./build/cn_msg_test $(WIRE_GAMES)
	./build/cn_twophone_test
	./build/cn_roll_test
	./build/cn_scene_test
	./build/cn_geom_test
	./build/cn_cam_test
	./build/cn_lay_test
	./build/cn_stage_test $(TEX_PACK)
	./build/ios_smoke build/cn_tex.pack

# THE RENDERER'S SPEED (not a test, not in `run`): a six-seat frame at 1x, 2x
# and 3x, at -O2 as the iOS library builds it; `./build/cn_scene_bench loop 2 20`
# is the long loop to point `sample` at.
build/cn_scene_bench: tests/cn_scene_bench.c tests/cn_scene_frame.h src/cn_scene.c src/cn_scene.h | build
	$(CC) -O2 $(WARN) tests/cn_scene_bench.c src/cn_scene.c -o $@
bench: build/cn_scene_bench
	./build/cn_scene_bench

ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN)

build/asan_%: tests/%.c $(SRC) $(HDR) | build
	$(CC) $(ASAN_FLAGS) $(SRC) $< -lm -o $@

build/asan_check_test: $(CHECK_TEST) | build
	$(CC) $(ASAN_FLAGS) $(SHARED)/test/check_test.c -o $@

build/asan_text_util_test: $(TEXT_UTIL) | build
	$(CC) $(ASAN_FLAGS) $(SHARED)/text_util/text_util.c $(SHARED)/text_util/text_util_test.c -o $@

build/asan_wire_check_test: $(WIRE_CHECK) | build
	$(CC) $(ASAN_FLAGS) $(WIRE_CHECK_SRC) -o $@

build/asan_msg_seat_tag_test: $(SEAT_TAG) | build
	$(CC) $(ASAN_FLAGS) $(SEAT_TAG_SRC) -o $@

build/asan_msg_lobby_roster_test: $(LOBBY_ROSTER) | build
	$(CC) $(ASAN_FLAGS) $(LOBBY_ROSTER_SRC) -o $@

build/asan_stats_test: $(STATS) | build
	$(CC) $(ASAN_FLAGS) $(STATS_SRC) -lm -o $@

asan: $(addprefix build/asan_,$(TESTS)) build/asan_check_test build/asan_text_util_test build/asan_wire_check_test build/asan_msg_seat_tag_test build/asan_msg_lobby_roster_test build/asan_stats_test build/asan_ios_smoke
	./build/asan_check_test
	./build/asan_text_util_test
	./build/asan_wire_check_test
	./build/asan_msg_seat_tag_test
	./build/asan_msg_lobby_roster_test
	./build/asan_stats_test
	./build/asan_cn_test
	./build/asan_cn_dice_test
	./build/asan_cn_plan_test
	./build/asan_cn_say_test
	./build/asan_cn_fuzz 600
	./build/asan_cn_msg_test 6
	./build/asan_cn_twophone_test
	./build/asan_cn_roll_test 60
	./build/asan_cn_scene_test
	./build/asan_cn_geom_test
	./build/asan_cn_cam_test
	./build/asan_cn_lay_test
	./build/asan_cn_stage_test $(TEX_PACK)
	./build/asan_ios_smoke build/cn_tex.pack

# THE SAME FILES FOR wasm32, freestanding (-nostdlib, shared/c/wasm's libc):
# the proof that nothing here reaches past memcpy / memset / memcmp / strlen /
# strncmp. Objects only.
# WASM_CC and the guard that refuses Apple clang: shared/tools/wasm_cc.mk.
include ../../shared/tools/wasm_cc.mk
wasm: wasm-cc-check | build
	@mkdir -p build/wasm
	@for s in $(SRC); do \
	  o=build/wasm/$$(basename $$s .c).o; \
	  $(WASM_CC) --target=wasm32 -Oz -nostdlib -ffreestanding $(WARN) \
	    -isystem $(SHARED)/wasm/include -c $$s -o $$o || exit 1; \
	done
	@echo "wasm32 objects in build/wasm/"

# THE THROW AS A MODULE (K14): cn_roll.c and its scalar wrapper (wasm/cn_roll_web.c)
# linked for a browser, in uttt's web shape: freestanding, no entry, the memory
# exported, MVP plus bulk memory, sign extension and non-trapping float-to-int
# (every browser since 2020; the renderer's inner loop casts on every pixel, and
# the trapping form costs it a third), at -O3 for the same reason. And SIMD128
# (Chrome 91, Firefox 89, Safari 16.4): the renderer draws four pixels at a time in
# the compilers' vector types, which without it are cut back to scalar code and
# made the study's throw frames a third slower than the old scalar renderer (26.5
# ms against 20 in Chrome); with it they take 18, and draw the same bits as one
# pixel at a time (cn_scene_test's golden, built as wasm, hashes alike).
# A build output, never committed as a file. docs-roll embeds it in docs/UI.html
# as base64 between two markers, so the one-file study still opens from disk;
# THAT embed is committed, and goes stale when cn_roll.c changes until docs-roll
# is run again (docs-roll-check says whether it has).
# The module carries the scene renderer too (src/cn_scene.c: a rasterizer with a
# shadow map, in C for speed, the same file the iOS library links; wasm/cn_scene_web.c
# gives it its 256 MB static arena and the names the page calls). Its test is
# cn_scene_test; its speed is cn_scene_bench (make bench).
ROLL_WASM_SRC := src/cn_roll.c wasm/cn_roll_web.c src/cn_scene.c wasm/cn_scene_web.c $(SHARED)/wasm/libc.c
ROLL_WASM_OUT := build/wasm/cn_roll.wasm
$(ROLL_WASM_OUT): $(ROLL_WASM_SRC) src/cn_roll.h src/cn_scene.h Makefile | build
	@mkdir -p build/wasm
	$(WASM_CC) --target=wasm32 -mcpu=mvp -mbulk-memory -msign-ext -mnontrapping-fptoint -msimd128 -O3 -nostdlib -ffreestanding \
	  $(WARN) -isystem $(SHARED)/wasm/include -Isrc \
	  -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=65536 -Wl,--strip-all \
	  $(ROLL_WASM_SRC) -o $@
	@echo "built $@ ($$(wc -c < $@) B, $$(gzip -9 -c $@ | wc -c) B gz)"
wasm-roll: wasm-cc-check $(ROLL_WASM_OUT)

DOC := ../docs/UI.html
docs-roll: $(ROLL_WASM_OUT)
	@b64=$$(base64 < $(ROLL_WASM_OUT) | tr -d '\n'); \
	 awk -v b="$$b64" 'BEGIN{skip=0} /<!-- cn_roll.wasm begin/{print; print "<script>window.CN_ROLL_WASM_B64 = \"" b "\";</script>"; skip=1; next} /<!-- cn_roll.wasm end/{skip=0} skip==0{print}' $(DOC) > $(DOC).new \
	 && mv $(DOC).new $(DOC) && echo "embedded $(ROLL_WASM_OUT) in $(DOC)"
# (the embed is cut out of the page and compared whole: grep with the module's
# 70 KB of base64 as its pattern runs out of memory, and its failure read as "differs")
docs-roll-check: $(ROLL_WASM_OUT)
	@b64=$$(base64 < $(ROLL_WASM_OUT) | tr -d '\n'); \
	 have=$$(sed -n 's/.*CN_ROLL_WASM_B64 = "\([A-Za-z0-9+/=]*\)".*/\1/p' $(DOC)); \
	 [ -n "$$have" ] && [ "$$have" = "$$b64" ] && echo "docs/UI.html carries this build of cn_roll.wasm" || { echo "docs/UI.html's embedded cn_roll.wasm differs from this build: make docs-roll (a different clang also differs)"; exit 1; }

# ---- the iOS bridge -----------------------------------------------------------
# ios-smoke runs the identical bridge with the host compiler, so every
# invariant on it is testable WITHOUT a Mac; ios-lib needs Xcode.
IOS_MIN_VERSION := 17.0
IOS_SRC    := $(SRC) ios/cn_api.c
IOS_HDR    := $(HDR) ios/include/cn_api.h ios/cn_api_layout.h
IOS_CFLAGS := -O2 -Isrc -Iios -Iios/include $(WARN)

build/ios_smoke: $(IOS_SRC) $(IOS_HDR) ios/cn_api_smoke.c | build
	$(CC) $(CFLAGS) -Iios/include $(IOS_SRC) ios/cn_api_smoke.c -lm -o $@

build/asan_ios_smoke: $(IOS_SRC) $(IOS_HDR) ios/cn_api_smoke.c | build
	$(CC) $(ASAN_FLAGS) -Iios/include $(IOS_SRC) ios/cn_api_smoke.c -lm -o $@

ios-smoke: build/ios_smoke tex
	./build/ios_smoke build/cn_tex.pack

# THE GENERATED SWIFT. Swift never learns a byte layout: every struct
# cn_api.h hands over is read through readers shared/tools/structgen writes
# from the kernel's own headers (ios/layout.args), and every string key
# through shared/tools/datagen. A BUILD OUTPUT, never committed.
GEN          := ../ios/Generated
STRUCTGEN_DIR := ../../shared/tools/structgen
DATAGEN_DIR  := ../../shared/tools/datagen
STRUCTGEN    := $(STRUCTGEN_DIR)/build/structgen
DATAGEN      := $(DATAGEN_DIR)/build/datagen
SG_SPEC      := ios/layout.args
SG_ARGS       = $(shell grep -v '^[[:space:]]*\#' $(SG_SPEC))
IOS_LAYOUT_TRIPLE  := arm64-apple-ios$(IOS_MIN_VERSION)
IOS_LAYOUT_TRIPLES := $(IOS_LAYOUT_TRIPLE) arm64-apple-ios$(IOS_MIN_VERSION)-simulator \
                      x86_64-apple-ios$(IOS_MIN_VERSION)-simulator

$(STRUCTGEN): $(wildcard $(STRUCTGEN_DIR)/*.c) $(wildcard $(STRUCTGEN_DIR)/*.h) \
              $(wildcard $(STRUCTGEN_DIR)/../sgcommon/*.c) $(wildcard $(STRUCTGEN_DIR)/../sgcommon/*.h) \
              $(STRUCTGEN_DIR)/Makefile $(STRUCTGEN_DIR)/../llvm.mk
	$(MAKE) -C $(STRUCTGEN_DIR) build/structgen
$(DATAGEN): $(DATAGEN_DIR)/datagen.c $(wildcard $(DATAGEN_DIR)/../sgcommon/*.c) \
            $(wildcard $(DATAGEN_DIR)/../sgcommon/*.h) $(DATAGEN_DIR)/Makefile $(DATAGEN_DIR)/../llvm.mk
	$(MAKE) -C $(DATAGEN_DIR) build/datagen

# cmp-then-copy: a generated file's mtime moves only when its content does.
sync = mkdir -p $(dir $(2)) && { cmp -s $(1) $(2) || { cp $(1) $(2); echo "wrote $(2)"; }; } && rm -f $(1)

build/layout/ios.hash: $(STRUCTGEN) $(SG_SPEC) $(IOS_HDR) FORCE | build
	@mkdir -p build/layout
	@for t in $(IOS_LAYOUT_TRIPLES); do \
	  $(STRUCTGEN) --cwd . $(SG_ARGS) --build 'ios=$(strip $(IOS_CFLAGS))' --target $$t --print-hash || exit 1; \
	done | sort -u > $@.new
	@[ "$$(grep -c . $@.new)" = 1 ] || { echo "layout ios: the iOS slices do not share one layout:"; cat $@.new; exit 1; }
	@{ cmp -s $@.new $@ || { echo "layout ios: $$(cat $@ 2>/dev/null || echo none) -> $$(cat $@.new)"; mv $@.new $@; }; }; rm -f $@.new
	@$(STRUCTGEN) --cwd . $(SG_ARGS) --build 'ios=$(strip $(IOS_CFLAGS))' --target $(IOS_LAYOUT_TRIPLE) \
	  --swift $@.swift && $(call sync,$@.swift,$(GEN)/ChuiniuKernel.swift)

structgen: build/layout/ios.hash

datagen: $(DATAGEN) i18n/keys.h i18n/strings_en.c | build
	@mkdir -p build/i18n
	@$(DATAGEN) --cwd i18n --header keys.h --table CN_KEY_NAME --require-complete \
	  --name ChuiniuStringKeys --swift build/i18n/ChuiniuStringKeys.swift \
	  && $(call sync,build/i18n/ChuiniuStringKeys.swift,$(GEN)/i18n/ChuiniuStringKeys.swift)
	@$(DATAGEN) --cwd i18n --header strings_en.c --table CN_STRINGS_EN --labels CN_STRINGS_EN.0=CN_KEY_NAME \
	  --require-complete --name ChuiniuStringsEn --swift build/i18n/ChuiniuStringsEn.swift \
	  && $(call sync,build/i18n/ChuiniuStringsEn.swift,$(GEN)/i18n/ChuiniuStringsEn.swift)

# THE SWIFT SIDE, ON THIS MAC: the readers generated for this machine's
# triple, the kernel compiled for it and stamped with that layout's hash, and
# a Swift program that drives the bridge through them (ios/cn_api_smoke.swift).
SWIFT_TRIPLE = $(shell swiftc -print-target-info 2>/dev/null | sed -n 's/.*"unversionedTriple": *"\([^"]*\)".*/\1/p' | head -1)
swift-smoke: $(STRUCTGEN) datagen tex | build
	@rm -rf build/swift && mkdir -p build/swift/obj
	@$(STRUCTGEN) --cwd . $(SG_ARGS) --build 'host=$(strip $(IOS_CFLAGS))' --target $(SWIFT_TRIPLE) \
	  --swift build/swift/ChuiniuKernel.swift --print-hash > build/swift/hash
	@for s in $(IOS_SRC); do \
	  $(CC) $(IOS_CFLAGS) -DSG_LAYOUT_HASH=$$(cat build/swift/hash) -c $$s -o build/swift/obj/$$(basename $$s .c).o || exit 1; \
	done
	swiftc -parse-as-library -import-objc-header ios/include/cn_api.h build/swift/ChuiniuKernel.swift \
	  $(GEN)/i18n/*.swift ios/cn_api_smoke.swift build/swift/obj/*.o -o build/swift/smoke
	./build/swift/smoke build/cn_tex.pack

include ../../shared/tools/ios_xcframework.mk

# THE LIBRARY CARRIES THE LAYOUT IT WAS COMPILED FOR, and the readers are
# written beside it in the same run, so the pair cannot come apart.
# THE SCENE RENDERER (src/cn_scene.c, in SRC) is in the library too, and its
# header goes into the xcframework beside cn_api.h, so the bridge can include it
# and Swift reach it through module CChuiniu; after the build every slice is
# asked for its entry points, so a library without the renderer fails here, not
# in an app's link.
IOS_HEADERS := build/ios/include
IOS_SCENE_SYMBOLS := cn_scene_init cn_scene_reset cn_scene_tex_new cn_scene_begin cn_scene_occluder \
                     cn_scene_render cn_scene_prepare cn_scene_band cn_scene_fb cn_scene_frame_bytes
ios-lib: IOS_CFLAGS += -DSG_LAYOUT_HASH=$$(cat build/layout/ios.hash)
ios-lib: build/layout/ios.hash datagen tex-ios
	@rm -rf $(IOS_HEADERS) && mkdir -p $(IOS_HEADERS) && cp ios/include/* src/cn_scene.h $(IOS_HEADERS)/
	$(call IOS_XCFRAMEWORK,chuiniu,$(IOS_SRC),$(IOS_CFLAGS),$(IOS_HEADERS),$(IOS_MIN_VERSION),../ios/vendor/Chuiniu.xcframework)
	@for a in build/ios/device/libchuiniu.a build/ios/sim-arm64/libchuiniu.a build/ios/sim-x86_64/libchuiniu.a; do \
	  for s in $(IOS_SCENE_SYMBOLS); do \
	    nm -g $$a 2>/dev/null | grep -q " T _$$s$$" || { echo "ios-lib: $$a does not define $$s"; exit 1; }; \
	  done; \
	done; echo "the scene renderer is in every slice ($(words $(IOS_SCENE_SYMBOLS)) entry points)"
	@echo "layout $$(cat build/layout/ios.hash)"

clean:
	rm -rf build

.PHONY: all run bench asan wasm wasm-roll docs-roll docs-roll-check ios-smoke swift-smoke structgen datagen ios-lib clean FORCE
FORCE:
.SECONDARY:

# ---- THE TEXTURE PACK (package C) ----------------------------------------------
# The cups' and dice's textures are baked at BUILD time (a procedural texture made
# at launch once took the iMessage extension down on a device): tools/cn_texgen.c
# bakes the verdigris and bone tiles and the numerals 0-9 (IM Fell English, OFL,
# tools/fonts/) into build/cn_tex.pack, and src/cn_tex.c derives every seat's and
# die's texture from it at upload. Layout in src/cn_tex.h; notes in docs_pkgC.md.
#   make -C chuiniu/c tex          build/cn_tex.pack
#   make -C chuiniu/c tex-ios      ...and its copy in ../ios/ChuiniuKit/Resources/
#   make -C chuiniu/c tex-test     the pack's and the textures' tests (in `run`; tex-asan in `asan`)
#   make -C chuiniu/c tex-wasm     cn_tex.c for wasm32, freestanding (an object only)
#   make -C chuiniu/c tex-compare REF=dir   MAD against the study's captures, and a contact sheet
TEX_FONT := tools/fonts/IMFeENrm28P.ttf
TEX_SRC  := src/cn_tex.c src/cn_tex.h
TEX_PACK := build/cn_tex.pack
TEX_IOS  := ../ios/ChuiniuKit/Resources/cn_tex.pack

build/cn_texgen: tools/cn_texgen.c $(TEX_SRC) | build
	$(CC) $(CFLAGS) tools/cn_texgen.c src/cn_tex.c -o $@

$(TEX_PACK): build/cn_texgen $(TEX_FONT)
	./build/cn_texgen --font $(TEX_FONT) --pack $@

tex: $(TEX_PACK)

# cmp-then-copy, so the resource's mtime moves only when its bytes do
tex-ios: $(TEX_PACK)
	@mkdir -p $(dir $(TEX_IOS)) && { cmp -s $(TEX_PACK) $(TEX_IOS) && echo "$(TEX_IOS) is current" || { cp $(TEX_PACK) $(TEX_IOS); echo "wrote $(TEX_IOS)"; }; }

build/cn_tex_test: tests/cn_tex_test.c tools/cn_texgen.c $(TEX_SRC) | build
	$(CC) $(CFLAGS) tests/cn_tex_test.c src/cn_tex.c -o $@

build/asan_cn_tex_test: tests/cn_tex_test.c tools/cn_texgen.c $(TEX_SRC) | build
	$(CC) $(ASAN_FLAGS) tests/cn_tex_test.c src/cn_tex.c -o $@

tex-test: build/cn_tex_test $(TEX_PACK)
	./build/cn_tex_test $(TEX_FONT) $(TEX_PACK)

tex-asan: build/asan_cn_tex_test $(TEX_PACK)
	./build/asan_cn_tex_test $(TEX_FONT) $(TEX_PACK)

run: tex-test
asan: tex-asan

tex-wasm: wasm-cc-check | build
	@mkdir -p build/wasm
	$(WASM_CC) --target=wasm32 -Oz -nostdlib -ffreestanding $(WARN) -isystem $(SHARED)/wasm/include -c src/cn_tex.c -o build/wasm/cn_tex.o

tex-compare: build/cn_texgen
	@[ -n "$(REF)" ] || { echo "tex-compare: REF=the directory of the study's captures (docs_pkgC.md)"; exit 2; }
	./build/cn_texgen --font $(TEX_FONT) --compare $(REF) --sheet build/cn_tex_sheet.png

.PHONY: tex tex-ios tex-test tex-asan tex-wasm tex-compare

# ---- THE ICONS (package F) -----------------------------------------------------------
# The app's icons, drawn by the table's own renderer (tools/cn_icon.c: the cup and die
# meshes, the baked verdigris and bone, cn_scene.c's rasterizer and shadow map), each
# size at its own size, straight into both asset catalogues; commit what it changes.
# A host-only tool (zlib for the PNGs), never in the app. Notes: ../docs/ICON.md.
#   make -C chuiniu/c icons        every catalogue PNG, and build/cn_icon_sheet.png
ICON_EXT := ../ios/ChuiniuMessages/Assets.xcassets/iMessage App Icon.stickersiconset
ICON_APP := ../ios/ChuiniuMessagesApp/Assets.xcassets/AppIcon.appiconset

build/cn_icon: tools/cn_icon.c $(SRC) $(HDR) $(TEX_SRC) | build
	$(CC) $(CFLAGS) $(SRC) tools/cn_icon.c -lz -lm -o $@

icons: build/cn_icon $(TEX_PACK)
	./build/cn_icon --pack $(TEX_PACK) --ext "$(ICON_EXT)" --app "$(ICON_APP)" --sheet build/cn_icon_sheet.png

.PHONY: icons
