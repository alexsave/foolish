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
#   make -C chuiniu/c bot-test    the offline bot's tests (bot/, docs/BOT.md); bot-asan, bot-arena, bot-arena-fast
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
           $(SHARED)/msg_seat_tag/msg_seat_tag.c $(SHARED)/msg_lobby_roster/msg_lobby_roster.c \
           $(SHARED)/msg_whole/msg_whole.c

# One binary per test file; each takes an optional scale argument.
TESTS   := cn_test cn_dice_test cn_plan_test cn_say_test cn_fuzz cn_msg_test cn_twophone_test cn_roll_test cn_scene_test cn_geom_test cn_cam_test cn_lay_test cn_stage_test
FUZZ_GAMES ?= 3000
WIRE_GAMES ?= 40

all: $(addprefix build/,$(TESTS))

# The tests that drive the bridge compile it in (#include "../ios/cn_api.c").
# ---- how a file becomes an object, and a program ----------------------------
# shared/tools/c_rules.mk: one directory of objects per compiler-and-flags (a
# "variant"), each object depending on exactly the headers its compile opened
# and on the command line it was built with. There is no list of headers here
# any more: the one there was had to name every header any test could reach,
# and the bridge's besides for the two tests that compile it in (cn_dice_test,
# cn_twophone_test).
#
# -Iios/include is for the bridge's own files (ios/...): said per object
# (c_rules.mk, $*), so the kernel is compiled once for the tests and the smoke
# alike.
ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN)
HOST_FLAGS  = $(CFLAGS) $(if $(filter ios/%,$*),-Iios/include)
SAN_FLAGS   = $(ASAN_FLAGS) $(if $(filter ios/%,$*),-Iios/include)
# The bench and the two picture tools are -O2 whatever CFLAGS says.
O2_FLAGS   := -O2 $(WARN)

# shared/c's own tests, the ones for what this kernel links: the harness
# (TEST, CHECK, report, STEP, OK), the text writers, the envelope's truncated
# SHA-256 that cn_msg.c carries CN_CHECK_LEN bytes of, the seat identity
# cn_msg_resolve hands its rows to, the lobby rules the lobby adapter and
# cn_msg.c call, and the arena's statistics. What each is built from is
# shared/tools/shared_tests.mk's.
SHARED_TESTS := check_test rng_test text_util_test wire_check_test msg_seat_tag_test msg_lobby_roster_test msg_whole_test stats_test

include ../../shared/tools/c_rules.mk
include ../../shared/tools/shared_tests.mk
$(eval $(call C_VARIANT,build/obj/host,CC,HOST_FLAGS))
$(eval $(call C_VARIANT,build/obj/asan,CC,SAN_FLAGS))
$(eval $(call C_VARIANT,build/obj/o2,CC,O2_FLAGS))
$(call SHARED_TESTS_IN,,build/obj/host,CC,CFLAGS)
$(call SHARED_TESTS_IN,asan_,build/obj/asan,CC,ASAN_FLAGS)

# $(call prog,<name>,<what it is besides the kernel>,<more to link>): the
# program build/<name>, and its sanitized twin build/asan_<name>.
define PROG
build/$(1): $$(call c_objs,build/obj/host,$$(SRC) $(2))
	$$(CC) $$(CFLAGS) $$^ $(3) -lm -o $$@
build/asan_$(1): $$(call c_objs,build/obj/asan,$$(SRC) $(2))
	$$(CC) $$(ASAN_FLAGS) $$^ $(3) -lm -o $$@
endef
prog = $(eval $(call PROG,$(1),$(2),$(3)))

# One binary per test file.
$(foreach t,$(TESTS),$(call prog,$(t),tests/$(t).c))

run: $(addprefix build/,$(TESTS)) shared-tests build/ios_smoke
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
build/cn_scene_bench: $(call c_objs,build/obj/o2,tests/cn_scene_bench.c src/cn_scene.c)
	$(CC) $(O2_FLAGS) $^ -o $@
bench: build/cn_scene_bench
	./build/cn_scene_bench

# THE NAMES ON THE TABLE, OFFLINE (package N; not a test, not in `run`): a six-seat table through the stage with
# every name handed over as the iOS host hands it, laid over the planks, one name half behind a cup; and what the
# names cost a still frame at 2x. tools/cn_names_shot.c.
build/cn_names_shot: $(call c_objs,build/obj/o2,$(SRC) tools/cn_names_shot.c)
	$(CC) $(O2_FLAGS) $^ -lm -o $@
names-shot: build/cn_names_shot $(TEX_PACK)
	./build/cn_names_shot $(TEX_PACK) tools/fonts/IMFeENsc28P.ttf build/names_shot.png build/names_shot_zoom.png

# THE HUD OVER THE PICTURE, ON THE GLASS (package S; not a test, not in `run`): a frame turned as the host turns it,
# each counting die's face, the plate and the outcome line drawn over it. tools/cn_glass_shot.c.
build/cn_glass_shot: $(call c_objs,build/obj/o2,$(SRC) tools/cn_glass_shot.c)
	$(CC) $(O2_FLAGS) $^ -lm -o $@
glass-shot: build/cn_glass_shot $(TEX_PACK)
	for n in 2 4 6; do ./build/cn_glass_shot $(TEX_PACK) 390 718 $$n 2 build/glass_718_$$n.png; ./build/cn_glass_shot $(TEX_PACK) 390 340 $$n 2 build/glass_340_$$n.png; done

asan: $(addprefix build/asan_,$(TESTS)) shared-tests-asan build/asan_ios_smoke
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
# WASM_CC, the guard that refuses Apple clang and the `wasm` target itself:
# shared/tools/wasm_cc.mk.
include ../../shared/tools/wasm_cc.mk
$(eval $(call WASM_OBJECT_PROOF,$(SRC)))

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
# What compiles a file and what links the module, apart: the linker's flags
# mean nothing to a compile. The link is still handed every flag the single
# command had, so the driver does there exactly what it did.
ROLL_WASM_CFLAGS  := --target=wasm32 -mcpu=mvp -mbulk-memory -msign-ext -mnontrapping-fptoint -msimd128 -O3 -nostdlib -ffreestanding \
                     $(WARN) -isystem $(SHARED)/wasm/include -Isrc
ROLL_WASM_LDFLAGS := -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=65536 -Wl,--strip-all
$(eval $(call C_VARIANT,build/obj/roll,WASM_CC,ROLL_WASM_CFLAGS))
$(ROLL_WASM_OUT): $(call c_objs,build/obj/roll,$(ROLL_WASM_SRC)) Makefile
	@mkdir -p build/wasm
	$(WASM_CC) $(ROLL_WASM_CFLAGS) $(ROLL_WASM_LDFLAGS) $(filter %.o,$^) -o $@
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
IOS_CFLAGS := -O2 -Isrc -Iios -Iios/include $(WARN)

$(call prog,ios_smoke,ios/cn_api.c ios/cn_api_smoke.c)

ios-smoke: build/ios_smoke tex
	./build/ios_smoke build/cn_tex.pack

# THE GENERATED SWIFT. Swift never learns a byte layout: every struct
# cn_api.h hands over is read through readers shared/tools/structgen writes
# from the kernel's own headers (ios/layout.args), and every string key
# through shared/tools/datagen. A BUILD OUTPUT, never committed.
#
# The device is arm64 and the simulator arm64 or x86_64; one hash is stamped
# into all three slices, so all three must lay these structs out alike. Where
# the generators are, the layout rule and the slices themselves:
# shared/tools/gen_tools.mk and shared/tools/ios_xcframework.mk.
GEN              := ../ios/Generated
IOS_SG_SPEC      := ios/layout.args
IOS_KERNEL_SWIFT := $(GEN)/ChuiniuKernel.swift
include ../../shared/tools/gen_tools.mk
include ../../shared/tools/ios_xcframework.mk

structgen: build/layout/ios.hash

datagen: $(DATAGEN)
	$(call DATAGEN_I18N,CN,Chuiniu,$(GEN)/i18n)

# THE SWIFT SIDE, ON THIS MAC: the readers generated for this machine's
# triple, the kernel compiled for it and stamped with that layout's hash, and
# a Swift program that drives the bridge through them (ios/cn_api_smoke.swift).
swift-smoke: $(STRUCTGEN) datagen tex
	$(call IOS_SWIFT_SMOKE,ios/include/cn_api.h,ChuiniuKernel,$(GEN)/i18n,ios/cn_api_smoke.swift,build/cn_tex.pack)

# THE LIBRARY CARRIES THE LAYOUT IT WAS COMPILED FOR, and the readers are
# written beside it in the same run, so the pair cannot come apart.
# THE SCENE RENDERER (src/cn_scene.c, in SRC) is in the library too, and its
# header goes into the xcframework beside cn_api.h, so the bridge can include it
# and Swift reach it through module CChuiniu; after the build every slice is
# asked for its entry points, so a library without the renderer fails here, not
# in an app's link.
#
# The headers directory is assembled (the bridge's own and the renderer's), so
# it is a rule with a stamp, and the xcframework is made from the stamp: a
# header edit rewrites the directory, and the xcframework after it.
IOS_HEADERS := build/ios/include
IOS_SCENE_SYMBOLS := cn_scene_init cn_scene_reset cn_scene_tex_new cn_scene_begin cn_scene_occluder \
                     cn_scene_render cn_scene_prepare cn_scene_band cn_scene_fb cn_scene_frame_bytes
$(IOS_HEADERS)/.stamp: $(wildcard ios/include/*) src/cn_scene.h
	@rm -rf $(IOS_HEADERS) && mkdir -p $(IOS_HEADERS) && cp ios/include/* src/cn_scene.h $(IOS_HEADERS)/
	@touch $@
# The device slice, both simulator slices, the lipo and the xcframework are
# the shared recipe, and it rebuilds only what changed - which is what lets
# Xcode run this target on every build (shared/scripts/xcode_kernel.sh).
$(eval $(call IOS_XCFRAMEWORK,chuiniu,$(IOS_SRC),$(IOS_HEADERS),../ios/vendor/Chuiniu.xcframework,$(IOS_HEADERS)/.stamp))
ios-lib: datagen tex-ios $(IOS_XCFRAMEWORKS)
	@for a in build/ios/device/libchuiniu.a build/ios/sim-arm64/libchuiniu.a build/ios/sim-x86_64/libchuiniu.a; do \
	  for s in $(IOS_SCENE_SYMBOLS); do \
	    nm -g $$a 2>/dev/null | grep -q " T _$$s$$" || { echo "ios-lib: $$a does not define $$s"; exit 1; }; \
	  done; \
	done; echo "the scene renderer is in every slice ($(words $(IOS_SCENE_SYMBOLS)) entry points)"
	@echo "layout $$(cat build/layout/ios.hash)"

# ---- THE OFFLINE BOT AND ITS ARENA (bot/, ../docs/BOT.md) ------------------------
# Never shipped: bot/ is in no SRC list, so the wasm objects, the bridge and the
# xcframework never carry it, and it is not in `run` or `asan` (no workflow ran
# it before it was built here, and none does now).
#   make -C chuiniu/c bot-test        the bot's tests, built and run
#   make -C chuiniu/c bot-asan        the same under ASan + UBSan
#   make -C chuiniu/c bot-arena       the headline runs: bot vs random at 2 and 4 seats
#   make -C chuiniu/c bot-arena-fast  a smoke run of the same
#
# IT HAD ITS OWN MAKEFILE (bot/Makefile), which listed the kernel's headers by
# hand and compiled the kernel again from source for each program. It is built
# here now, through the same rules as everything else (c_rules.mk): an object
# knows what it included, and the three programs share one compile of the
# kernel files they link.
#
# ITS OWN TWO VARIANTS, because its flags are not the kernel's. WARN above
# carries -ffp-contract=off for cn_roll.c, and the bot was never built with it:
# every number in docs/BOT.md comes from a bot compiled without, and a fused
# multiply-add is exactly the kind of thing that moves a belief by one ulp and
# a close game by a seat. So the kernel files the bot links are compiled again
# under the bot's flags, in build/obj/bot, and the kernel's own objects are
# left out of it.
#
# THE KERNEL FILES IT LINKS ARE A SUBSET (BOT_KSRC), as they always were: the
# rules, the dice and the plan. Not SRC - the bot has no wire, no words and no
# picture, and a program that links what it does not use hides what it needs.
BOT_WARN       := -Wall -Wextra -Werror -std=c11
BOT_FLAGS      ?= -O2 $(BOT_WARN)
BOT_ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(BOT_WARN)
BOT_KSRC       := src/cn.c src/cn_dice.c src/cn_plan.c $(SHARED)/deal_rng.c $(SHARED)/sha256.c
BOT_SRC        := bot/cn_bot.c bot/cn_bot_random.c
BOT_LIBS       := -lm -lpthread
ARENA_GAMES    ?= 4000
ARENA_SEED     ?= 1
$(eval $(call C_VARIANT,build/obj/bot,CC,BOT_FLAGS))
$(eval $(call C_VARIANT,build/obj/botasan,CC,BOT_ASAN_FLAGS))

build/cn_bot_test: $(call c_objs,build/obj/bot,$(BOT_KSRC) $(BOT_SRC) bot/cn_bot_test.c)
	$(CC) $(BOT_FLAGS) $^ -o $@ $(BOT_LIBS)
build/asan_cn_bot_test: $(call c_objs,build/obj/botasan,$(BOT_KSRC) $(BOT_SRC) bot/cn_bot_test.c)
	$(CC) $(BOT_ASAN_FLAGS) $^ -o $@ $(BOT_LIBS)
build/cn_arena: $(call c_objs,build/obj/bot,$(BOT_KSRC) $(BOT_SRC) $(SHARED)/stats/stats.c bot/cn_arena.c)
	$(CC) $(BOT_FLAGS) $^ -o $@ $(BOT_LIBS)

bot-test: build/cn_bot_test
	./build/cn_bot_test

bot-asan: build/asan_cn_bot_test
	./build/asan_cn_bot_test

bot-arena: build/cn_arena
	./build/cn_arena --seats=2 --mix=bot,random --games=$(ARENA_GAMES) --seed=$(ARENA_SEED)
	./build/cn_arena --seats=4 --mix=bot,random,random,random --games=$(ARENA_GAMES) --seed=$(ARENA_SEED)

bot-arena-fast: build/cn_arena
	./build/cn_arena --fast --seats=2 --mix=bot,random
	./build/cn_arena --fast --seats=4 --mix=bot,random,random,random

.PHONY: bot-test bot-asan bot-arena bot-arena-fast

clean:
	rm -rf build

.PHONY: all run bench names-shot glass-shot asan wasm-roll docs-roll docs-roll-check ios-smoke swift-smoke structgen datagen ios-lib clean

# ---- THE TEXTURE PACK (package C) ----------------------------------------------
# The cups' and dice's textures are baked at BUILD time (a procedural texture made
# at launch once took the iMessage extension down on a device): tools/cn_texgen.c
# bakes the verdigris and bone tiles and the numerals 0-9 (Libre Caslon Text's
# lining figures, SIL OFL 1.1, tools/fonts/LibreCaslonText-wght.ttf with its licence
# OFL-LibreCaslonText.txt; Fell's old-style 5 read as a long s, docs_pkgQ.md) into build/cn_tex.pack, and src/cn_tex.c derives every seat's and
# die's texture from it at upload. Layout in src/cn_tex.h; notes in docs_pkgC.md.
#   make -C chuiniu/c tex          build/cn_tex.pack
#   make -C chuiniu/c tex-ios      ...and its copy in ../ios/ChuiniuKit/Resources/
#   make -C chuiniu/c tex-test     the pack's and the textures' tests (in `run`; tex-asan in `asan`)
#   make -C chuiniu/c tex-wasm     cn_tex.c for wasm32, freestanding (an object only)
#   make -C chuiniu/c tex-compare REF=dir   MAD against the study's captures, and a contact sheet
TEX_FONT := tools/fonts/LibreCaslonText-wght.ttf
TEX_PACK := build/cn_tex.pack
TEX_IOS  := ../ios/ChuiniuKit/Resources/cn_tex.pack

build/cn_texgen: $(call c_objs,build/obj/host,tools/cn_texgen.c src/cn_tex.c)
	$(CC) $(CFLAGS) $^ -lm -o $@

$(TEX_PACK): build/cn_texgen $(TEX_FONT)
	./build/cn_texgen --font $(TEX_FONT) --pack $@

tex: $(TEX_PACK)

# THE FLAT IMAGES the Swift host paints with (DECISIONS I27): the planks (the
# study's six-plank tile, nails baked in, 2 texels a point, 1032 by 1660), the
# verdigris tile (the plates and the buttons), the barnacle crust a plate wears
# in its corner (RGBA, 512 by 128) and the bone tile (the flat dice),
# baked by the same tool as uncompressed PNGs, recompressed by macOS's own sips
# (the planks to a JPEG at quality 80, about 185 KB; the tiles to deflated PNGs).
# And the two faces of IM Fell English (roman and small caps, SIL OFL 1.1, from
# google/fonts ofl/imfellenglish and ofl/imfellenglishsc; the licence is
# tools/fonts/OFL.txt, the same text for both), which ChuiniuKit registers at
# run time from its own bundle. All git-ignored copies, made here.
TEX_IMG   := build/img/cn_planks.png
TEX_FONTS := tools/fonts/IMFeENrm28P.ttf tools/fonts/IMFeENsc28P.ttf tools/fonts/OFL.txt
IOS_RES   := ../ios/ChuiniuKit/Resources
$(TEX_IMG): build/cn_texgen $(TEX_FONT)
	@mkdir -p build/img && ./build/cn_texgen --font $(TEX_FONT) --images build/img
build/img/cn_planks.jpg: $(TEX_IMG)
	sips -s format jpeg -s formatOptions 80 $< --out $@ >/dev/null
build/img/cn_verd_z.png: $(TEX_IMG)
	sips -s format png build/img/cn_verd.png --out $@ >/dev/null
build/img/cn_bone_z.png: $(TEX_IMG)
	sips -s format png build/img/cn_bone.png --out $@ >/dev/null
build/img/cn_crust_z.png: $(TEX_IMG)
	sips -s format png build/img/cn_crust.png --out $@ >/dev/null

# cmp-then-copy, so the resource's mtime moves only when its bytes do
cmpcp = { cmp -s $(1) $(2) && echo "$(2) is current" || { cp $(1) $(2); echo "wrote $(2)"; }; }
tex-ios: $(TEX_PACK) build/img/cn_planks.jpg build/img/cn_verd_z.png build/img/cn_bone_z.png build/img/cn_crust_z.png $(TEX_FONTS)
	@mkdir -p $(IOS_RES)
	@$(call cmpcp,$(TEX_PACK),$(TEX_IOS))
	@$(call cmpcp,build/img/cn_planks.jpg,$(IOS_RES)/cn_planks.jpg)
	@$(call cmpcp,build/img/cn_verd_z.png,$(IOS_RES)/cn_verd.png)
	@$(call cmpcp,build/img/cn_bone_z.png,$(IOS_RES)/cn_bone.png)
	@$(call cmpcp,build/img/cn_crust_z.png,$(IOS_RES)/cn_crust.png)
	@$(call cmpcp,tools/fonts/IMFeENrm28P.ttf,$(IOS_RES)/IMFeENrm28P.ttf)
	@$(call cmpcp,tools/fonts/IMFeENsc28P.ttf,$(IOS_RES)/IMFeENsc28P.ttf)
	@$(call cmpcp,tools/fonts/OFL.txt,$(IOS_RES)/OFL-IMFellEnglish.txt)

build/cn_tex_test: $(call c_objs,build/obj/host,tests/cn_tex_test.c src/cn_tex.c)
	$(CC) $(CFLAGS) $^ -lm -o $@

build/asan_cn_tex_test: $(call c_objs,build/obj/asan,tests/cn_tex_test.c src/cn_tex.c)
	$(CC) $(ASAN_FLAGS) $^ -lm -o $@

tex-test: build/cn_tex_test $(TEX_PACK)
	./build/cn_tex_test $(TEX_FONT) $(TEX_PACK)

tex-asan: build/asan_cn_tex_test $(TEX_PACK)
	./build/asan_cn_tex_test $(TEX_FONT) $(TEX_PACK)

run: tex-test
asan: tex-asan

tex-wasm: wasm-cc-check $(call c_objs,build/wasm,src/cn_tex.c)

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

$(call prog,cn_icon,tools/cn_icon.c,-lz)

icons: build/cn_icon $(TEX_PACK)
	./build/cn_icon --pack $(TEX_PACK) --ext "$(ICON_EXT)" --app "$(ICON_APP)" --sheet build/cn_icon_sheet.png

.PHONY: icons
