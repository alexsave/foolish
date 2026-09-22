CC      ?= clang

# THE TWO SHARED PRIMITIVES. sha256 and deal_rng are compiled from ../shared/c
# by this product and by werewolf alike - a standard and a pure function, with
# no game in either. Everything else under src/ is foolish's.
SHARED  := ../shared/c

# A literal comma, so $(filter-out) patterns can name a -Wl,--flag. Defined here
# rather than beside its first user: := expands immediately, so a definition
# further down the file silently expands to nothing at every earlier use, and
# the msg target's memory-pin filter spent its whole life being a no-op that
# emitted BOTH pins and let the later one win.
comma := ,
# -flto links the engine, legal-move generator and bots as one unit so the
# tiny per-ply leaf calls (can_cover, game_done, should_bot_act, card_eq,
# hand_remove_card — 100M–300M calls in a cordite eval) inline into the rollout
# hot loops instead of being cross-translation-unit calls. ~1.5x faster cordite
# eval, bit-identical play (verified: same mean_finish / win_rate / histogram,
# CD_DIFFTEST clean). The win grows with player count.
CFLAGS  ?= -O3 -ffast-math -flto -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Isrc -DACCELERATE_NEW_LAPACK
ifeq ($(shell uname),Darwin)
  LDFLAGS ?= -lm -flto -framework Accelerate
else
  LDFLAGS ?= -lm -flto
endif

# Parallel eval: `make OMP=1 ...` fans the independent games of cnitro_eval
# across cores (the engine RNG and all cordite scratch state are _Thread_local,
# so games don't race; results are identical to serial). Off by default so a
# plain `make` needs no libomp. On macOS/clang you'll also want libomp
# (brew install libomp) — gcc has it built in.
ifeq ($(OMP),1)
  CFLAGS  += -fopenmp
  LDFLAGS += -fopenmp
else
  # ...and when it is NOT parallel, the qualifier is pure cost. Darwin has no
  # local-exec TLS model: on arm64 every read of a _Thread_local compiles to a
  # call through the variable's thread-local descriptor into ___tlv_get_addr,
  # and the solver reads its scratch (sim_rec_moves, sim_rec_child, VALUE_MASK,
  # the TT pointer) several times per node. Measured with /usr/bin/sample over
  # build/cnitro_tests: 2,814 of 18,554 samples - 15% of the whole binary -
  # were inside that thunk, 1,428 of them from sim_solve_rec alone. Removing it
  # took `make tests` from 27.7s to 24.0s with every one of the 5,761 checks
  # passing and every counter in the output identical.
  #
  # Safe exactly while nothing here runs two threads, which is the same
  # condition OMP=1 turns off: the only pthreads in the tree are the analyser's
  # (src/analyse.c, AnalyseParams.threads), and the two targets that pass
  # threads > 1 build their own objects from CORE_SRC and filter this back out
  # - see ANALYSE_CFLAGS. The wasm bots module has neutralised the qualifier
  # the same way since it was written (WASM_BOT_CFLAGS below), so this is the
  # build the shipped module already gets, not a new experiment.
  CFLAGS  += -D_Thread_local=
endif

# LEAFBOOK (c/LEAFBOOK.md, docs/L1_SPEND_PLAN.md §4): compile octogen's
# precomputed endgame oracle into every native binary. octogen enables it
# per-decision (cd_sim_set_leafbook); other solver callers that never enable it
# are byte-identical. Also added to WASM_BOT_CFLAGS so production octogen gets
# the ~15% e2e p50 speedup. The generated book (src/leafbook_data.h) is
# committed; regenerate it with `make leafbook` after an endgame-solver change.
CFLAGS += -DCD_LEAFBOOK

# The engine + all heuristic / Monte-Carlo bots. Linked into every binary.
CORE_SRC := src/game.c $(SHARED)/deal_rng.c src/legal.c src/replay.c src/replay_steps.c \
            src/replay_extras.c src/view.c src/awire.c \
            src/evwire.c src/random_strategy.c src/espresso_strategy.c \
            src/handwritten_strategy.c src/robusta_strategy.c \
            src/firecracker_strategy.c src/gunpowder_strategy.c \
            src/blackpowder_strategy.c src/cordite_strategy.c \
            src/cordite_sim.c src/astrolite_strategy.c \
            src/simple_heuristic_strategy.c src/champion_strategy.c \
            src/ultimate_champion_strategy.c src/hacker_strategy.c \
            src/fulminate_strategy.c src/espresso_prod_strategy.c \
            src/handwritten_prod_strategy.c src/distill_feat.c \
            src/distilled_strategy.c src/semtex_strategy.c \
            src/octogen_strategy.c src/torpex_strategy.c \
            src/torpex_value.c src/novichok_strategy.c \
            src/bot_knobs.c src/bot_roster.c src/bot_drive.c \
            $(SHARED)/sha256.c src/msg_wire.c src/msg_expand.c src/anim_plan.c \
            src/analyse.c src/roster.c src/table.c src/client_table.c src/card.c
# BY BASENAME, not by stripping src/: the sources come from two directories now,
# and the old `$(CORE_SRC:src/%.c=build/%.o)` left the shared pair as .c paths -
# which still links, from source, silently, while the pattern rule below never
# fires.
CORE_OBJ := $(addprefix build/,$(notdir $(CORE_SRC:.c=.o)))

.PHONY: all clean tests print-core print-wasm-src
# Emit the core engine+strategy source list (used by tools/tt_divergence.sh to
# build cnitro_eval at arbitrary -DCD_TT_BITS without duplicating the list).
print-core:
	@echo $(CORE_SRC)
# Emit every C source compiled into a SHIPPED wasm module, for
# scripts/wasm_stamp.sh. Read out of the real WASM_*_SRC lists rather
# than copied into the script: a second copy of this list is how the artifact
# and the sources it is built from drift apart in the first place. Not the same
# set as CORE_SRC - the native CLI mains (main_elo.c, main_showcase.c) are in
# no wasm module, and a change to one of them must not be read as a stale
# artifact.
print-wasm-src:
	@echo $(sort $(WASM_BOT_SRC) $(WASM_ORACLE_MT_SRC))
all: build/cnitro_eval build/cnitro_elo build/cnitro_replay build/cnitro_showcase build/cnitro_tests build/cnitro_gen

# Generic compile rule for sources under src/ (build/%.o: src/%.c).
# Blanket header dependency: a struct-layout change in any header must
# rebuild every object, or the binaries mix layouts (observed: SimState
# edits producing garbage evals until a make clean).
build/%.o: src/%.c $(wildcard src/*.h) $(wildcard $(SHARED)/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: $(SHARED)/%.c $(wildcard src/*.h) $(wildcard $(SHARED)/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/cnitro_eval: $(CORE_OBJ) build/main_eval.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_elo: $(CORE_OBJ) build/main_elo.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_replay: $(CORE_OBJ) build/main_replay.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_showcase: $(CORE_OBJ) build/main_showcase.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_tests: $(CORE_OBJ) build/tests.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_distill: $(CORE_OBJ) build/main_distill.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
build/cnitro_gen: $(CORE_OBJ) build/main_gen.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Same blanket header dependency as src objects: a struct-layout change must
# rebuild the tests or the binary mixes layouts (observed: 1-byte Card pack).
build/tests.o: tests/tests.c $(wildcard src/*.h) $(wildcard $(SHARED)/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

# 5,761 checks in 23.8s at -O3, and 23.5s of that is ONE of the 204 tests:
# test_table_session_log_is_load_bearing_for_octogen, 3,430 octogen decisions at
# 6.9ms each (half of them the memory-taken-away run that IS the experiment).
# /usr/bin/sample puts all of it in sim_solve_rec / sim_gen_moves / cover_assign
# - no per-decision setup, no rebuild, no allocation - so the only lever left on
# it is fewer decisions, which is sample size and not waste.
#
# Where that one test should live is an open question for a human, and here are
# the two measurements it needs. No workflow runs THIS target; the only CI lane
# that runs the native suite is coverage.yml, which builds it at -O0 --coverage
# (and without -DCD_LEAFBOOK, so octogen there is not the octogen that ships).
# That binary is 81.6s with the test and 11.8s without it - the experiment is
# 69.8s of a coverage job, several times that on a CI runner. And it buys the
# gate nothing: game.c / legal.c / replay.c come to 2163 of 2304 lines with it
# and 2163 of 2304 without it, the identical missing-line list. So the useful
# move is a lane that runs `make tests` at the flags the product ships, and then
# leaving the experiment out of the coverage build - not one without the other,
# since dropping it from coverage today would leave it running in no CI at all.
tests: build/cnitro_tests
	./build/cnitro_tests

# The same suite under AddressSanitizer + UBSan, built in one shot at -O1 (not
# against CORE_OBJ, whose objects are -O3 -flto and not instrumented).
# This is where a read past the end of a caller's buffer stops being invisible:
# the wire decoders take byte counts off the network and off a DB row, and a
# test that hands one a truncated payload only proves anything if the buffer is
# exactly that long and on the heap - see tests.c
# test_state_import_refuses_a_truncated_payload. Not part of `make tests`: it is
# several times slower and the full-game loops dominate it.
# object-size is off: replay.c's DealSlot is a Game DELIBERATELY shorter than a
# Game (see deal_reveals_from_seed, which already turns gcc's -Warray-bounds off
# for the same reason), and that check flags the intent rather than a bug.
ASAN_CFLAGS := -fsanitize=address,undefined -fno-sanitize=object-size \
               -fno-omit-frame-pointer -fno-sanitize-recover=undefined \
               -g -O1 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Isrc -DCD_LEAFBOOK
.PHONY: tests-asan
tests-asan:
	@mkdir -p build
	$(CC) $(ASAN_CFLAGS) $(CORE_SRC) tests/tests.c -o build/cnitro_tests_asan -lm
	./build/cnitro_tests_asan

# Bitboard-rollout difftests (not part of the default suite). Each plays many
# games and asserts the bitboard engine matches the struct engine.
build/sim_difftest: tests/sim_difftest.c $(CORE_OBJ)
	$(CC) $(CFLAGS) tests/sim_difftest.c $(CORE_OBJ) -o $@ $(LDFLAGS)
build/apply_difftest: tests/apply_difftest.c $(CORE_OBJ)
	$(CC) $(CFLAGS) tests/apply_difftest.c $(CORE_OBJ) -o $@ $(LDFLAGS)
build/solver_difftest: tests/solver_difftest.c $(CORE_OBJ)
	$(CC) $(CFLAGS) tests/solver_difftest.c $(CORE_OBJ) -o $@ $(LDFLAGS)
# Replay-codec round trip against real engine games (the native mirror of
# e2e/replay_codec.test.ts): losslessness, mid-game cuts, size
# (docs/IMESSAGE_GAME_DESIGN.md §16 option 3). It absorbed the old
# replay_difftest, which drove the same games through the retired retrodiction
# encoder and asserted the same full-stream fidelity.
build/replay_v6_test: tests/replay_v6_test.c $(CORE_OBJ)
	$(CC) $(CFLAGS) tests/replay_v6_test.c $(CORE_OBJ) -o $@ $(LDFLAGS)
# FMSG v1 envelope (the iMessage payload): round-trip, replay fidelity, tamper
# matrix, size guardrail (docs/IMESSAGE_GAME_DESIGN.md §4/§7, src/msg_wire.h).
build/msg_wire_test: tests/msg_wire_test.c $(CORE_OBJ)
	$(CC) $(CFLAGS) tests/msg_wire_test.c $(CORE_OBJ) -o $@ $(LDFLAGS)
# Animation core (c/src/anim_plan.h): the C twins of the React animation-quality
# tests (e2e/optimistic_animation, optimistic_revert, reconcile) plus plan
# building. The owner's headline ask — the quality tests live in C now.
# The name-entry drawer decision (c/src/msg_expand.h): the C home of the eight
# cold opens that measured WHEN Messages honours an `.expanded` request. A pure
# transition function, so no game and no arguments - it either matches the
# filmed trace or it does not.
build/msg_expand_test: tests/msg_expand_test.c $(CORE_OBJ)
	$(CC) $(CFLAGS) tests/msg_expand_test.c $(CORE_OBJ) -o $@ $(LDFLAGS)
build/anim_plan_test: tests/anim_plan_test.c $(CORE_OBJ)
	$(CC) $(CFLAGS) tests/anim_plan_test.c $(CORE_OBJ) -o $@ $(LDFLAGS)
difftests: build/sim_difftest build/apply_difftest build/msg_wire_test build/solver_difftest build/replay_v6_test build/anim_plan_test build/msg_expand_test
	./build/sim_difftest 4 1000 && ./build/apply_difftest 4 200 && \
	./build/msg_wire_test 20 && \
	./build/msg_expand_test && \
	./build/anim_plan_test && \
	./build/solver_difftest 2 40 "" 200000 && \
	./build/solver_difftest 3 40 "" 200000 && \
	./build/solver_difftest 4 40 "" 200000 && \
	./build/replay_v6_test 40 && \
	$(MAKE) analyse-test && \
	$(MAKE) msg-flow-sim

# ---------- the post-game analyser (docs/POST_GAME_ANALYSER.md) ---------------
#
# Built at the iOS/production caps, in one shot, never against CORE_OBJ: the
# native arena caps drop legal moves past MAX_LEGAL_MOVES / MAX_MOVE_CARDS
# silently in enumeration order, and an analyser that enumerates every legal
# move must not have the winning one dropped before it is scored. A LegalMoves
# at these caps is ~240KB, so nothing here puts one on the stack.
#
#   make analyse          build/cnitro_analyse, the CLI (--threads=N fans the
#                         world loop over N pthreads; same bytes at any N)
#   make analyse-test     build + run tests/analyse_test.c on the recorded game
#   make winprob          build/cnitro_winprob, the win-probability strip: the
#                         same rebuild, belief and playout, measured for EVERY
#                         seat at EVERY step rather than per decision
# The ONE native product that really does run the engine on several threads, so
# it is the one that keeps real thread-local storage: --threads=N fans the world
# loop over N pthreads and every world plays games through game.c's RNG and the
# cordite solver's scratch. The default build neutralises _Thread_local for the
# 15% it costs on Darwin (see OMP above); here the qualifier is load-bearing and
# is filtered back in. Same idiom as WASM_ORACLE_MT_CFLAGS.
ANALYSE_CFLAGS = $(filter-out -D_Thread_local=,$(CFLAGS)) $(IOS_CAPS)
.PHONY: analyse analyse-test winprob
analyse:
	@mkdir -p build
	$(CC) $(ANALYSE_CFLAGS) $(CORE_SRC) src/main_analyse.c -o build/cnitro_analyse $(LDFLAGS)
winprob:
	@mkdir -p build
	$(CC) $(ANALYSE_CFLAGS) $(CORE_SRC) src/main_winprob.c -o build/cnitro_winprob $(LDFLAGS)
analyse-test:
	@mkdir -p build
	$(CC) $(ANALYSE_CFLAGS) $(CORE_SRC) tests/analyse_test.c -o build/analyse_test $(LDFLAGS)
	./build/analyse_test

# ---------- iOS static library / xcframework (docs/IOS_APP_DESIGN.md §16.A1) --
#
# Builds ../ios/vendor/Foolish.xcframework from the SAME CORE_SRC every native
# binary uses, plus the Swift-visible bridge ($(IOS_BRIDGE_SRC) +
# ios/ios_bots_api.c, declared by ios/include/ios_api.h). The BOT half is its
# own object on purpose: this archive links per object, so keeping every bot
# reference in ios_bots_api.c is what lets a binary that never drives a bot
# leave the strategy ladder behind (see ios/ios_internal.h). The core half is
# several objects for a different reason - it is split by domain, which the
# linker does not care about either way.
#
# This is macOS + Xcode ONLY (needs xcrun/libtool/xcodebuild and
# the iOS SDKs). On Linux/CI, `make ios-smoke` exercises the identical bridge
# natively (clang) — it is the portable proof that new->legal->apply->state and
# a full game run through the Swift entry points.
#
# Caps: the wide human-game caps the production wasm rules module uses
# (MAX_BATTLES=64 etc.), because human games reach states the compact arena
# never does (huge post-pickup hands). CD_LEAFBOOK is required by octogen
# (src/leafbook_data.h is committed). NO -flto here — the xcframework is linked
# by Xcode, and per-TU objects keep the archive portable across Xcode versions.
IOS_MIN_VERSION := 15.0
IOS_CAPS   := -DMAX_LOG_PAIRS=64 -DMAX_BATTLES=64 -DMAX_MOVE_CARDS=28 \
              -DMAX_LEGAL_MOVES=4096 -DCD_LEAFBOOK \
              -DREPLAY_REC_CAP=4096 -DREPLAY_MAX_ACTIONS=1024
# The two replay caps above are the iMessage extension's, and they are here
# because an iMessage extension is memory-capped far below an app: these two
# arenas are 1.1 MB of the bridge's dirty pages, which is 45% of them.
#
# REPLAY_REC_CAP: replay.c's own default is 65536 and iOS was the LAST build
# still taking it - the shipped wasm modules have run 4096 for a long time.
# Measured peak coder choices over 111,977 games is 719, a 5.7x margin, and an
# overflow is a clean REPLAY_ECAP rather than a corruption. Shrinks g_rec
# 786,432 -> 49,152 and the derived g_bn 172,040 -> 10,760.
#
# REPLAY_MAX_ACTIONS: see the note in replay_steps.h - this is the ceiling the
# code can reach, not a sample. MAX_LOGS is 1024 here, an action stream is at
# most MAX_LOGS-1 long, so 1024 cannot be overrun by any encodable game.
# Observed peak 497.
#
# Neither reaches the server, the wasm modules or the test suites: IOS_CAPS is
# read only by the iOS slices and the native `analyse` dev tool.
# TWO libraries, not one, and this is the whole reason the C bridge is two
# translation units. Foolish.xcframework is STATIC, so ld pulls it in per
# object: while one archive held both halves, FoolishKit (a dynamic framework
# with a public botDrive on EngineC) exported a bot entry, which is a dead-strip
# root, which linked bot_drive -> bot_roster -> all 21 strategies including
# octogen. FoolishKit ships inside FoolishMessagesApp, so octogen shipped in the
# iMessage bundle.
#
# Split at the archive, the ladder has nowhere to ride. The extension links only
# the core; the host app links both.
#
# The split is BY REFERENCE, not by taste: the core set below names no strategy
# symbol at all (random_strategy_set_seed is defined in game.c, not a strategy
# file), so nothing in it can drag one in.
#
# THE BRIDGE'S OWN CORE HALF is split again, by DOMAIN rather than by link
# reachability: 123 entry points at about nine lines each is wide, not deep, so
# they sort by what each one is ABOUT (ios/ios_internal.h says which is which).
# Every one of these files must keep naming no strategy symbol, and
# ios/split_check.sh asserts it on each object separately - a bot call put back
# into ANY of them is caught rather than hidden by the others.
IOS_BRIDGE_SRC := ios/ios_api.c ios/ios_api_view.c ios/ios_api_play.c \
                  ios/ios_api_anim.c ios/ios_api_replay.c \
                  ios/ios_api_identity.c ios/ios_api_msg.c
IOS_CORE_SRC := src/game.c $(SHARED)/deal_rng.c src/legal.c src/replay.c \
                src/replay_steps.c src/replay_extras.c src/view.c src/awire.c \
                src/evwire.c $(SHARED)/sha256.c src/msg_wire.c src/msg_expand.c \
                src/anim_plan.c src/roster.c src/card.c src/client_table.c \
                $(IOS_BRIDGE_SRC)
IOS_BOTS_SRC := $(filter-out $(IOS_CORE_SRC),$(CORE_SRC)) ios/ios_bots_api.c

# Everything, for the host-side smoke/golden binaries that want one link line.
IOS_SRC    := $(IOS_CORE_SRC) $(IOS_BOTS_SRC)
# The bots headers sit one level down, in a directory named for their clang
# module, because Xcode copies BOTH xcframeworks' headers into the one
# $(BUILT_PRODUCTS_DIR)/include: two files called module.modulemap there is
# "Multiple commands produce". Nested, the bots map lands at
# include/CFoolishBots/module.modulemap, and the FoolishBots target names that
# directory in HEADER_SEARCH_PATHS (ios/project.yml).
IOS_CFLAGS := -O2 -Isrc -Iios/include -Iios/include-bots/CFoolishBots $(IOS_CAPS)

# $(1)=target-triple  $(2)=sdk name  $(3)=output dir  $(4)=lib name  $(5)=sources
define IOS_BUILD_SLICE
	@rm -rf $(3)/obj-$(4)
	@mkdir -p $(3)/obj-$(4)
	@for s in $(5); do \
	  o=$(3)/obj-$(4)/$$(basename $$s .c).o; \
	  echo "  CC[$(2)] $$s"; \
	  xcrun -sdk $(2) clang -target $(1) $(IOS_CFLAGS) -c $$s -o $$o || exit 1; \
	done
	libtool -static -o $(3)/lib$(4).a $(3)/obj-$(4)/*.o
endef

# Both libraries for one slice.
define IOS_BUILD_BOTH
	$(call IOS_BUILD_SLICE,$(1),$(2),$(3),foolish,$(IOS_CORE_SRC))
	$(call IOS_BUILD_SLICE,$(1),$(2),$(3),foolishbots,$(IOS_BOTS_SRC))
endef

.PHONY: ios-lib ios-smoke
# THE LIBRARY CARRIES THE LAYOUT IT WAS COMPILED FOR. sdk/swift/gen is generated
# from these same headers under these same flags; a library built from different
# ones would read right at every offset the two happen to share and wrong at the
# rest. Stamped only here, because only the shipped slices are read by the Swift
# bindings - ios-smoke, ios-goldens and ios-archives build the same sources with
# the HOST compiler and need no libclang on the machine that runs them.
ios-lib: IOS_CFLAGS += -DSG_LAYOUT_HASH=$$(cat build/layout/ios.hash)
ios-lib: build/layout/ios.hash
	@echo "== device slice (arm64) =="
	$(call IOS_BUILD_BOTH,arm64-apple-ios$(IOS_MIN_VERSION),iphoneos,build/ios/device)
	@echo "== simulator slices (arm64 + x86_64) =="
	$(call IOS_BUILD_BOTH,arm64-apple-ios$(IOS_MIN_VERSION)-simulator,iphonesimulator,build/ios/sim-arm64)
	$(call IOS_BUILD_BOTH,x86_64-apple-ios$(IOS_MIN_VERSION)-simulator,iphonesimulator,build/ios/sim-x86_64)
	@mkdir -p build/ios/sim
	lipo -create build/ios/sim-arm64/libfoolish.a build/ios/sim-x86_64/libfoolish.a \
	     -output build/ios/sim/libfoolish.a
	lipo -create build/ios/sim-arm64/libfoolishbots.a build/ios/sim-x86_64/libfoolishbots.a \
	     -output build/ios/sim/libfoolishbots.a
	@rm -rf ../ios/vendor/Foolish.xcframework ../ios/vendor/FoolishBots.xcframework
	@mkdir -p ../ios/vendor
	xcodebuild -create-xcframework \
	  -library build/ios/device/libfoolish.a -headers ios/include \
	  -library build/ios/sim/libfoolish.a    -headers ios/include \
	  -output ../ios/vendor/Foolish.xcframework
	xcodebuild -create-xcframework \
	  -library build/ios/device/libfoolishbots.a -headers ios/include-bots \
	  -library build/ios/sim/libfoolishbots.a    -headers ios/include-bots \
	  -output ../ios/vendor/FoolishBots.xcframework
	@echo "wrote ../ios/vendor/Foolish.xcframework + FoolishBots.xcframework"
	@./ios/archive_check.sh build/ios/device/libfoolish.a build/ios/device/libfoolishbots.a

# The two shipped iOS libraries, built and checked WITHOUT a Mac.
#
# `make ios-lib` needs xcrun/libtool/xcodebuild; this builds the same two source
# lists with the host compiler into the same two archives, so the split's real
# invariants are testable on Linux and in CI:
#   1. the CORE archive defines no strategy-ladder symbol (ios/archive_check.sh)
#   2. core + bots still share ONE resident game (ios/one_game_check.c)
# (1) is the one that keeps octogen out of the iMessage bundle. (2) is what
# would break silently if the split were done carelessly.
#
# THE OBJECT DIRECTORIES ARE EMPTIED FIRST, here and in IOS_BUILD_SLICE, because
# both archives are made by globbing *.o: a source that MOVES from one list to
# the other leaves its old object behind in the directory it left, the glob
# picks it up, and the two archives then define the same symbols twice. Moving
# client_table.c into the core list is exactly that, and "ld: 16 duplicate
# symbols" is what it says when nothing sweeps.
# `ar` on a Mac is not necessarily Apple's: a homebrew binutils on PATH writes a
# GNU-format archive that ld(1) refuses ("archive member '/' not a mach-o
# file"). Apple's libtool writes what its linker reads, and is what ios-lib
# already uses; elsewhere plain ar is right. Both take -o/out then inputs.
IOS_ARCHIVE := $(if $(filter Darwin,$(shell uname -s)),libtool -static -o,ar rcs)

.PHONY: ios-archives
ios-archives:
	@rm -rf build/ios-a/core build/ios-a/bots
	@mkdir -p build/ios-a/core build/ios-a/bots
	@for s in $(IOS_CORE_SRC); do \
	  $(CC) $(IOS_CFLAGS) -c $$s -o build/ios-a/core/$$(basename $$s .c).o || exit 1; done
	@for s in $(IOS_BOTS_SRC); do \
	  $(CC) $(IOS_CFLAGS) -c $$s -o build/ios-a/bots/$$(basename $$s .c).o || exit 1; done
	@$(IOS_ARCHIVE) build/ios-a/libfoolish.a     build/ios-a/core/*.o
	@$(IOS_ARCHIVE) build/ios-a/libfoolishbots.a build/ios-a/bots/*.o
	@printf 'core %s B   bots %s B\n' \
	  $$(wc -c < build/ios-a/libfoolish.a) $$(wc -c < build/ios-a/libfoolishbots.a)
	./ios/archive_check.sh build/ios-a/libfoolish.a build/ios-a/libfoolishbots.a
	$(CC) $(IOS_CFLAGS) ios/one_game_check.c \
	  build/ios-a/libfoolishbots.a build/ios-a/libfoolish.a -lm -o build/ios-a/one_game
	./build/ios-a/one_game

# Portable bridge smoke test (Linux/CI-friendly). Compiles the bridge + core
# fresh at the iOS caps (self-consistent struct layout — never mix cap sets)
# and drives a full 4-player game to a fool through the Swift-facing entry
# points. Fast (< 1s). This is the M-A keystone check runnable without a Mac.
ios-smoke:
	@mkdir -p build/ios-split
	@for s in $(IOS_BRIDGE_SRC); do \
	  $(CC) $(IOS_CFLAGS) -c $$s -o build/ios-split/$$(basename $$s .c).o || exit 1; done
	./ios/split_check.sh build/ios-split/*.o
	$(CC) $(IOS_CFLAGS) ios/ios_api_smoke.c $(IOS_BRIDGE_SRC) ios/ios_bots_api.c $(CORE_SRC) -lm -o build/ios_smoke
	./build/ios_smoke

# PART OF `difftests`, deliberately. Left out of it, this target rotted twice
# over without a word: fio_msg_encode's roster went packed and the file stopped
# COMPILING, and before that fio_msg_pack's blob grew ten bytes in front of the
# roster and the file kept reading the old offset. Both are cheap to notice and
# expensive to leave - the second one made every schedule stop at the lobby and
# report all four invariants clean. 0.3s a run against the kernel's own gates is
# not a price worth deferring.
#
# Monte-Carlo the iMessage SWIFT layer's decisions over the real kernel
# (tests/msg_flow_sim.c): random 4/8/9-human schedules with racing joins,
# racing Starts, stale taps and evaporated sends, checking safety (nobody ever
# renders a seat that isn't theirs), exclusion, convergence and liveness.
# Runnable without a Mac; the file header lists the Swift functions it ports
# line for line — keep them in step.
.PHONY: msg-flow-sim
msg-flow-sim:
	@mkdir -p build
	$(CC) $(IOS_CFLAGS) tests/msg_flow_sim.c $(IOS_BRIDGE_SRC) ios/ios_bots_api.c $(CORE_SRC) -lm -o build/msg_flow_sim
	./build/msg_flow_sim 300 9 0
	./build/msg_flow_sim 300 8 1
	./build/msg_flow_sim 300 4 1

# The online packed-view decode is now pure Swift (MaskedView) — see
# ios/FoolishTests/PackedViewTests.swift, which replaces the retired C harness.

# Golden fixtures for the Swift suite (§16.A3), both written from the SAME
# bridge the app links. Regenerate after any engine/bridge change and commit the
# result; the Swift tests then prove the native build reproduces them byte for
# byte, and CI (.github/workflows/ios.yml) fails if either is stale.
#
#   goldens.json          EngineGoldenTests - deal/menu/playthrough hashes.
#                         JSON only because it predates the packed-wire rule;
#                         do not grow it, and do not take it as precedent.
#   action_goldens.bin    PackedActionTests - the awire action frame the kernel
#                         writes for a fixed set of moves, PACKED (see
#                         ios/ios_action_goldens.c for the layout).
#   table_goldens.bin     TableWireTests - the one table layout the kernel
#                         writes for a fixed set of boards, masked cells
#                         included, PACKED (see ios/ios_table_goldens.c).
.PHONY: ios-goldens
ios-goldens:
	@mkdir -p build ../ios/Fixtures
	$(CC) $(IOS_CFLAGS) ios/ios_goldens.c $(IOS_BRIDGE_SRC) ios/ios_bots_api.c $(CORE_SRC) -lm -o build/ios_goldens
	./build/ios_goldens > ../ios/Fixtures/goldens.json
	@echo "wrote ../ios/Fixtures/goldens.json"
	$(CC) $(IOS_CFLAGS) ios/ios_action_goldens.c $(IOS_BRIDGE_SRC) ios/ios_bots_api.c $(CORE_SRC) -lm -o build/ios_action_goldens
	./build/ios_action_goldens > ../ios/Fixtures/action_goldens.bin
	@echo "wrote ../ios/Fixtures/action_goldens.bin"
	$(CC) $(IOS_CFLAGS) ios/ios_table_goldens.c $(IOS_BRIDGE_SRC) ios/ios_bots_api.c $(CORE_SRC) -lm -o build/ios_table_goldens
	./build/ios_table_goldens > ../ios/Fixtures/table_goldens.bin
	@echo "wrote ../ios/Fixtures/table_goldens.bin"

# L1 memory budget measurement (docs/WASM_L1_BUDGET.md). Plays ~63K real games
# and reports the observed maxima that size every wasm static buffer. Built at
# the WASM cap set + headroom so the numbers are true maxima; -DREPLAY_STATS
# turns on the (otherwise compiled-out) replay coder peak counters.
# `make l1-measure GAMES=1500` runs the sweep across seeds.
GAMES ?= 1500
build/l1_measure: tests/l1_measure.c $(wildcard src/*.h) $(wildcard $(SHARED)/*.h) src/game.c $(SHARED)/deal_rng.c src/legal.c src/replay.c src/view.c src/random_strategy.c src/handwritten_strategy.c
	@mkdir -p build
	$(CC) -O2 -Isrc -DMAX_LOG_PAIRS=64 -DMAX_BATTLES=64 -DMAX_LOGS=2048 \
	  -DMAX_LEGAL_MOVES=16384 -DMAX_MOVE_CARDS=52 -DREPLAY_STATS \
	  src/game.c $(SHARED)/deal_rng.c src/legal.c src/replay.c src/view.c \
	  src/random_strategy.c src/handwritten_strategy.c tests/l1_measure.c -o $@ -lm
.PHONY: l1-measure
l1-measure: build/l1_measure
	./build/l1_measure $(GAMES) 1 && ./build/l1_measure $(GAMES) 99991

# LEAFBOOK (docs/L1_SPEND_PLAN.md §4, S3). `make leafbook` regenerates the
# committed src/leafbook_data.h: enumerate every canonical <=K-card round-boundary
# endgame, solve each exactly, emit a key-sorted book. `make leafbook-gate` runs
# the enumeration feasibility census; `make leafbook-verify` runs the V-book
# value-safety gate (offline canonicalization + in-engine probe, must be 100%).
# The build_book pass compiles WITHOUT -DCD_LEAFBOOK (it builds the book by
# direct solves); verify_book compiles WITH it to exercise the engine probe.
LEAFBOOK_K ?= 4
LEAFBOOK_CFLAGS := -O2 -ffast-math -Isrc -Wno-deprecated-declarations -DCD_TT_BITS=12
.PHONY: leafbook leafbook-gate leafbook-verify
leafbook:
	@mkdir -p build
	$(CC) $(LEAFBOOK_CFLAGS) -DLEAFBOOK_BUILD_K=$(LEAFBOOK_K) $(CORE_SRC) tools/leafbook/build_book.c -o build/leafbook_build -lm
	./build/leafbook_build > src/leafbook_data.h
leafbook-gate:
	@mkdir -p build
	$(CC) -O2 -Isrc tools/leafbook/enumerate.c -o build/leafbook_enum && ./build/leafbook_enum
leafbook-verify:
	@mkdir -p build
	$(CC) $(LEAFBOOK_CFLAGS) -DCD_LEAFBOOK $(CORE_SRC) tools/leafbook/verify_book.c -o build/leafbook_verify -lm
	./build/leafbook_verify 1000000

# ---------------------------------------------------------------------------
# The `og_explain` native target and its c/tools/og_explain python pipeline are
# gone. They re-dealt a game from a 64-hex seed and X-rayed octogen's
# deliberation into a static HTML page. The kernel does that job directly now,
# and does it from a share link with no seed at all: replay_steps_board_v6 /
# replay_steps_memory_v6 (src/replay_steps.h) take the board and the public log
# straight out of a v6 code, which is what the Infinite Oracle (`wasm-oracle`
# below, public/oracle.wasm.gz, src/oracle/) reads to show the same
# per-candidate deliberation live in the browser, and what `make analyse`
# reads to review every legal move at every decision (docs/POST_GAME_ANALYSER.md).
# The python drivers were a second way to a place the kernel already reaches.
# -DOG_EXPLAIN_BUILD lives on: it is what the oracle builds are made of.

# ---------------------------------------------------------------------------
# WebAssembly rules kernel: game.c + legal.c + the wasm bridge, compiled
# freestanding (no libc; wasm/include has the tiny shims, wasm_api.c provides
# memcpy/memset). This is THE production rules engine: the TS server imports
# the resulting module via sdk/ts/wasm/. `-D_Thread_local=`
# strips TLS (single-threaded module); the bridge sets the production
# deck-size boundary at init (see card.h).
WASM_CC    ?= clang
# Optimize for SIZE (-Oz), not speed (-O3). Measured: -Oz cuts every module
# ~40% (bots 194->116KB, rules 49->27KB, guards 20->12KB embedded b64 drops
# ~105KB) for ~0 warm-perf cost — cordite's hot loop is bitboard popcount/ctz
# that both opt levels compile identically, and V8's TurboFan re-optimizes the
# hot functions the same way regardless. On the edge it's also FASTER cold: a
# smaller module means ~2x quicker TurboFan compile and far less base64 to
# decode per worker (short-lived invocations never tier up, so -O3's inlining
# only bloats Liftoff). Override for a warm-latency regime: `make WASM_OPT=-O3`.
WASM_OPT   ?= -Oz

# --- Post-link Binaryen (wasm-opt) size pass ------------------------------
# clang/wasm-ld compile each object in ISOLATION here — LTO is banned (it
# corrupts the indirect-function table, see WASM_FLAGS). wasm-opt runs on the
# already-LINKED binary, so it is the only stage that can clean up / inline
# ACROSS the object boundary. REQUIRES binaryen (`wasm-opt`) on PATH to build
# bots.wasm; CI does not rebuild the wasm (it ships the committed .gz), so this
# is a build-time dep only for whoever regenerates the asset. Correctness is
# gated by the SAME suites that caught the LTO table corruption — the embedded
# module still feeds e2e/wasm_kernel_fuzz + e2e/bot_parity (both green).
#
# Measured on the shipped bots.wasm (post strategy-cull, docs/
# WASM_OPT_INLINING_PROTOTYPE.md): -O2 --inlining-optimizing shrinks the SHIPPED
# gzip 49,446 -> 46,259 B (-6.4%) and is runtime-NEUTRAL on the bench:bot-e2e
# p50 (octogen/semtex/cordite/fulminate all within noise). So the win is module
# size / cold-start, not per-move speed. Plain -Oz is ~150 B smaller but is
# size-over-speed and unverified for warm latency, so it is NOT the default.
# rules/guards are already at clang -Oz's floor — wasm-opt is a no-op-to-worse
# there — so only bots enables it; WASM_POSTOPT stays empty for the others.
WASM_OPT_BIN  ?= wasm-opt
# General manual override for any module, e.g. `make wasm WASM_POSTOPT=-Oz`.
WASM_POSTOPT  ?=
# bots ships with the pass ON by default; a global WASM_POSTOPT override wins.
WASM_BOTS_POSTOPT ?= $(if $(strip $(WASM_POSTOPT)),$(WASM_POSTOPT),-O2 --inlining-optimizing)
# bulk-memory is on in every module (-mbulk-memory); the rest are stock LLVM
# output features wasm-opt must be told to accept before it will touch the binary.
WASM_OPT_FEATURES ?= --enable-bulk-memory --enable-mutable-globals --enable-sign-ext
# $(call wasm_postopt,<flags>,<file>) — rewrites <file> in place, or a no-op
# when <flags> is empty.
define wasm_postopt
$(if $(strip $(1)),$(WASM_OPT_BIN) $(WASM_OPT_FEATURES) $(1) $(2) -o $(2),:)
endef
# -mbulk-memory: memcpy/memset (incl. clang-lowered struct copies) become the
# wasm memory.copy/memory.fill instructions, which every 2020+ runtime (V8,
# JSC, SpiderMonkey, Deno edge) executes as native memmove — the hand-rolled
# word-loop shim showed up at ~5% of cordite's wasm profile.
# NO -flto: wasm-ld LTO corrupts the indirect-function table for the
# address-taken hook/strategy pointers — engine_snap_hook calls trap with
# "table index out of bounds"; caught by the fuzz suite. No -ffast-math
# either: wasm float semantics must stay IEEE-identical to the TS oracles
# the parity suite compares against.
# MAX_LEGAL_MOVES=4096 matches every native build (arena, difftests, eval).
# The rules module briefly shipped 65536 "for hardening", but action
# validation is direct rule-checking (handle_*), NOT enumeration membership —
# the buffer only feeds the exported move MENU, where the legal.c cap guards
# already stop combinatorial blowups safely at any value. 65536 cost 10.9MB
# of the module's 12MB for zero correctness benefit.
# M2 (docs/BOTS_WASM_MEMORY_PLAN.md) measured whether the bots build could drop
# to 1024 like rules (−181 KiB g_moves + −3 KiB forced_loss_flags): NEGATIVE.
# Instrumenting the enumerator (-DLEGAL_STATS, legal.c) over every shipped bot
# family × player counts 2–8 (52-card deck at 6–8p, production-accurate) shows
# the DEFENDER cover-combination menu saturates the buffer (>16,384) in ordinary
# large-hand 8-player states — e.g. a 20–23 card hand over 4 uncovered battles
# enumerates 5,616–16,384+ covers. That is real reachable bot play, not a
# corrupt state; the plan's decision rule (max > 2,900 → keep 4096) fires, and
# unlike rules.wasm (which serves the human UI, where a truncated cover menu is
# a benign UX bound) truncating a BOT's cover set at 1024 drops legal options it
# would autonomously pick from → a behavior change. 4096 stays; the rules/bots
# cap asymmetry is deliberate. (bots already truncates the widest 8p states at
# 4096; 1024 would only truncate MORE.)
# WORLD_LOG_CAP=40: sampled-world/trial/difftest Game slots keep a 40-entry
# discard-only log instead of the full 512 (see cordite_sim.h) — the research
# full-log world modes stay native-only. 3 x 61KB per bots.wasm instance.
# Stack: the bots build now ships 64 KiB (override in WASM_BOT_LDFLAGS below),
# down from 256 KiB, once three changes together collapsed the shadow-stack
# high-water (docs/BOTS_WASM_MEMORY_PLAN.md §6). A wasm stack canary
# (e2e/stack_canary.mts — paint 0xA5, run the shipped-bot corpus through the
# production bridge, scan) measured the pre-work worst case at 179 KiB, driven by
# (1) sim_solve_rec's 48-frame recursion carrying ~3.2 KiB/frame of fat locals and
# (2) the int[MAX_LEGAL_MOVES] index arrays in the heuristic choosers (handwritten
# 112 KiB, and the struct-rollout espresso frame semtex/fulminate reach). The three
# changes: M7a hoists the solver's move buffer + child SimState into depth-indexed
# BSS (cordite_sim.c — scalar frames, recursion depth-capped at 48); M2-stream
# rewrites handwritten's chooser as a single streaming pass (no index arrays); and
# the ship-set trim drops semtex/fulminate (the bots that reached the struct-rollout
# espresso frame). The shipped MC bots (octogen/cordite) use the bitboard rollout
# exclusively, so with the solver hoisted the measured high-water is 13.2 KiB —
# 64 KiB is 4.8x that (the recursion depth cap makes ~15 KiB a near-hard ceiling,
# so this margin is conservative). --stack-first keeps any regression a loud trap,
# and CI's mem job runs unpinned games. Net vs 256 KiB: −192 KiB stack at +102 KiB
# BSS (M7a) = −90 KiB. Native builds keep their large stack (this override is
# wasm-bots-only); the solver hoist is behavior-neutral (_Thread_local, native OMP
# safe; SIG-identical) so native play is unchanged.
# MAX_MOVE_CARDS=28: for ANY state reachable with <= 52 distinct cards no
# legal move exceeds 26 cards — an attack/pass of k needs the defender to
# hold >= k (2k <= 52), a cover of n needs n covers + n table attacks
# (2n <= 52) — so 28 emits the identical move set for every real game (the
# production deck is 36). Only corrupt/hostile marshaled states (duplicate
# cards) could enumerate wider moves, which the caps drop by design.
#
# --- L1 memory budget (docs/WASM_L1_BUDGET.md) --------------------------
# Every static buffer below is sized from tests/l1_measure.c (28K engine
# games, all player counts, random + handwritten strategies) with the
# margins stated inline. Overflow of any of them is a CLEAN error (replay
# REPLAY_ECAP, snap-ring drop, io bounds-checks), never corruption.
# REPLAY_REC_CAP=4096: peak recorded coder choices observed 674 (6.1x
#   margin); BN_CAP derives from it in replay.c (2,688 limbs) and stays
#   above the 2,049-limb floor a max-size REPLAY_MAX_INT_BYTES decode
#   integer needs. Cuts g_rec 768KB->48KB and g_bn 192KB->10.5KB in BOTH
#   replay-linking modules.
# WASM_REPLAY_IO_CAP=32768: observed worst encode input 3,117B / blob 200B
#   (>10x margin); decode input is separately capped at 8KB by
#   REPLAY_MAX_INT_BYTES. Cuts g_replay_io 2MB->32KB per module.
# MAX_SNAPS=24: the ring holds ONE marshal window's animation snapshots
#   (resets per marshal). Analytic worst is the round-transition window:
#   MAGIC + TRASH + defender-draw + <=num_players batch draws = num_players
#   + 3 = 11 at 8 players (refill snaps PER-PLAYER, not per card; no OUT/
#   DEFENDER_CHANGE snap in that path). The deal window is num_players + 3 =
#   11 too. Measured worst 12 over 63K games. 16 is 1.33x the measured worst
#   and 1.45x the analytic ceiling (11) — snap-ring overflow silently DROPS
#   animation frames (visual, not corruption), so a modest margin is safe.
#   Cuts g_snaps 55.7KB->27.8KB->18.6KB (M4, docs/BOTS_WASM_MEMORY_PLAN.md;
#   was 24). Shared by bots + rules; guards keeps its own MAX_SNAPS=1.
# -mno-nontrapping-fptoint pins the wasm FEATURE SET, the same way -mbulk-memory
# does. clang >= 22 emits i32.trunc_sat_* by default; a wasm-opt built without
# that feature enabled then rejects the module outright ("all used features
# should be allowed"), so the toolchain that produced the committed artifact
# built fine while a newer clang cannot build at all. Pinning the feature keeps
# the module's instruction set identical across clang versions instead of
# letting the local compiler decide what the shipped artifact requires.
WASM_FLAGS := --target=wasm32 $(WASM_OPT) -nostdlib -ffreestanding -mbulk-memory \
              -mno-nontrapping-fptoint \
              -isystem wasm/include -Isrc -D_Thread_local= -DMAX_LOG_PAIRS=64 \
              -DMAX_LEGAL_MOVES=4096 -DMAX_MOVE_CARDS=28 -DMAX_BATTLES=64 \
              -DWORLD_LOG_CAP=40 \
              -DREPLAY_REC_CAP=4096 -DWASM_REPLAY_IO_CAP=32768 -DMAX_SNAPS=16 \
              -DCD_TT_BITS=12 -DCD_TT_2WAY -DCD_TT_PACK8 \
              -Wall -Wextra -Wno-unused-parameter \
              -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=262144 -Wl,--stack-first \
              -Wl,--strip-all
# CD_TT_BITS=12 + CD_TT_2WAY: the cordite endgame solver's transposition table
# (bump-allocated on first play) is the largest driver of bots.wasm's RUNTIME
# peak — it grows, unlike the static caps above. 16 = 1 MiB; 13 = 128 KiB; 12 =
# 64 KiB (4,096 entries, exactly one wasm page → L1d-resident). It's a
# memoization cache, but the solver is node-budget-limited, so table size is a
# bot-STRENGTH knob, sized by the per-game move-divergence study
# (tools/tt_divergence.sh, docs/WASM_L1_BUDGET.md). There is no cliff-to-zero: the
# binding bot octogen (persists the table per game) has an INHERENT ~4e-4 floor of
# size-sensitivity that exists even at TT16-TT19; the collision knee is ~TT10-11.
# CD_TT_2WAY makes each aligned slot pair a 2-entry bucket (one cache line),
# roughly halving conflict misses ⟹ ~+1 effective bit, so TT12+2WAY (64 KiB)
# matches/beats direct-mapped TT13 (128 KiB): over 3,000 espresso games it gives
# 0 outcome flips and 0.10% move-divergence vs TT22, both below TT13's 0.14%/0.04%
# floor, and it FIXES TT13's known win->loss flips (seeds 720958, 700910). See
# docs/SOLVER_TT_WORKING_SET_PLAN.md (candidate C4) for the full validation
# ladder. Drops the solver's runtime peak another 64 KiB (1 page) below TT13,
# with strength >= the previously shipped table. Only bots links cordite_sim, so
# rules/guards ignore it.
# CD_TT_PACK8 (candidate C6): packs each entry from 16 to 8 bytes (40-bit key tag
# + 12-bit value + 6-bit depth + flags), halving the table again to 32 KiB at the
# same 4,096 slots. SIG-identical to the 16-byte TT12+2WAY table on the full tricky
# panel and at TT22 (the 40-bit tag + 12-bit index = 52-bit effective key aliases
# to a wrong value at only ~1e-5/game, an order of magnitude below octogen's ~4e-4
# inherent floor). Net solver table: 128 KiB (TT13) -> 64 KiB (TT12+2WAY) -> 32 KiB.

# Explicit export allow-list — only the memory (above) + wasm_* API the TS
# bridge actually calls (engine.ts / bots.ts). This REPLACES -Wl,--export-all,
# which exported every internal C symbol by name (41 in rules.wasm, 98 in
# bots.wasm — solver scratch, difftest reporters, *_oracle_/*_old_ strategy
# variants that production never calls) AND pinned each as a GC root, blocking
# wasm-ld's dead-code elimination of the unreferenced ones. Explicit exports
# shrink the name section and let the linker drop unreachable code.
# NOTE: a NEW wasm_* entry point must be added here or the TS side won't see it
# (the parity + e2e suites fail loudly on an omission).
WASM_API_EXPORTS := \
  -Wl,--export=wasm_io_ptr -Wl,--export=wasm_io_cap \
  -Wl,--export=wasm_game_ptr_internal -Wl,--export=wasm_moves_ptr_internal \
  -Wl,--export=wasm_cards_a_ptr -Wl,--export=wasm_cards_b_ptr \
  -Wl,--export=wasm_init -Wl,--export=wasm_set_seed \
  -Wl,--export=wasm_set_deal_seed_bytes -Wl,--export=wasm_reject_reason \
  -Wl,--export=wasm_import_state -Wl,--export=wasm_export_state \
  -Wl,--export=wasm_state_serialize -Wl,--export=wasm_state_deserialize \
  -Wl,--export=wasm_state_format_version \
  -Wl,--export=wasm_export_logs -Wl,--export=wasm_export_logs_masked \
  -Wl,--export=wasm_snap_count \
  -Wl,--export=wasm_snap_tag -Wl,--export=wasm_snap_aux -Wl,--export=wasm_export_snapshot \
  -Wl,--export=wasm_start_game -Wl,--export=wasm_attack -Wl,--export=wasm_cover \
  -Wl,--export=wasm_pass -Wl,--export=wasm_pickup -Wl,--export=wasm_good \
  -Wl,--export=wasm_transition -Wl,--export=wasm_refill \
  -Wl,--export=wasm_reset_to_lobby -Wl,--export=wasm_human_mask \
  -Wl,--export=wasm_import_strategy_keys \
  -Wl,--export=wasm_game_done -Wl,--export=wasm_should_act \
  -Wl,--export=wasm_next_player -Wl,--export=wasm_can_cover \
  -Wl,--export=wasm_replay_io_ptr -Wl,--export=wasm_replay_io_cap \
  -Wl,--export=wasm_replay_decode \
  -Wl,--export=wasm_replay_encode_v6 \
  -Wl,--export=wasm_replay_error_detail -Wl,--export=wasm_replay_error_ptr \
  -Wl,--export=wasm_legal_moves -Wl,--export=wasm_legal_moves_ptr \
  -Wl,--export=wasm_apply_action -Wl,--export=wasm_finalize_win \
  -Wl,--export=wasm_view_serialize -Wl,--export=wasm_events_serialize \
  -Wl,--export=wasm_rearrange_hand \
  -Wl,--export=wasm_set_deterministic_deck \
  -Wl,--export=wasm_seed_rng_deterministic \
  -Wl,--export=wasm_set_strategy_seed_deterministic \
  -Wl,--export=wasm_strategy_seed_probe \
  -Wl,--export=wasm_set_rng_base \
  -Wl,--export=wasm_layout_hash

# --- Layout hash (tools/structgen) ------------------------------------------
# Every module linking wasm_api.c exports wasm_layout_hash(): the structgen hash
# of the Game layout (tools/structgen/specs/game_layout.args) under THAT build's
# flags, passed in as -DSG_LAYOUT_HASH. sdk/ts/gen/layout_hash.<build>.ts carries
# the same number as LAYOUT_HASH (a module of its own, beside the accessors in
# game_layout.<build>.ts, so the browser checks it without importing a reader of
# the unmasked Game), and bots.ts / engine.ts refuse to run a module
# whose hash differs - a stale wasm or a stale generated module is a load-time
# error instead of a wrong read.
#
# build/layout/<build>.hash is regenerated on EVERY make that needs it (FORCE)
# but rewritten only when its content changes, and the bots/rules rules also
# refresh sdk/ts/gen/{game_layout,layout_hash}.<build>.ts the same way, so a header edit
# followed by `make wasm-bots` leaves module and wasm in agreement.
#
# The same-second trap: make compares whole-second mtimes, so a header edit
# landing in the second of the last build rebuilds nothing, and a header whose
# mtime went BACKWARDS (a checkout, a copy) never does. For a layout change that
# means objects compiled against two different Game structs linked into one
# module. So the wasm phony targets settle by CONTENT first: every object
# directory records the layout hash its objects were compiled for
# (<objdir>/.layout), and when that differs from the current hash (sg_settle)
# the directory and the module are deleted before a sub-make builds them from
# clean. Deleting is the only reliable lever: make 3.81 does not even relink a
# target whose prerequisite was recreated inside the same second. The file
# targets (build/bots.wasm, used directly by tools/wasm_anatomy) keep the plain
# mtime rules, with wasm_api.o depending on the hash file.
# TWO DIRECTORIES, because the generator is shared and its configuration is not.
# STRUCTGEN_DIR is the tool, built once for both products; SG_SPEC_DIR is this
# product's description of its own structs and stays beside the product.
STRUCTGEN_DIR  := ../shared/tools/structgen
SG_SPEC_DIR    := ../tools/structgen/specs
STRUCTGEN      := $(STRUCTGEN_DIR)/build/structgen
SG_GAME_SPEC   := $(SG_SPEC_DIR)/game_layout.args
SG_GAME_ARGS    = $(shell grep -v '^[[:space:]]*\#' $(SG_GAME_SPEC))

# EVERY source and header, not structgen.c alone. structgen used to be one
# translation unit and this line named it; once it split into sg_clang/sg_model/
# sg_ts/sg_swift/sg_hash/sg_args, naming one of them meant an edit to any of the
# others left this target "up to date" - and the binary it did not rebuild is the
# one that computes SG_LAYOUT_HASH. A stale structgen bakes a hash for a layout
# that is no longer the layout, which is the failure the hash exists to catch,
# arriving through the tool that catches it. Measured: after `touch
# tools/structgen/sg_ts.c`, make called this target up to date.
#
# tools/structgen/Makefile lists its sources explicitly and is correct on its
# own; this wildcard exists because THIS file reaches across into that directory
# and must not hold a narrower idea of what structgen is made of than it does.
$(STRUCTGEN): $(wildcard $(STRUCTGEN_DIR)/*.c) $(wildcard $(STRUCTGEN_DIR)/*.h) \
              $(wildcard $(STRUCTGEN_DIR)/../sgcommon/*.c) $(wildcard $(STRUCTGEN_DIR)/../sgcommon/*.h) \
              $(STRUCTGEN_DIR)/Makefile $(STRUCTGEN_DIR)/../llvm.mk
	$(MAKE) -C $(STRUCTGEN_DIR) build/structgen

# $(call sg_hash,<build name>,<cflags>,<ts stem, or empty>): with a stem, also
# writes ../sdk/ts/gen/game_layout.<stem>.ts and ../sdk/ts/gen/layout_hash.<stem>.ts.
sg_gen   = ../sdk/ts/gen
sg_swift = ../sdk/swift/gen
# mkdir first: nothing under sdk/*/gen is committed, so on a fresh checkout the
# directory does not exist until something generates into it.
sg_sync = && { mkdir -p $(dir $(2)); { cmp -s $(1) $(2) || { cp $(1) $(2); echo "layout: wrote $(2)"; }; }; } && rm -f $(1)
sg_hash = @mkdir -p build/layout && \
  $(STRUCTGEN) --cwd . $(SG_GAME_ARGS) --build '$(1)=$(strip $(2))' --print-hash \
    $(if $(3),--ts $@.ts --hash-ts $@.hash.ts) > $@.new && \
  { cmp -s $@.new $@ || { echo "layout $(1): $$(cat $@ 2>/dev/null || echo none) -> $$(cat $@.new)"; mv $@.new $@; }; } && \
  rm -f $@.new $(if $(3),$(call sg_sync,$@.ts,$(sg_gen)/game_layout.$(3).ts) $(call sg_sync,$@.hash.ts,$(sg_gen)/layout_hash.$(3).ts))

# $(call sg_settle,<build name>,<object dir>,<wasm file target>): run before the
# sub-make that builds <wasm file target>, after build/layout/<build>.hash.
sg_settle = @cmp -s $(2)/.layout build/layout/$(1).hash || { \
  if [ -d $(2) ]; then echo "layout $(1): $(2) was compiled for $$(cat $(2)/.layout 2>/dev/null || echo an unrecorded layout), not $$(cat build/layout/$(1).hash) - rebuilding $(3) from clean"; fi; \
  rm -rf $(2) $(3) && mkdir -p $(2) && cp build/layout/$(1).hash $(2)/.layout; }

# The iOS layout, which is its own: its caps are not the wasm caps and its
# target is not wasm32 (a pointer alone is 4 bytes there and 8 here). The hash
# is of the structs the Swift bindings read (tools/structgen/specs/
# ios_layout.args), and `ios-lib` bakes it into the library as SG_LAYOUT_HASH,
# so sdk/swift/gen and the .a it is read against cannot come apart silently.
#
# ALL THREE SLICES, compared: the device is arm64 and the simulator is arm64 or
# x86_64, and one hash is stamped into all three. They agree today - none of
# these structs has a member whose size or alignment differs between LP64
# targets - and if one ever stops agreeing, the library it is stamped into
# would read right on two architectures and wrong on the third, which is
# exactly the failure this file refuses to ship.
IOS_LAYOUT_TRIPLE  := arm64-apple-ios$(IOS_MIN_VERSION)
IOS_LAYOUT_TRIPLES := $(IOS_LAYOUT_TRIPLE) arm64-apple-ios$(IOS_MIN_VERSION)-simulator x86_64-apple-ios$(IOS_MIN_VERSION)-simulator
SG_IOS_SPEC := $(SG_SPEC_DIR)/ios_layout.args
SG_IOS_ARGS  = $(shell grep -v '^[[:space:]]*\#' $(SG_IOS_SPEC))
build/layout/ios.hash: $(STRUCTGEN) $(SG_IOS_SPEC) FORCE
	@mkdir -p build/layout && \
	for t in $(IOS_LAYOUT_TRIPLES); do \
	  $(STRUCTGEN) --cwd . $(SG_IOS_ARGS) --build 'ios=$(strip $(IOS_CFLAGS))' --target $$t --print-hash || exit 1; \
	done | sort -u > $@.new && \
	[ "$$(grep -c . $@.new)" = 1 ] || { echo "layout ios: the iOS slices do not share one layout:"; cat $@.new; exit 1; }
	@{ cmp -s $@.new $@ || { echo "layout ios: $$(cat $@ 2>/dev/null || echo none) -> $$(cat $@.new)"; mv $@.new $@; }; }; rm -f $@.new
# AND THE MODULE THE HASH IS FOR, beside it. `ios-lib` bakes this hash into the
# library; sdk/swift/gen/kernel.ios.swift carries it as SG_LAYOUT_HASH and
# FoolishKit compiles that file. Writing only the hash here meant the pair was
# made by two different commands - `make ios-lib` and `tools/structgen/gen.sh` -
# and running the first without the second gave a library and a module that
# disagree, which sdk/swift/KernelLayout.swift turns into a fatalError at the
# first EngineC(). The wasm builds have written their own generated module all
# along ($(call sg_hash,…,bots)); this is the same rule for iOS.
#
# Same cmp-then-copy as sg_sync: the file's mtime moves only when its CONTENT
# does, so a rebuild that changes no layout does not make Xcode recompile
# FoolishKit.
	@$(STRUCTGEN) --cwd . $(SG_IOS_ARGS) --build 'ios=$(strip $(IOS_CFLAGS))' --target $(IOS_LAYOUT_TRIPLE) \
	  --swift $@.swift $(call sg_sync,$@.swift,$(sg_swift)/kernel.ios.swift)

build/layout/bots.hash: $(STRUCTGEN) FORCE
	$(call sg_hash,bots,$(WASM_BOT_CFLAGS),bots)
build/layout/bots-explain.hash: $(STRUCTGEN) FORCE
	$(call sg_hash,bots_explain,$(WASM_BOT_CFLAGS) -DOG_EXPLAIN_BUILD,)
build/layout/msg.hash: $(STRUCTGEN) FORCE
	$(call sg_hash,msg,$(WASM_MSG_CFLAGS),)
build/layout/oracle.hash: $(STRUCTGEN) FORCE
	$(call sg_hash,oracle,$(WASM_ORACLE_CFLAGS),)
build/layout/oracle-mt.hash: $(STRUCTGEN) FORCE
	$(call sg_hash,oracle_mt,$(WASM_ORACLE_MT_CFLAGS),)

.PHONY: FORCE
FORCE:

# bots.wasm exports the rules API above PLUS the bot bridge (wasm_bots_api.c),
# and PLUS the FMSG pair (wasm_msg_decode/seal). FMSG is bots-only by steer, not
# by necessity: decode would run in rules.wasm, but every host is consolidating
# onto the ONE big module (see docs/IMESSAGE_BODY_CODEC.md), and seal cannot live
# anywhere else anyway — it reads a resident session log.
# Three entry points live in wasm_api.c but are exported ONLY here, for the same
# reason: they need a whole SESSION LOG resident, which only this module can
# hold. Two are the log-offset variants for the drive cycle, whose belief bots
# keep the session log under the records the cycle writes (see
# wasm_export_logs_masked_from). The third is wasm_replay_encode_v6_from_game:
# it reads a finished game's actions out of its imported logs. The rules module
# is built at MAX_LOGS=128 with no log import — raising it is not a knob, since
# the Game struct alone would go 33KB -> 133KB and blow the module's pinned
# 3-page memory — so its exports stay as they were and rules.wasm/guards.wasm
# do not move.
WASM_BOTS_API_EXPORTS := \
  -Wl,--export=wasm_setenv_from_io -Wl,--export=wasm_clearenv \
  -Wl,--export=wasm_import_logs -Wl,--export=wasm_clear_logs \
  -Wl,--export=wasm_set_game_key \
  -Wl,--export=wasm_set_strategy_seed -Wl,--export=wasm_reload_bot_flags \
  -Wl,--export=wasm_choose_move \
  -Wl,--export=wasm_bot_roster_dump \
  -Wl,--export=wasm_bot_drive -Wl,--export=wasm_bot_eligible_mask \
  -Wl,--export=wasm_bot_cycle_delay_ms -Wl,--export=wasm_bot_drive_log_start \
  -Wl,--export=wasm_export_logs_masked_from \
  -Wl,--export=wasm_events_serialize_from \
  -Wl,--export=wasm_replay_encode_v6_from_game \
  -Wl,--export=wasm_replay_events -Wl,--export=wasm_replay_events_index_ptr \
  -Wl,--export=wasm_replay_step_count \
  -Wl,--export=wasm_replay_step_index \
  -Wl,--export=wasm_replay_extras_encode -Wl,--export=wasm_replay_extras_ptr \
  -Wl,--export=wasm_replay_extras_decode \
  -Wl,--export=wasm_replay_link \
  -Wl,--export=wasm_replay_b32_encode -Wl,--export=wasm_replay_b32_decode \
  -Wl,--export=wasm_replay_link_parse \
  -Wl,--export=wasm_unambiguous_cover \
  -Wl,--export=wasm_msg_decode -Wl,--export=wasm_msg_seal -Wl,--export=wasm_msg_header_ptr \
  -Wl,--export=wasm_msg_rule_p -Wl,--export=wasm_msg_rebase \
  -Wl,--export=wasm_msg_pickup_hold \
  -Wl,--export=wasm_anim_should_drop_stale -Wl,--export=wasm_anim_stale_optimistic \
  -Wl,--export=wasm_anim_event_key \
  -Wl,--export=wasm_anim_finish_rows -Wl,--export=wasm_anim_hand_laid_out \
  -Wl,--export=wasm_anim_conflict_verdicts \
  -Wl,--export=wasm_anim_set_transport -Wl,--export=wasm_anim_transport \
  -Wl,--export=wasm_anim_plan_ptr -Wl,--export=wasm_anim_frame_ptr -Wl,--export=wasm_anim_beats_ptr \
  -Wl,--export=wasm_anim_build_plan -Wl,--export=wasm_anim_plan_at -Wl,--export=wasm_anim_build_beats \
  -Wl,--export=wasm_anim_reversal_order \
  -Wl,--export=wasm_belief_probe_reset -Wl,--export=wasm_belief_probe_dump -Wl,--export=wasm_belief_probe_ptr

# The C Roster (src/roster.h), in the TEST build only (WASM_TEST_EXPORTS below):
# e2e/helpers/roster_kernel.ts drives the durable encoding and the envelope
# trailer through these directly (e2e/packed_roster_wire.test.ts feeds the Swift
# decoder C-written trailers). Production reaches the roster through the table
# exports, never through these.
WASM_ROSTER_EXPORTS := \
  -Wl,--export=wasm_roster_encode -Wl,--export=wasm_roster_spec_ptr -Wl,--export=wasm_roster_trailer_ptr -Wl,--export=wasm_roster_decode \
  -Wl,--export=wasm_roster_trailer_write -Wl,--export=wasm_roster_trailer_read

# The C Table (src/table.h, wasm/wasm_table_api.c), bots.wasm ONLY, for the same
# reason as the roster: the oracle modules link WASM_BOTS_API_EXPORTS and have no
# use for a table, and an unexported table.o is dropped whole by the linker. No
# production caller yet (docs/C_GAME_SHAPE_MIGRATION.md Phase 3); the server
# switches to it in Phase 4b, through sdk/ts/table/server_table.ts.
WASM_TABLE_EXPORTS := \
  -Wl,--export=wasm_table_roster_ptr -Wl,--export=wasm_table_commit_ptr \
  -Wl,--export=wasm_table_request_ptr -Wl,--export=wasm_table_elo_ptr \
  -Wl,--export=wasm_table_detail -Wl,--export=wasm_table_reject \
  -Wl,--export=wasm_table_request_decode -Wl,--export=wasm_table_load \
  -Wl,--export=wasm_table_act -Wl,--export=wasm_table_needs_bots \
  -Wl,--export=wasm_table_bots_need_logs -Wl,--export=wasm_table_commit_products \
  -Wl,--export=wasm_table_push -Wl,--export=wasm_table_envelope \
  -Wl,--export=wasm_table_action_response -Wl,--export=wasm_table_rankings \
  -Wl,--export=wasm_elo_deltas \
  -Wl,--export=wasm_table_create -Wl,--export=wasm_table_join \
  -Wl,--export=wasm_table_leave -Wl,--export=wasm_table_add_bot \
  -Wl,--export=wasm_table_remove_bot -Wl,--export=wasm_table_ready \
  -Wl,--export=wasm_table_reseat -Wl,--export=wasm_table_retitle \
  -Wl,--export=wasm_table_continue -Wl,--export=wasm_table_rearrange_hand \
  -Wl,--export=wasm_table_redact -Wl,--export=wasm_table_seat_of \
  -Wl,--export=wasm_table_set_deal_seed -Wl,--export=wasm_table_set_session_log \
  -Wl,--export=wasm_table_bot_drive -Wl,--export=wasm_table_drive_ptr \
  -Wl,--export=wasm_table_drive_prefs -Wl,--export=wasm_table_cycle_delay_ms \
  -Wl,--export=wasm_table_replay_code -Wl,--export=wasm_table_replay_extras

# The web client's slot (src/client_table.h, wasm/wasm_table_api.c), bots.wasm
# ONLY: the browser reads every envelope and push through these, and through the
# generated snapshot readers in sdk/ts/gen/view_layout.bots.ts.
WASM_CLIENT_EXPORTS := \
  -Wl,--export=wasm_client_view_ptr -Wl,--export=wasm_client_event_ptr \
  -Wl,--export=wasm_client_detail -Wl,--export=wasm_client_adopt_envelope \
  -Wl,--export=wasm_client_adopt_resident -Wl,--export=wasm_client_identity_at \
  -Wl,--export=wasm_client_push_open -Wl,--export=wasm_client_push_next \
  -Wl,--export=wasm_client_push_final -Wl,--export=wasm_client_identity \
  -Wl,--export=wasm_client_identity_begin -Wl,--export=wasm_client_identity_seat \
  -Wl,--export=wasm_client_rules_view_ptr -Wl,--export=wasm_client_rules_ptr \
  -Wl,--export=wasm_client_view_rules \
  -Wl,--export=wasm_client_edit_view_ptr -Wl,--export=wasm_client_final_view_ptr \
  -Wl,--export=wasm_client_board_edit_ptr -Wl,--export=wasm_client_conflict_ptr \
  -Wl,--export=wasm_client_verdicts_ptr -Wl,--export=wasm_client_validate \
  -Wl,--export=wasm_client_optimistic_apply -Wl,--export=wasm_client_board_edit \
  -Wl,--export=wasm_client_rearrange_hand -Wl,--export=wasm_client_conflict_verdicts \
  -Wl,--export=wasm_client_gesture_ptr -Wl,--export=wasm_client_play_ptr \
  -Wl,--export=wasm_client_play

# A replay's recorded decision as the Oracle imports it, and a code at a glance
# (src/replay_steps.h, wasm/wasm_bots_api.c), bots.wasm ONLY: the web page builds
# the Oracle's job and hands the bytes to oracle.wasm, which only imports them, so
# the oracle modules do not link these and their unexported code is dropped there.
WASM_REPLAY_WEB_EXPORTS := \
  -Wl,--export=wasm_replay_step_masked_state -Wl,--export=wasm_replay_step_logs \
  -Wl,--export=wasm_replay_summary

# C-backed test fixtures (wasm/wasm_table_api.c, e2e/helpers/table_fixture.ts),
# in the TEST build only, like WASM_ROSTER_EXPORTS: a test composes a board
# through the generated setters and seals it with the roster into a row the
# kernel has loaded; card text goes through card.h's parser.
WASM_FIXTURE_EXPORTS := \
  -Wl,--export=wasm_fixture_begin -Wl,--export=wasm_fixture_title \
  -Wl,--export=wasm_fixture_seat -Wl,--export=wasm_fixture_seal \
  -Wl,--export=wasm_card_list_parse

# The exports only tests call. They are NOT in the shipped bots.wasm.gz: an
# export roots its code, so each one kept roster.c's durable codec, the fixture
# sealer and card.h's parser in every module the browser and the edge load.
# build/bots_test.wasm is the shipped module's objects and link plus these, built
# on demand (`make -C c wasm-bots-test`, which e2e/helpers/bots_test_wasm.ts runs
# when the build is missing or older than the wasm sources) and never committed.
# Same objects, so the same layout hash: the helpers check it like any host.
# A code's whole log stream, record by record (replay.h ReplayDecodedLog,
# e2e/helpers/replay_decode.ts): tests and tools read it, no shipped host does.
WASM_REPLAY_DECODE_EXPORTS := \
  -Wl,--export=wasm_replay_decoded_open -Wl,--export=wasm_replay_decoded_next

# The rules and bot bridge entries no shipped host of bots.wasm calls any more.
# They served the TS game shape: engine.ts marshalled a TS Game into the resident
# slot and drove it move by move (attack .. refill, the state and log codecs, the
# snapshot ring, the replay codec), and bots.ts chose moves for it (the env
# table, choose_move, the resident drive and its seeding). Phase 8 deleted both;
# the server plays the C Table and the web its client slot. Checked before the
# move, by name, across src/, server/, sdk/ts/ and scripts/: no caller. The oracle
# modules keep them all (oracleBridge.ts imports states, logs and brains and calls
# choose_move on oracle.wasm), and so does the test build, where the fixture and
# probe suites (kernel_state_validation, security_hidden_info) still knock on the
# state doors. Filtered out of bots.wasm's link only, so WASM_API_EXPORTS and
# WASM_BOTS_API_EXPORTS stay the oracle modules' lists.
#
# wasm_unambiguous_cover joined them on 2026-09-18. legal.c's unambiguous_cover
# was the web's one-tap cover resolver and the web's alone (its own header said
# so); the web's gesture rules moved to client_play, which asks the menu's
# question instead, and nothing shipped knocks on this door any more. Leaving it
# exported cost 1,191 raw / 533 gzip bytes of a module whose gzip is capped at
# 80 KiB (e2e/mem/wasm_memory.test.ts) - the rule and its fuzz stay, on the test
# build, which is what this list is for.
#
# wasm_replay_error_detail joined them on 2026-09-19, found by the gate that
# now asks the question continuously (e2e/validation/wasm_exports_validation.test.ts).
# It is the PACKED-INT predecessor of wasm_replay_error_ptr: one int carrying
# log_type << 16 | menu, which every host stopped unpacking when replay.h grew
# a ReplayError struct and structgen started generating its reader
# (sdk/ts/wasm/bots.ts replayErrorDetail). No TypeScript has named it since.
# The C entry stays for the oracle modules and the test build, and
# replay_last_error_detail() itself is what the C suites print on a refusal.
# It is a one-line accessor, so the shipped module moved by 39 raw / 14 gzip
# bytes: the point here is not the bytes but that the export list and the
# TypeScript that binds it agree, which is the only state a gate can tell apart
# from drift.
WASM_BOTS_UNSHIPPED_API := \
  -Wl,--export=wasm_unambiguous_cover \
  -Wl,--export=wasm_replay_error_detail \
  -Wl,--export=wasm_game_ptr_internal -Wl,--export=wasm_moves_ptr_internal \
  -Wl,--export=wasm_set_seed -Wl,--export=wasm_set_deal_seed_bytes -Wl,--export=wasm_reject_reason \
  -Wl,--export=wasm_import_state -Wl,--export=wasm_export_state \
  -Wl,--export=wasm_state_serialize -Wl,--export=wasm_state_deserialize \
  -Wl,--export=wasm_state_format_version \
  -Wl,--export=wasm_export_logs -Wl,--export=wasm_export_logs_masked \
  -Wl,--export=wasm_snap_count -Wl,--export=wasm_snap_tag -Wl,--export=wasm_snap_aux \
  -Wl,--export=wasm_export_snapshot \
  -Wl,--export=wasm_start_game -Wl,--export=wasm_attack -Wl,--export=wasm_cover \
  -Wl,--export=wasm_pass -Wl,--export=wasm_pickup -Wl,--export=wasm_good \
  -Wl,--export=wasm_transition -Wl,--export=wasm_refill \
  -Wl,--export=wasm_reset_to_lobby -Wl,--export=wasm_human_mask \
  -Wl,--export=wasm_import_strategy_keys \
  -Wl,--export=wasm_game_done -Wl,--export=wasm_should_act -Wl,--export=wasm_next_player \
  -Wl,--export=wasm_replay_decode -Wl,--export=wasm_replay_encode_v6 \
  -Wl,--export=wasm_apply_action -Wl,--export=wasm_finalize_win \
  -Wl,--export=wasm_view_serialize -Wl,--export=wasm_events_serialize \
  -Wl,--export=wasm_rearrange_hand -Wl,--export=wasm_set_deterministic_deck \
  -Wl,--export=wasm_seed_rng_deterministic -Wl,--export=wasm_set_strategy_seed_deterministic \
  -Wl,--export=wasm_strategy_seed_probe -Wl,--export=wasm_set_rng_base \
  -Wl,--export=wasm_setenv_from_io -Wl,--export=wasm_clearenv \
  -Wl,--export=wasm_import_logs -Wl,--export=wasm_clear_logs \
  -Wl,--export=wasm_set_game_key -Wl,--export=wasm_set_strategy_seed -Wl,--export=wasm_reload_bot_flags \
  -Wl,--export=wasm_choose_move \
  -Wl,--export=wasm_bot_drive -Wl,--export=wasm_bot_eligible_mask \
  -Wl,--export=wasm_bot_cycle_delay_ms -Wl,--export=wasm_bot_drive_log_start \
  -Wl,--export=wasm_export_logs_masked_from -Wl,--export=wasm_events_serialize_from

WASM_TEST_EXPORTS := $(WASM_ROSTER_EXPORTS) $(WASM_FIXTURE_EXPORTS) $(WASM_REPLAY_DECODE_EXPORTS) $(WASM_BOTS_UNSHIPPED_API)

# The small-memory flag set the iMessage module (wasm-msg below) is built from.
# It was rules.wasm's, which the web and the edge functions no longer load: every
# host reads the ONE bots.wasm (docs/C_GAME_SHAPE_MIGRATION.md Phase 8). Its caps
# and its pin are rules.wasm's measured ones (docs/RULES_GUARDS_WASM_MEMORY_PLAN.md
# R0, R1, R4): a 1024-move menu, MAX_LOGS=128 (one action's logs), a 32 KiB stack,
# CD_RULES_OVERLAY (the replay scratch aliased over the action buffers, see
# src/rules_overlay.h), and a 3-page memory pin that wasm-msg lifts to its own.
WASM_RULES_FLAGS := $(filter-out -DMAX_LEGAL_MOVES=4096,$(WASM_FLAGS)) \
                    -DMAX_LEGAL_MOVES=1024 -DMAX_LOGS=128 -DWASM_IO_CAP=24576 \
                    -DCD_RULES_OVERLAY \
                    -Wl,-z,stack-size=32768 \
                    -Wl,--initial-memory=196608 -Wl,--max-memory=196608

# ---------- msg module: the iMessage extension's kernel, and nothing else ----
#
# What a message thread actually needs: the rules, the chain envelope, the
# animation core, and the replay codec that turns a finished game into a share
# link. NOT a single bot - an iMessage game is two people, and the extension
# (ios/FoolishMessages) never drives a seat. NOT the bot roster, the belief
# probe, the knob table or the strategy dispatch that go with them.
#
# It is also the only host that uses the PRE/POST-SETTLEMENT SPLIT: it plays the
# action, holds the settlement back (evwire_frames_settlement_cut) and releases
# it on Send, so a bout end does not spoil itself in the composer. That lives in
# evwire.c, which is here.
#
# Cut as a measurement of the floor: how small is the kernel when the host is a
# chat extension rather than a server. Compare `make wasm-msg-size` against
# bots.wasm, which is the same kernel plus every rung of the ladder.
# NOT replay_steps.c: that is the step WALKER, which replays a finished code
# through the engine to feed a replay screen. The extension has no replay
# screen.
#
# REPLAY IS ENCODE-ONLY HERE. replay.c and replay_extras.c stay, because the
# extension very much does want the share URL at the end of a game - but only
# the writing half of them ships. The exports below name the encoders and
# nothing else, so wasm-ld drops replay_decode (the step-list decode behind a
# replay screen), replay_b32_decode, replay_extras_decode and
# replay_link_parse: reading a code someone else made is the WEB's job, and the
# extension only ever writes its own. wasm/msg_exports_check.mjs fails the build
# if a decoder comes back or an encoder goes missing.
#
# The one decoder that stays is not a replay: replay_decode_atoms_v6 is the FMSG
# BODY codec (msg_wire.c calls it - a chain's body IS a v6 action run), so
# opening a bubble needs it and it is not optional at any size.
WASM_MSG_SRC := src/game.c $(SHARED)/deal_rng.c src/legal.c src/view.c src/awire.c \
                src/evwire.c $(SHARED)/sha256.c src/msg_wire.c src/anim_plan.c \
                src/replay.c src/replay_extras.c \
                wasm/wasm_api.c wasm/wasm_msg_api.c

WASM_MSG_EXPORTS := \
  -Wl,--export=wasm_replay_extras_encode -Wl,--export=wasm_replay_extras_ptr \
  -Wl,--export=wasm_replay_b32_encode \
  -Wl,--export=wasm_replay_link \
  -Wl,--export=wasm_unambiguous_cover \
  -Wl,--export=wasm_msg_decode -Wl,--export=wasm_msg_seal -Wl,--export=wasm_msg_header_ptr \
  -Wl,--export=wasm_msg_rule_p -Wl,--export=wasm_msg_rebase \
  -Wl,--export=wasm_msg_pickup_hold \
  -Wl,--export=wasm_anim_should_drop_stale -Wl,--export=wasm_anim_stale_optimistic \
  -Wl,--export=wasm_anim_event_key \
  -Wl,--export=wasm_anim_finish_rows -Wl,--export=wasm_anim_hand_laid_out \
  -Wl,--export=wasm_anim_conflict_verdicts \
  -Wl,--export=wasm_anim_set_transport -Wl,--export=wasm_anim_transport \
  -Wl,--export=wasm_anim_plan_ptr -Wl,--export=wasm_anim_frame_ptr -Wl,--export=wasm_anim_beats_ptr \
  -Wl,--export=wasm_anim_build_plan -Wl,--export=wasm_anim_plan_at -Wl,--export=wasm_anim_build_beats \
  -Wl,--export=wasm_anim_reversal_order

# The rules API, minus the one export that would drag a replay DECODER in.
# (wasm_replay_decode is declared in wasm_api.c, which this target shares with
# rules.wasm, so dropping it has to happen on the link line rather than in
# WASM_MSG_EXPORTS below.)
WASM_MSG_API_EXPORTS := $(filter-out -Wl$(comma)--export=wasm_replay_decode,$(WASM_API_EXPORTS))

# The rules flags, minus its memory pin. rules.wasm freezes linear memory at
# 196,608 B (3 pages); this set needs 5, which is what wasm-ld picks for itself
# when the pin is taken off. The pin stays a pin - initial == max, so the module
# can never grow - just at the size this source set actually needs, rounded to
# the 64 KiB wasm page. Dropping the decoders did not buy a page back: they are
# code, and the 5 pages are bss.
WASM_MSG_MEM := 327680
WASM_MSG_FLAGS := $(filter-out -Wl$(comma)--initial-memory=196608 -Wl$(comma)--max-memory=196608,$(WASM_RULES_FLAGS)) \
                  -Wl,--initial-memory=$(WASM_MSG_MEM) -Wl,--max-memory=$(WASM_MSG_MEM)
WASM_MSG_CFLAGS  := $(filter-out -Wl%,$(WASM_MSG_FLAGS))
WASM_MSG_LDFLAGS := --target=wasm32 -nostdlib $(filter -Wl%,$(WASM_MSG_FLAGS))
WASM_MSG_POSTOPT ?= $(if $(strip $(WASM_POSTOPT)),$(WASM_POSTOPT),-Oz)
WASM_MSG_OBJ := $(addprefix build/msgobj/,$(notdir $(WASM_MSG_SRC:.c=.o)))

build/msgobj/%.o: src/%.c $(wildcard src/*.h) $(wildcard $(SHARED)/*.h) $(wildcard wasm/*.h)
	@mkdir -p build/msgobj
	$(WASM_CC) $(WASM_MSG_CFLAGS) -c $< -o $@
build/msgobj/%.o: wasm/%.c $(wildcard src/*.h) $(wildcard $(SHARED)/*.h) $(wildcard wasm/*.h)
	@mkdir -p build/msgobj
	$(WASM_CC) $(WASM_MSG_CFLAGS) -c $< -o $@
build/msgobj/wasm_api.o: wasm/wasm_api.c build/layout/msg.hash $(wildcard src/*.h) $(wildcard $(SHARED)/*.h) $(wildcard wasm/*.h)
	@mkdir -p build/msgobj
	$(WASM_CC) $(WASM_MSG_CFLAGS) -DSG_LAYOUT_HASH=$$(cat build/layout/msg.hash) -c $< -o $@

build/msgobj/%.o: $(SHARED)/%.c $(wildcard src/*.h) $(wildcard $(SHARED)/*.h) $(wildcard wasm/*.h)
	@mkdir -p build/msgobj
	$(WASM_CC) $(WASM_MSG_CFLAGS) -c $< -o $@

build/msg.wasm: $(WASM_MSG_OBJ) wasm/msg_exports_check.mjs Makefile
	@mkdir -p build
	$(WASM_CC) $(WASM_MSG_LDFLAGS) $(WASM_MSG_API_EXPORTS) $(WASM_MSG_EXPORTS) $(WASM_MSG_OBJ) -o $@
	$(call wasm_postopt,$(WASM_MSG_POSTOPT),$@)
	node wasm/msg_exports_check.mjs $@

.PHONY: wasm-msg wasm-msg-size
wasm-msg: build/layout/msg.hash
	$(call sg_settle,msg,build/msgobj,build/msg.wasm)
	@$(MAKE) --no-print-directory build/msg.wasm

# The number this target exists to produce.
wasm-msg-size: build/msg.wasm
	@printf 'msg.wasm    raw %7d  gzip %7d\n' \
	  $$(wc -c < build/msg.wasm) $$(gzip -9 -c build/msg.wasm | wc -c)
	@[ -f build/bots.wasm ] && printf 'bots.wasm   raw %7d  gzip %7d\n' \
	  $$(wc -c < build/bots.wasm) $$(gzip -9 -c build/bots.wasm | wc -c) || true

# Bot module: the rules kernel PLUS every algorithmic bot strategy and the
# choose-move bridge. A superset of rules.wasm, loaded only where bots run
# (edge functions' bot turns, offline harnesses) so the pure rules path keeps
# the smaller module.
# Note: the distilled strategy (research, strength-negative — see
# CORDITE_RESEARCH.md) stays native-arena-only; it is deliberately NOT in the
# production bot module.
# The SHIPPED difficulty-ladder bots (Durak Bot Ordnance Chart), all now ported
# to wasm — random, simple_heuristic, handwritten_prod, firecracker, blackpowder,
# cordite, octogen (the full seven rungs) — plus the two heuristics the MC bots
# call as rollout policies: espresso and handwritten (arena builds,
# cordite_strategy.c:591-594; also firecracker/blackpowder's rollouts).
# robusta_strategy.c rides along because firecracker IS robusta's MC with an
# espresso rollout (firecracker_strategy.c calls robusta_mc_choose). Dropped
# entirely (no ladder slot, no dep of a shipped bot): champion, ultimate_champion,
# hacker, fulminate, semtex, espresso_prod — their code + static data leave the
# module. NOTE: robusta/blackpowder MC rolls a full Game + LegalMoves per sample;
# those were stack locals (fine on native's 8 MB stack) and are hoisted to
# lazily-malloc'd per-thread scratch so they fit the 22 KiB bots.wasm shadow
# stack (see the stack-hoist notes in robusta_strategy.c / blackpowder_strategy.c).
#
# bot_knobs.c is here because cordite/octogen read their CD_*/OG_* knobs through
# it. It is a no-op for this module as long as the TS adapter keeps setting the
# env table: with no roster spec installed, bot_knob() is just getenv() (see
# bot_knobs.h), so bots.wasm behaves byte-for-byte as before.
#
# bot_roster.c IS here — A2 brought it in, because bot_drive.c dispatches
# through the roster and the module does not link without it. (This comment
# used to say the opposite, "deliberately NOT here yet"; it predates A2. The
# feared cost did not materialize: linking the roster does NOT drag
# espresso_prod_strategy.c / gunpowder_strategy.c in, so the module still
# carries only the rungs the server seeds.)
#
# What is left of A1 is the SWITCH, not the link: wasm_choose_move still
# dispatches its own way, and the TS env blocks in bot_strategy.ts still set
# the knobs the roster already specifies. Folding the two is one change,
# because it rewrites bots.wasm.gz.
#
# There is NO toolchain skew in the RAW module, and it is measured rather than
# asserted: with clang 22.1.8 + binaryen 130, build/bots.wasm comes out
# byte-identical on macOS arm64 (Homebrew), Linux arm64 and Linux x86_64
# (apt.llvm.org) - all md5 ac53b4dc5484aabad381d2d7bc451088, 191,485 B. A raw
# byte delta therefore means YOUR CHANGE, not the compiler, and
# `scripts/wasm_build.sh --check` is the gate that keeps that true.
#
# THE .gz IS A DIFFERENT STORY, and it is the compressor rather than the
# compiler: gzip -9 -n over that one identical module gives 81,892 B from Apple
# gzip, 82,043 B from GNU gzip and 82,468 B from node 20's zlib. So compare raw
# modules, never .gz sizes, and see e2e/mem/wasm_memory.test.ts for why the size
# gate moved onto the raw one.
#
# The trap that keeps manufacturing a phantom skew: WASM_CC defaults to plain
# `clang`, which on macOS is APPLE clang and cannot target wasm32 at all
# ("No available targets are compatible with triple wasm32"). It is WASM_CC=,
# not CC=.
#
# Nothing here is committed any more - CI builds the shipped modules in the lane
# that ships them (scripts/wasm_build.sh), so there is no committed artifact to
# reproduce as a baseline.
WASM_BOT_SRC := src/game.c $(SHARED)/deal_rng.c src/legal.c src/replay.c src/replay_steps.c \
                src/replay_extras.c src/view.c src/awire.c \
                src/evwire.c $(SHARED)/sha256.c src/msg_wire.c src/anim_plan.c \
                src/random_strategy.c \
                src/espresso_strategy.c src/handwritten_strategy.c \
                src/robusta_strategy.c src/firecracker_strategy.c \
                src/blackpowder_strategy.c \
                src/cordite_strategy.c src/cordite_sim.c \
                src/simple_heuristic_strategy.c \
                src/handwritten_prod_strategy.c \
                src/octogen_strategy.c \
                src/bot_knobs.c src/bot_roster.c src/bot_drive.c src/roster.c src/table.c src/client_table.c src/card.c \
                wasm/wasm_api.c wasm/wasm_bots_api.c wasm/wasm_msg_api.c wasm/wasm_table_api.c

# PER-FILE opt (a mix one clang invocation can't express): the profiled hot MC
# core is built at -O3, everything else at -Oz. -O3 on those five files cuts the
# heavy belief bots' (octogen/semtex) end-to-end p50 ~15% — the thinking-bot
# latency that the 2s CPU cap actually gates — while keeping the rest of the
# module -Oz. That's cold code the edge runtime's short-lived workers never tier
# up past Liftoff, so -O3 there only bloats the module (and its cold-start
# compile) for no speed. Measured: this MC-core mix MATCHES whole-module -O3 p50
# at ~66KB gz vs ~73KB (and beats all-Oz's ~52KB by ~15% p50). The heavy
# decisions self-tier-up mid-search, so the win holds cold as well as warm.
# Split the shared flags into compile-only vs link-only so per-file opt applies.
# CD_WASM_OVERLAY (M8, docs/BOTS_WASM_MEMORY_PLAN.md) is added HERE, not in the
# shared WASM_FLAGS: it aliases the replay scratch into cordite_sim.c's solve_ws,
# which only the bots module links — rules.wasm has no solver, and native builds
# run OMP games in parallel where the alias would race. bots-module compile only.
# -DCD_LEAFBOOK: octogen's shipped endgame oracle (c/LEAFBOOK.md). octogen
# enables it per-decision (cd_sim_set_leafbook); it terminates round-boundary
# ≤K-card subtrees with a proven-exact value for ~15% faster e2e p50. The book
# data (~27 KiB read-only) is dwarfed by the runtime TT peak, so the wasm memory
# footprint is unchanged; only the module SIZE grows by the book bytes.
# -DFOOLISH_SEEDED_BOTS_ONLY: bot_roster.c dispatches only the brains this
# module links (the seeded ladder). Without it the roster's dispatch would drag
# espresso_prod + gunpowder in for two offline-only rungs the server can never
# seed — measured at +5.1 KB gzip (54.1 -> 59.2 KB) on every edge cold start.
# The roster TABLE is unaffected: same keys, knobs, flags, tiers everywhere.
# WASM_IO_CAP=409600 (400 KiB). Two jobs, and the larger one sets it.
#
# 1. The LOG EXPORT, which wasm_api.c's export_logs writes UNCHECKED:
#    2 + MAX_LOGS x (4 + MAX_LOG_PAIRS x 2) = 135,170 B at 1024/64. That coupling
#    is why MAX_LOGS could not exceed 558 at the old 72 KiB — 512 was never a
#    choice — and it is a _Static_assert in wasm_api.c now rather than a comment,
#    after a MAX_LOGS bump sailed straight past it.
#
# 2. The LOG IMPORT, which is bigger: the session log arrives UNTRIMMED, and
#    ~40% of its records are dead goods (measured: 52% at 8 players). Sizing for
#    ~3072 raw records means the kernel can accept the whole stream and filter
#    it ITSELF. The alternative was TS pre-filtering to fit the buffer — i.e. a
#    rules decision ("which goods are live?") taken in TypeScript, which is the
#    one thing this repo does not do.
#
# It costs 400 KiB: g_io no longer fits the solve_ws slot M9 aliased it into
# (wasm_overlay.h), so it becomes its own static. Bots' linear memory ~1.4 -> 1.8
# MB against a 150 MB edge budget. Paid deliberately.
WASM_BOT_CFLAGS  := $(filter-out -Wl% $(WASM_OPT),$(WASM_FLAGS)) -DCD_WASM_OVERLAY -DCD_LEAFBOOK \
                    -DWASM_IO_CAP=409600 \
                    -DFOOLISH_SEEDED_BOTS_ONLY
# bots-only 22 KiB shadow stack. Adversarial canary (e2e/rules_stack_canary.mts
# for the cover path + e2e/stack_canary.mts for the solver path) measures the
# PRODUCTION worst at 14.3 KiB — the cover enumeration (calc_cover_moves'
# CoverOption opts[64] ~9.5 KiB + ~576 B per uncovered battle), which dominates
# the solver's ~12 KiB. Real games cap the defender at <=6 uncovered battles, so
# 22 KiB is 1.54x the production worst. Only a CORRUPT/hostile marshaled state
# with >~22 uncovered battles could exceed it (the cover frame is linear in
# battle count) — and that traps cleanly under --stack-first, never corrupts,
# exactly as the rules 32 KiB choice accepts. bots is server-side and only ever
# marshals engine-produced states (<=6 battles), so this path is unreachable in
# production. Net: -42 KiB stack takes bots' INITIAL linear memory 14 -> 13 pages
# (893,664 -> 850,656 B; the module still grows a TT on top at runtime — see the
# peak table in docs/WASM_L1_BUDGET.md). Trailing -z stack-size wins in wasm-ld.
WASM_BOT_LDFLAGS := $(filter -Wl%,$(WASM_FLAGS)) -Wl,-z,stack-size=22528
WASM_BOT_O3_SRC  := src/cordite_sim.c src/cordite_strategy.c \
                    src/octogen_strategy.c

WASM_BOT_OBJ    := $(addprefix build/botobj/,$(notdir $(WASM_BOT_SRC:.c=.o)))
WASM_BOT_O3_OBJ := $(addprefix build/botobj/,$(notdir $(WASM_BOT_O3_SRC:.c=.o)))

# Default bot-object opt is -Oz; the MC-core objects override to -O3.
build/botobj/%.o: WASM_BOT_FILE_OPT := -Oz
$(WASM_BOT_O3_OBJ): WASM_BOT_FILE_OPT := -O3

build/botobj/%.o: src/%.c $(wildcard src/*.h) $(wildcard $(SHARED)/*.h) $(wildcard wasm/*.h)
	@mkdir -p build/botobj
	$(WASM_CC) $(WASM_BOT_CFLAGS) $(WASM_BOT_FILE_OPT) -c $< -o $@
build/botobj/%.o: wasm/%.c $(wildcard src/*.h) $(wildcard $(SHARED)/*.h) $(wildcard wasm/*.h)
	@mkdir -p build/botobj
	$(WASM_CC) $(WASM_BOT_CFLAGS) $(WASM_BOT_FILE_OPT) -c $< -o $@
build/botobj/wasm_api.o: wasm/wasm_api.c build/layout/bots.hash $(wildcard src/*.h) $(wildcard $(SHARED)/*.h) $(wildcard wasm/*.h)
	@mkdir -p build/botobj
	$(WASM_CC) $(WASM_BOT_CFLAGS) $(WASM_BOT_FILE_OPT) -DSG_LAYOUT_HASH=$$(cat build/layout/bots.hash) -c $< -o $@

build/botobj/%.o: $(SHARED)/%.c $(wildcard src/*.h) $(wildcard $(SHARED)/*.h) $(wildcard wasm/*.h)
	@mkdir -p build/botobj
	$(WASM_CC) $(WASM_BOT_CFLAGS) $(WASM_BOT_FILE_OPT) -c $< -o $@

WASM_BOTS_SHIPPED_EXPORTS = $(filter-out $(WASM_BOTS_UNSHIPPED_API),$(WASM_API_EXPORTS) $(WASM_BOTS_API_EXPORTS)) \
                            $(WASM_TABLE_EXPORTS) $(WASM_CLIENT_EXPORTS) $(WASM_REPLAY_WEB_EXPORTS)

# AND THIS FILE, because the export list is IN this file. Every wasm link below
# reads a WASM_*_EXPORTS list, and none of them named the Makefile, so a
# changed list relinked nothing: measured on 2026-09-19, dropping
# wasm_replay_error_detail from WASM_BOTS_UNSHIPPED_API and running
# `make wasm-bots` reported the module up to date and re-gzipped the OLD bytes.
# The export table is as much an input to the link as an object file is, and
# the only place it lives is here. A relink is seconds and the output is
# reproducible (gzip -n over a deterministic link), so a Makefile edit that
# changes nothing leaves the tree clean rather than dirty.
build/bots.wasm: $(WASM_BOT_OBJ) Makefile
	@mkdir -p build
	$(WASM_CC) --target=wasm32 -nostdlib $(WASM_BOT_LDFLAGS) \
	          $(WASM_BOTS_SHIPPED_EXPORTS) $(WASM_BOT_OBJ) -o $@
	$(call wasm_postopt,$(WASM_BOTS_POSTOPT),$@)

# bots-explain.wasm: byte-for-byte the SAME octogen as bots.wasm (same sources,
# same per-file opt, same flags, same wasm-opt pass) PLUS -DOG_EXPLAIN_BUILD,
# which is behavior-neutral (the deliberation dump is #ifdef'd + gated and resets
# the TT). Extra exports let the JS harness read the per-decision dump from a
# static buffer (wasm has no file I/O). ANALYSIS ONLY — never shipped.
.PHONY: bots-wasm-explain
bots-wasm-explain: build/layout/bots-explain.hash
	@mkdir -p build/botexplain
	@for src in $(WASM_BOT_SRC); do \
	  base=$$(basename $$src .c); opt=-Oz; \
	  case " $(WASM_BOT_O3_SRC) " in *" $$src "*) opt=-O3;; esac; \
	  echo "  CC(explain) $$src [$$opt]"; \
	  $(WASM_CC) $(WASM_BOT_CFLAGS) -DOG_EXPLAIN_BUILD -DSG_LAYOUT_HASH=$$(cat build/layout/bots-explain.hash) $$opt -c $$src -o build/botexplain/$$base.o || exit 1; \
	done
	$(WASM_CC) --target=wasm32 -nostdlib $(WASM_BOT_LDFLAGS) \
	          $(WASM_BOTS_SHIPPED_EXPORTS) \
	          -Wl,--export=wasm_og_explain_ptr -Wl,--export=wasm_og_explain_len -Wl,--export=wasm_og_explain_reset \
	          build/botexplain/*.o -o build/bots-explain.wasm
	$(call wasm_postopt,$(WASM_BOTS_POSTOPT),build/bots-explain.wasm)
	gzip -9 -c build/bots-explain.wasm > build/bots-explain.wasm.gz
	@echo "built build/bots-explain.wasm(.gz) — load via the harness"

# Name-preserving companion for the wasm-anatomy tool (tools/wasm_anatomy). It
# must have a CODE section BYTE-IDENTICAL to build/bots.wasm but keep the
# function name section, so the disassembler maps names 1:1 onto the shipped
# (stripped) bytes. Same objects + same link + same wasm-opt pass as
# build/bots.wasm, differing ONLY by: no -Wl,--strip-all, and wasm-opt keeps
# names via --debuginfo. Because the wasm-opt pass now reshapes the module
# (inlining removes functions), a companion WITHOUT the pass would have
# different function indices and mis-map every name — hence this target runs the
# identical pass. generate.sh re-strips this and asserts it equals build/bots.wasm.
build/named/bots.named.wasm: $(WASM_BOT_OBJ) Makefile
	@mkdir -p build/named
	$(WASM_CC) --target=wasm32 -nostdlib $(filter-out -Wl$(comma)--strip-all,$(WASM_BOT_LDFLAGS)) \
	          $(WASM_BOTS_SHIPPED_EXPORTS) $(WASM_BOT_OBJ) -o $@
	$(if $(strip $(WASM_BOTS_POSTOPT)),$(WASM_OPT_BIN) $(WASM_OPT_FEATURES) $(WASM_BOTS_POSTOPT) --debuginfo $@ -o $@,:)

# bots.wasm ships as ONE gzip STATIC ASSET (config.toml static_files + deploy
# --use-api, no Docker) — a real binary, not a base64 .ts embed.
#
# EVERY host reads this one file. The server (Deno edge / Node) with
# fs/readFileSync; the browser by FETCHING it, because it has neither — see
# sdk/ts/wasm/wasm_asset.ts. The browser needs the big module for two reasons
# that arrived together: FMSG's decode/seal live only here (sealing reads a
# resident session log rules.wasm cannot hold, and /m/ is a web page), and A5's
# replay_steps does too (~272 KB of decoded actions vs a 196,608 B pin). One
# module behind every host is the steer.
#
# There was briefly a base64 twin here for the browser. It is deliberately gone:
# a second carrier of the same bytes is exactly how rules_wasm.ts went stale for
# weeks while this .gz kept being rebuilt — two kernels in one tree, and a C
# change invisible to every TS test that loaded the stale one. It also put 80 KB
# of single-line base64, rewritten on every kernel build, into git. The price is
# that the browser's door is async (ensureBotsAsync); that is the cheaper half.
wasm-bots: build/layout/bots.hash
	$(call sg_settle,bots,build/botobj,build/bots.wasm)
	@$(MAKE) --no-print-directory build/bots.wasm
	gzip -9 -n -c build/bots.wasm > ../sdk/ts/wasm/bots.wasm.gz

# The test build (WASM_TEST_EXPORTS above): never committed, never shipped.
# build/bots_test.stamp records the wasm source hash it was built from
# (scripts/wasm_stamp.sh --hash), which is how the e2e helper knows it is current.
build/bots_test.wasm: $(WASM_BOT_OBJ) Makefile
	@mkdir -p build
	$(WASM_CC) --target=wasm32 -nostdlib $(WASM_BOT_LDFLAGS) \
	          $(WASM_BOTS_SHIPPED_EXPORTS) $(WASM_TEST_EXPORTS) $(WASM_BOT_OBJ) -o $@.tmp
	$(call wasm_postopt,$(WASM_BOTS_POSTOPT),$@.tmp)
	mv $@.tmp $@

.PHONY: wasm-bots-test
wasm-bots-test: build/layout/bots.hash
	$(call sg_settle,bots,build/botobj,build/bots_test.wasm)
	@$(MAKE) --no-print-directory build/bots_test.wasm
	@bash ../scripts/wasm_stamp.sh --hash > build/bots_test.stamp.tmp && mv build/bots_test.stamp.tmp build/bots_test.stamp

# ---------------------------------------------------------------------------
# oracle.wasm — the client-side "infinite oracle" replay analyzer
# (docs/INFINITE_ORACLE_DESIGN.md). bots.wasm's brain + the OG_EXPLAIN
# per-candidate dump + oracle-only hooks (-DFOOLISH_ORACLE_BUILD), rebuilt for
# the browser's unconstrained-memory regime: TT20 (+2WAY+PACK8 = 8 MiB runtime
# table — a strength knob, Makefile:296-304) and the default 256 KiB stack (no
# 22528 override). CD_WASM_OVERLAY stays: the explain sink requires it
# (octogen_strategy.c) and repeated complete calls respect the tenancy. NEVER
# replaces the shipped modules; ships as a committed public/oracle.wasm.gz the
# replay page fetches lazily on the first Oracle button press.
WASM_ORACLE_CFLAGS := $(filter-out -DCD_TT_BITS=12,$(WASM_BOT_CFLAGS)) \
                      -DCD_TT_BITS=20 -DOG_EXPLAIN_BUILD -DFOOLISH_ORACLE_BUILD
WASM_ORACLE_LDFLAGS := $(filter -Wl%,$(WASM_FLAGS))
WASM_ORACLE_EXPORTS := $(WASM_API_EXPORTS) $(WASM_BOTS_API_EXPORTS) \
  -Wl,--export=wasm_og_explain_ptr -Wl,--export=wasm_og_explain_len \
  -Wl,--export=wasm_og_explain_reset -Wl,--export=wasm_og_reload_flags \
  -Wl,--export=wasm_og_paths_ptr -Wl,--export=wasm_og_paths_len
.PHONY: wasm-oracle
wasm-oracle: build/layout/oracle.hash
	@mkdir -p build/botoracle
	@for src in $(WASM_BOT_SRC); do \
	  base=$$(basename $$src .c); opt=-Oz; \
	  case " $(WASM_BOT_O3_SRC) " in *" $$src "*) opt=-O3;; esac; \
	  echo "  CC(oracle) $$src [$$opt]"; \
	  $(WASM_CC) $(WASM_ORACLE_CFLAGS) -DSG_LAYOUT_HASH=$$(cat build/layout/oracle.hash) $$opt -c $$src -o build/botoracle/$$base.o || exit 1; \
	done
	$(WASM_CC) --target=wasm32 -nostdlib $(WASM_ORACLE_LDFLAGS) \
	          $(WASM_ORACLE_EXPORTS) build/botoracle/*.o -o build/oracle.wasm
	$(call wasm_postopt,$(WASM_BOTS_POSTOPT),build/oracle.wasm)
	gzip -9 -n -c build/oracle.wasm > ../public/oracle.wasm.gz
	@echo "built ../public/oracle.wasm.gz ($$(wc -c < ../public/oracle.wasm.gz) B gz, $$(wc -c < build/oracle.wasm) B raw)"

# ---------------------------------------------------------------------------
# oracle-mt.wasm - Mode B (docs/INFINITE_ORACLE_DESIGN.md §8b): the same octogen
# brain rebuilt for SHARED-MEMORY THREADS. Real TLS restored (no -D_Thread_local=)
# so every RNG, the solver's transposition-table pointer and all cordite scratch
# is genuinely per-thread - the proven native OMP model. Coordination (job
# control, seeding, accumulation) lives in C atomics (wasm/wasm_oracle_mt.c);
# the TS/worker side is a thin trampoline.
#
# Dropped from the Mode A oracle build, both required:
#   -DCD_WASM_OVERLAY  its fixed-address aliasing into solve_ws is only legal
#                      when TLS is stripped (wasm/wasm_overlay.h says so); with
#                      real TLS it is the native data race.
#   -DOG_EXPLAIN_BUILD its wasm sink is #ifdef CD_WASM_OVERLAY and would not
#                      link; Mode B replaces the JSON dump with the C accumulator.
# A SECOND committed artifact (public/oracle-mt.wasm.gz); the browser loader
# picks it only when crossOriginIsolated. NEVER replaces the shipped modules.
WASM_ORACLE_MT_SRC := $(WASM_BOT_SRC) wasm/wasm_oracle_mt.c
WASM_ORACLE_MT_CFLAGS := $(filter-out -DCD_WASM_OVERLAY -DCD_TT_BITS=12 -D_Thread_local=,$(WASM_BOT_CFLAGS)) \
                         -DCD_TT_BITS=20 -matomics \
                         -DFOOLISH_ORACLE_BUILD -DFOOLISH_ORACLE_MT
WASM_ORACLE_MT_LDFLAGS := $(filter-out -Wl$(comma)--export-memory,$(filter -Wl%,$(WASM_FLAGS))) \
                          -Wl,--shared-memory -Wl,--import-memory -Wl,--max-memory=134217728 \
                          -Wl,--export=__stack_pointer -Wl,--export=__wasm_init_tls \
                          -Wl,--export=__tls_size -Wl,--export=__tls_align
WASM_ORACLE_MT_EXPORTS := $(WASM_API_EXPORTS) $(WASM_BOTS_API_EXPORTS) \
  -Wl,--export=wasm_og_reload_flags \
  -Wl,--export=wasm_mt_reserve \
  -Wl,--export=wasm_mt_warmup -Wl,--export=wasm_mt_setup -Wl,--export=wasm_mt_stop \
  -Wl,--export=wasm_mt_total -Wl,--export=wasm_mt_checksum -Wl,--export=wasm_mt_active \
  -Wl,--export=wasm_mt_thread_main \
  -Wl,--export=wasm_mt_ncand -Wl,--export=wasm_mt_ready -Wl,--export=wasm_mt_batches \
  -Wl,--export=wasm_mt_chosen -Wl,--export=wasm_mt_sumfp -Wl,--export=wasm_mt_nsim \
  -Wl,--export=wasm_mt_forced -Wl,--export=wasm_mt_candidates \
  -Wl,--export=wasm_mt_mismatch -Wl,--export=wasm_mt_canary \
  -Wl,--export=wasm_mt_solver -Wl,--export=wasm_mt_verdict -Wl,--export=wasm_mt_defuse
.PHONY: wasm-oracle-mt
wasm-oracle-mt: build/layout/oracle-mt.hash
	@mkdir -p build/botoraclemt
	@for src in $(WASM_ORACLE_MT_SRC); do \
	  base=$$(basename $$src .c); opt=-Oz; \
	  case " $(WASM_BOT_O3_SRC) " in *" $$src "*) opt=-O3;; esac; \
	  echo "  CC(oracle-mt) $$src [$$opt]"; \
	  $(WASM_CC) $(WASM_ORACLE_MT_CFLAGS) -DSG_LAYOUT_HASH=$$(cat build/layout/oracle-mt.hash) $$opt -c $$src -o build/botoraclemt/$$base.o || exit 1; \
	done
	$(WASM_CC) --target=wasm32 -nostdlib $(WASM_ORACLE_MT_LDFLAGS) \
	          $(WASM_ORACLE_MT_EXPORTS) build/botoraclemt/*.o -o build/oracle-mt.wasm
	$(WASM_OPT_BIN) $(WASM_OPT_FEATURES) --enable-threads $(WASM_BOTS_POSTOPT) build/oracle-mt.wasm -o build/oracle-mt.wasm
	gzip -9 -n -c build/oracle-mt.wasm > ../public/oracle-mt.wasm.gz
	@echo "built ../public/oracle-mt.wasm.gz ($$(wc -c < ../public/oracle-mt.wasm.gz) B gz, $$(wc -c < build/oracle-mt.wasm) B raw)"

# ---------------------------------------------------------------------------
# Coverage gate (CI: ../.github/workflows/coverage.yml). Instruments the engine
# with gcc --coverage, runs the committed unit suite + the difftests, and fails
# if core-engine (game/legal/replay) LINE or BRANCH coverage regresses below the
# floor. Uses gcc + gcovr (not clang/llvm-cov) so CI needs no llvm profile
# runtime. Objects (and their .gcno/.gcda) land in covbuild/ where gcovr finds
# them recursively. Tune the floors with COV_LINE_MIN / COV_BRANCH_MIN.
COV_CC         ?= gcc
COV_FLAGS      := -O0 -g --coverage -Isrc -Wno-unused-parameter -Wno-deprecated-declarations
COV_LINE_MIN   ?= 90
COV_BRANCH_MIN ?= 74
# THE WHOLE SHIPPED KERNEL, not three files of it.
#
# This list was game/legal/replay back when those were most of what C owned. A
# great deal has moved here since - the wire formats, the view masking, the
# animation planner, the table and session layer, the iMessage envelope - and
# none of it was gated. The gate measured 2,304 lines while the kernel had
# grown to 7,738, so a file could lose half its coverage and the build stayed
# green.
#
# What is NOT here, deliberately: the bot strategies. They are research code,
# eleven of them are arena-only and never linked into anything shipped, and
# holding a Monte-Carlo search to a line floor would gate the build on how
# often a rollout happens to visit a branch. `make tests` still runs them.
#
# The floors stay at 90/74 even though the measured numbers are 94/79, because
# CI runs real gcc and this was measured with Apple clang plus llvm-cov, and a
# few points of toolchain drift must not turn into a red main. Raise them as
# coverage climbs; never lower one to make a red build pass.
COV_KERNEL     := game legal replay replay_steps replay_extras view awire evwire \
                  msg_wire msg_expand anim_plan table client_table roster card \
                  bot_drive bot_roster analyse
# deal_rng and sha256 are in the SAME gate, from their new home. They left this
# list's `src/` prefix behind when they moved to shared/, and a filter that no
# longer matches does not fail - gcovr simply reports the file it cannot find as
# nothing at all, which is how a shared primitive quietly stops being measured.
# Anchored loosely (.*) because gcovr matches these against the path it resolved,
# which is absolute.
COV_SHARED     := deal_rng sha256
COV_FILTERS    := $(foreach f,$(COV_KERNEL),--filter 'src/$(f)\.c') \
                  $(foreach f,$(COV_SHARED),--filter '.*shared/c/$(f)\.c')

.PHONY: coverage
coverage:
	rm -rf covbuild && mkdir -p covbuild
	$(COV_CC) $(COV_FLAGS) $(CORE_SRC) tests/tests.c          -o covbuild/cnitro_tests    -lm
	$(COV_CC) $(COV_FLAGS) $(CORE_SRC) tests/sim_difftest.c   -o covbuild/sim_difftest    -lm
	$(COV_CC) $(COV_FLAGS) $(CORE_SRC) tests/apply_difftest.c -o covbuild/apply_difftest  -lm
	$(COV_CC) $(COV_FLAGS) $(CORE_SRC) tests/solver_difftest.c -o covbuild/solver_difftest -lm
	$(COV_CC) $(COV_FLAGS) $(CORE_SRC) tests/replay_v6_test.c  -o covbuild/replay_v6_test  -lm
	# THE SUITES `make difftests` RUNS AND THIS ONCE DID NOT. Coverage measured
	# five of the eleven suites, so a file could read 0% while being thoroughly
	# tested by a binary nobody built here. src/msg_wire.c - 644 lines, the FMSG
	# envelope the whole iMessage product rides on - reported ZERO, with a
	# 4,427-line test suite sitting beside it. Measuring a subset does not
	# understate coverage, it MISDIRECTS it: the number points at the wrong
	# files, and the gate protects whatever happens to be in the subset.
	$(COV_CC) $(COV_FLAGS) $(CORE_SRC) tests/msg_wire_test.c   -o covbuild/msg_wire_test   -lm
	$(COV_CC) $(COV_FLAGS) $(CORE_SRC) tests/anim_plan_test.c  -o covbuild/anim_plan_test  -lm
	$(COV_CC) $(COV_FLAGS) $(CORE_SRC) tests/msg_expand_test.c -o covbuild/msg_expand_test -lm
	$(COV_CC) $(COV_FLAGS) $(CORE_SRC) tests/analyse_test.c    -o covbuild/analyse_test    -lm
	./covbuild/cnitro_tests
	./covbuild/sim_difftest 4 100
	./covbuild/apply_difftest 4 50
	./covbuild/solver_difftest 2 40 "" 5000
	./covbuild/solver_difftest 3 40 "" 5000
	./covbuild/solver_difftest 4 40 "" 5000
	./covbuild/replay_v6_test 10
	./covbuild/msg_wire_test 20
	./covbuild/anim_plan_test
	./covbuild/msg_expand_test
	./covbuild/analyse_test
	# A NEW KERNEL FILE MUST NOT BE ABLE TO SLIP PAST THE GATE. COV_KERNEL is a
	# hand-written list, and this repo has been bitten four times in one day by
	# a check that named files while the files moved underneath it. Every source
	# the iOS app links is, by definition, shipped kernel - so if one of those is
	# missing from COV_KERNEL, say which, and fail. (The reverse is allowed:
	# COV_KERNEL also holds table/bot_drive/bot_roster/analyse, which the wasm
	# and the servers link but the phone does not.)
	@missing=""; for f in $(notdir $(basename $(filter src/%,$(IOS_CORE_SRC)))); do \
	   case " $(COV_KERNEL) " in *" $$f "*) ;; *) missing="$$missing $$f" ;; esac; \
	 done; \
	 if [ -n "$$missing" ]; then \
	   echo "coverage: these are linked into the iOS app but are not in COV_KERNEL:$$missing" >&2; \
	   echo "coverage: add them to COV_KERNEL in c/Makefile, or say in a comment why they are exempt." >&2; \
	   exit 1; \
	 fi
	# --gcov-ignore-parse-errors: gcov miscounts a hot loop, and adding the four
	# suites above made it hot enough to matter. game.c:1211 (the battle scan,
	# entered on nearly every kernel call) came back with a hit count of
	# 5,047,157,994 - a known gcov defect, gcc bugzilla 68080 - and gcovr's
	# default is to treat a suspicious count as a corrupt report and ABORT.
	#
	# That failure is the dangerous kind: gcovr exits non-zero having read only
	# part of the data, so the run looks like a coverage FAILURE while actually
	# being a coverage NON-MEASUREMENT. Measured here: the aborted run reported
	# src/msg_wire.c at 0%, which is wrong by ninety-one points.
	#
	# warn_once_per_file, not the silent form: a miscounted line is still worth
	# one line of output, and if this ever starts firing on files that are not
	# hot loops that is a signal, not noise.
	# BOTH SPELLINGS, because the two platforms overflow the same counter
	# differently: Apple's gcov reports the wrapped value as a huge positive
	# (suspicious_hits, game.c:1211 got 5,047,157,994) and Linux's reports it as
	# a negative (negative_hits, game.c:70 branch 1 taken -7407). Fixing only the
	# one a Mac produces left this lane green here and red on every runner.
	#
	# `all`, because the option takes ONE value and the two classes cannot both
	# be named: comma-joined is a single unrecognised choice, and space-separated
	# leaves the second token to be read as a search directory ("Unknown
	# directory 'negative_hits.warn_once_per_file'"). Both were tried here.
	# `all` still prints what it skipped to stderr, so a miscount that is not one
	# of these two hot-loop overflows is still visible rather than swallowed.
	gcovr --root . $(COV_FILTERS) --txt \
	      --gcov-ignore-parse-errors all \
	      --fail-under-line $(COV_LINE_MIN) --fail-under-branch $(COV_BRANCH_MIN)

clean:
	rm -rf build covbuild
