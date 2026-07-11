CC      ?= clang
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
endif

# The engine + all heuristic / Monte-Carlo bots. Linked into every binary.
CORE_SRC := src/game.c src/deal_rng.c src/legal.c src/replay.c src/view.c src/awire.c \
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
            src/torpex_value.c src/novichok_strategy.c
CORE_OBJ := $(CORE_SRC:src/%.c=build/%.o)

.PHONY: all clean tests print-core
# Emit the core engine+strategy source list (used by tools/tt_divergence.sh to
# build cnitro_eval at arbitrary -DCD_TT_BITS without duplicating the list).
print-core:
	@echo $(CORE_SRC)
all: build/cnitro_eval build/cnitro_elo build/cnitro_replay build/cnitro_showcase build/cnitro_tests build/cnitro_gen

# Generic compile rule for sources under src/ (build/%.o: src/%.c).
# Blanket header dependency: a struct-layout change in any header must
# rebuild every object, or the binaries mix layouts (observed: SimState
# edits producing garbage evals until a make clean).
build/%.o: src/%.c $(wildcard src/*.h)
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
build/tests.o: tests/tests.c $(wildcard src/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

tests: build/cnitro_tests
	./build/cnitro_tests

# Bitboard-rollout difftests (not part of the default suite). Each plays many
# games and asserts the bitboard engine matches the struct engine.
build/sim_difftest: tests/sim_difftest.c $(CORE_OBJ)
	$(CC) $(CFLAGS) tests/sim_difftest.c $(CORE_OBJ) -o $@ $(LDFLAGS)
build/apply_difftest: tests/apply_difftest.c $(CORE_OBJ)
	$(CC) $(CFLAGS) tests/apply_difftest.c $(CORE_OBJ) -o $@ $(LDFLAGS)
build/solver_difftest: tests/solver_difftest.c $(CORE_OBJ)
	$(CC) $(CFLAGS) tests/solver_difftest.c $(CORE_OBJ) -o $@ $(LDFLAGS)
# Replay-codec round trip against real engine games (native mirror of
# e2e/replay_codec.test.ts; also part of `make difftests`).
build/replay_difftest: tests/replay_difftest.c $(CORE_OBJ)
	$(CC) $(CFLAGS) tests/replay_difftest.c $(CORE_OBJ) -o $@ $(LDFLAGS)
difftests: build/sim_difftest build/apply_difftest build/solver_difftest build/replay_difftest
	./build/sim_difftest 4 1000 && ./build/apply_difftest 4 200 && \
	./build/solver_difftest 2 40 "" 200000 && \
	./build/solver_difftest 3 40 "" 200000 && \
	./build/solver_difftest 4 40 "" 200000 && \
	./build/replay_difftest 40

# L1 memory budget measurement (docs/WASM_L1_BUDGET.md). Plays ~63K real games
# and reports the observed maxima that size every wasm static buffer. Built at
# the WASM cap set + headroom so the numbers are true maxima; -DREPLAY_STATS
# turns on the (otherwise compiled-out) replay coder peak counters.
# `make l1-measure GAMES=1500` runs the sweep across seeds.
GAMES ?= 1500
build/l1_measure: tests/l1_measure.c $(wildcard src/*.h) src/game.c src/deal_rng.c src/legal.c src/replay.c src/view.c src/random_strategy.c src/handwritten_strategy.c
	@mkdir -p build
	$(CC) -O2 -Isrc -DMAX_LOG_PAIRS=64 -DMAX_BATTLES=64 -DMAX_LOGS=2048 \
	  -DMAX_LEGAL_MOVES=16384 -DMAX_MOVE_CARDS=52 -DREPLAY_STATS \
	  src/game.c src/deal_rng.c src/legal.c src/replay.c src/view.c \
	  src/random_strategy.c src/handwritten_strategy.c tests/l1_measure.c -o $@ -lm
.PHONY: l1-measure
l1-measure: build/l1_measure
	./build/l1_measure $(GAMES) 1 && ./build/l1_measure $(GAMES) 99991

# ---------------------------------------------------------------------------
# WebAssembly rules kernel: game.c + legal.c + the wasm bridge, compiled
# freestanding (no libc; wasm/include has the tiny shims, wasm_api.c provides
# memcpy/memset). This is THE production rules engine: the TS server imports
# the resulting module via supabase/functions/_shared/wasm/. `-D_Thread_local=`
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
WASM_FLAGS := --target=wasm32 $(WASM_OPT) -nostdlib -ffreestanding -mbulk-memory \
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
  -Wl,--export=wasm_game_done -Wl,--export=wasm_should_act \
  -Wl,--export=wasm_next_player -Wl,--export=wasm_can_cover \
  -Wl,--export=wasm_replay_io_ptr -Wl,--export=wasm_replay_io_cap \
  -Wl,--export=wasm_replay_encode -Wl,--export=wasm_replay_decode \
  -Wl,--export=wasm_replay_error_detail \
  -Wl,--export=wasm_legal_moves -Wl,--export=wasm_export_moves \
  -Wl,--export=wasm_apply_action -Wl,--export=wasm_finalize_win \
  -Wl,--export=wasm_view_serialize -Wl,--export=wasm_events_serialize \
  -Wl,--export=wasm_rearrange_hand \
  -Wl,--export=wasm_set_deterministic_deck \
  -Wl,--export=wasm_seed_rng_deterministic \
  -Wl,--export=wasm_set_strategy_seed_deterministic \
  -Wl,--export=wasm_set_rng_base

# bots.wasm exports the rules API above PLUS the bot bridge (wasm_bots_api.c).
WASM_BOTS_API_EXPORTS := \
  -Wl,--export=wasm_setenv_from_io -Wl,--export=wasm_clearenv \
  -Wl,--export=wasm_import_logs -Wl,--export=wasm_clear_logs \
  -Wl,--export=wasm_set_game_key -Wl,--export=wasm_import_strategy_keys \
  -Wl,--export=wasm_set_strategy_seed -Wl,--export=wasm_reload_bot_flags \
  -Wl,--export=wasm_choose_move

# The rules module's move buffer only feeds the exported menu — production
# validation is direct rule-checking, and production menus/bot enumeration
# run on the adopted bots instance (engine slot adoption, see engine.ts).
# 1024 covers every menu the parity/fuzz suites produce; the bots module
# keeps the native 4096 for bot candidate lists.
# MAX_LOGS=128: the rules module never imports session logs (that is
# wasm_import_logs, bots-only) and each marshal restarts num_logs at 0, so
# g_game only ever holds ONE action's logs — hard worst ~76 (a 64-cover
# round's discard + draws + transitions). The bots module MUST keep 512:
# its belief imports the whole session log (TS MAX_KERNEL_LOGS mirror).
# Stack 32KB (R4, docs/RULES_GUARDS_WASM_MEMORY_PLAN.md): no solver, no
# rollouts — the deepest rules path is the cover enumeration. A wasm stack
# canary (paint 0xA5, drive the cover + attack-combination corpus through the
# production kernelLegalMoves marshal, scan) measures the realistic worst at
# 14.3 KiB (calc_cover_moves' CoverOption opts[64] ~= 9 KiB single frame, plus
# ~576 B/level over n_uncovered levels of choose_attack_subset). 32 KiB is
# 2.23x that. The old 64 KiB was sized for the analytic MAX_BATTLES=64 worst
# (~45 KiB), but that state is unreachable: choose_attack_subset enumerates
# 2^n_uncovered subsets, so it HANGS by ~n_uncovered 28-30 — long before the
# ~40-battle depth that would overflow even the 32 KiB stack. So no state that
# completes at 64 KiB traps at 32 KiB; --stack-first keeps any surprise a loud
# trap that the fuzz + cover e2e suites (which drive enumeration harder than
# production) exercise directly. The trailing -z stack-size overrides the
# shared flags' 256KB (last one wins in wasm-ld). Net: -32 KiB stack, and with
# the R1 arena at 102,544 B the module drops 4 -> 3 pages.
# WASM_IO_CAP=24576: this build's widest unchunked io write is the log
# export at MAX_LOGS=128 — 2 + 128 x (4 + 64x2) = 16,898B; 24KB is 1.45x
# that (the 72KB default exists for bots' 512-log session export).
# Together with the shared L1 caps above, rules.wasm linear memory drops
# 53 pages (3.31MB) -> 5 pages (320KB).
# PIN (R0, docs/RULES_GUARDS_WASM_MEMORY_PLAN.md): --initial-memory ==
# --max-memory freezes rules.wasm linear memory exactly like the guards pin
# (see WASM_GUARDS_FLAGS below). The module has no allocator and never calls
# memory.grow (all state is the static caps above), so a fixed size is a hard
# invariant: if any buffer grows past the pin, wasm-ld REFUSES TO LINK, and any
# runtime memory.grow traps. This is what makes the tight R1 headroom a build
# error instead of a production surprise. Re-pin LOWER after each landed
# candidate. 196608 = 3 pages after the R1 arena overlay + R4 stack shrink
# (below); 4 pages (262144) was the post-overlay floor and 5 pages (327680)
# the pre-overlay floor.
# CD_RULES_OVERLAY (R1, docs/RULES_GUARDS_WASM_MEMORY_PLAN.md): alias the
# replay-call scratch family (g_rec + g_bn + g_replay_io, ~90 KiB) over the
# action family (g_moves + g_snaps + g_io) in one arena (src/rules_overlay.h,
# g_rules_arena in wasm_api.c). The two families are provably never live at the
# same time (single-threaded module; replay encode/decode vs action/menu are
# top-level exports that never nest, and each buffer is written-before-read
# within its call). Saves ~93 KiB → 5 pages -> 4 pages. rules.wasm ONLY: the
# bots build aliases into solve_ws instead (CD_WASM_OVERLAY), native uses plain
# statics; the two flavors are mutually exclusive (#error in rules_overlay.h).
WASM_RULES_FLAGS := $(filter-out -DMAX_LEGAL_MOVES=4096,$(WASM_FLAGS)) \
                    -DMAX_LEGAL_MOVES=1024 -DMAX_LOGS=128 -DWASM_IO_CAP=24576 \
                    -DCD_RULES_OVERLAY \
                    -Wl,-z,stack-size=32768 \
                    -Wl,--initial-memory=196608 -Wl,--max-memory=196608

# Blanket header dependency, same rationale as the native objects: a header
# edit must rebuild BOTH wasm modules or they ship divergent kernels.
WASM_RULES_SRC := src/game.c src/deal_rng.c src/legal.c src/replay.c src/view.c src/awire.c \
                  src/evwire.c wasm/wasm_api.c

build/rules.wasm: $(WASM_RULES_SRC) $(wildcard src/*.h) $(wildcard wasm/*.h)
	@mkdir -p build
	$(WASM_CC) $(WASM_RULES_FLAGS) $(WASM_API_EXPORTS) $(WASM_RULES_SRC) -o $@
	$(call wasm_postopt,$(WASM_POSTOPT),$@)

# Regenerates the base64 module the TS side imports (works unchanged in
# Deno edge functions, Node and browsers — no asset pipeline needed).
# rules stays a base64 embed: engine.ts is shared with the BROWSER (replay
# decode imports it), and browsers have no node:fs/node:zlib for the static-gz
# loader. Only bots.wasm — server-only — ships as a gzip static asset (below).
wasm: build/rules.wasm
	node wasm/embed.mjs build/rules.wasm ../supabase/functions/_shared/wasm/rules_wasm.ts --gzip

# Client-guards module: game.c ONLY — no legal.c enumeration, no replay.c
# codec. It backs the browser's UI move-gates (validate-only) and optimistic
# apply (see src/wasm/clientGuards.ts). The smallest kernel we ship: it exists
# so the interactive client stops reimplementing the rules in TS (one engine,
# not two). Same wire layout as rules.wasm; e2e asserts guards==rules parity.
# Smaller caps than the rules module: the client never imports a session log
# (each marshal starts fresh) and one optimistic move emits <64 logs.
#
# THE L1 MODULE: linear memory is PINNED to one 64KB wasm page — the whole
# module state (stack, Game, clone scratch, io, card buffers) lives inside
# a single page that fits an L1 data cache (Neoverse/Graviton L1d is
# exactly 64KB; Apple M-series is 128KB). --initial-memory=--max-memory
# makes it a hard link-time + run-time invariant: if a buffer grows past
# the page, wasm-ld REFUSES TO LINK (no silent regression), and the module
# can never memory.grow (it has no allocator — verified: no memory.grow
# opcode in the binary). Budget & measurements: docs/WASM_L1_BUDGET.md.
# Stack 16KB: the deepest guards path was validate_run's ~9.6KB Game clone,
# which now lives in BSS (wasm_guards_api.c g_validate_tmp); what remains on
# the stack is handle_* / state_get frames (<2KB observed) — 16KB is >5x
# that, and --stack-first keeps any regression a loud trap.
# MAX_SNAPS=1: the shipped guards module exports no snapshot readers (see
# WASM_GUARDS_EXPORTS below), so the ring was 55.7KB of write-only memory.
WASM_GUARDS_FLAGS := --target=wasm32 $(WASM_OPT) -nostdlib -ffreestanding -mbulk-memory \
                     -isystem wasm/include -Isrc -D_Thread_local= -DDEAL_RNG_DISABLED \
                     -DMAX_LOG_PAIRS=64 -DMAX_BATTLES=64 -DMAX_LOGS=64 -DMAX_SNAPS=1 \
                     -Wall -Wextra -Wno-unused-parameter \
                     -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=16384 -Wl,--stack-first \
                     -Wl,--initial-memory=65536 -Wl,--max-memory=65536 \
                     -Wl,--strip-all

# Explicit export allow-list, same rationale as WASM_API_EXPORTS (this
# replaced --export-all here too: it pinned every internal symbol as a GC
# root and bloated the name section). A NEW wasm_* guards entry point must be
# added here or the browser bridge won't see it (clientGuards.ts fails loudly).
# Trimmed to the roots LIVE client code + the guards tests actually reach
# (see e2e/client_guards.test.ts). Everything dropped here — the snapshot /
# log-readout exports, wasm_set_seed/io_cap/reject_reason, the awire-apply
# entry (wasm_apply_action), and the good-move entries (wasm_good /
# wasm_validate_good, subsumed by wasm_validate_action which handles all five
# kinds) — was reachable from NO caller in src/ or e2e/, so rooting it only
# bloated the shipped module. Re-add an export line to revive a capability;
# the C stays in game.c / wasm_guards_api.c. The kept wasm_attack..pickup +
# wasm_export_state are dormant in prod (the TS overlay does optimistic play)
# but guarded by the applyMove prediction test, so they stay.
WASM_GUARDS_EXPORTS := \
  -Wl,--export=wasm_io_ptr \
  -Wl,--export=wasm_cards_a_ptr -Wl,--export=wasm_cards_b_ptr \
  -Wl,--export=wasm_init \
  -Wl,--export=wasm_import_state -Wl,--export=wasm_export_state \
  -Wl,--export=wasm_validate_attack -Wl,--export=wasm_validate_cover \
  -Wl,--export=wasm_validate_pass -Wl,--export=wasm_validate_pickup \
  -Wl,--export=wasm_validate_action \
  -Wl,--export=wasm_attack -Wl,--export=wasm_cover -Wl,--export=wasm_pass \
  -Wl,--export=wasm_pickup \
  -Wl,--export=wasm_game_done -Wl,--export=wasm_next_player -Wl,--export=wasm_can_cover

# The client guards module never deals (its optimistic draws are placeholders
# and it never learns the seed), so -DDEAL_RNG_DISABLED compiles out the ChaCha
# deal RNG entirely and deal_rng.c is not linked — no crypto weight in the
# browser bundle. draw_card falls back to the trivial LCG index for its
# placeholder splices, exactly as before this feature.
WASM_GUARDS_SRC := src/game.c src/view.c src/awire.c wasm/wasm_guards_api.c

build/guards.wasm: $(WASM_GUARDS_SRC) $(wildcard src/*.h) $(wildcard wasm/*.h)
	@mkdir -p build
	$(WASM_CC) $(WASM_GUARDS_FLAGS) $(WASM_GUARDS_EXPORTS) $(WASM_GUARDS_SRC) -o $@
	$(call wasm_postopt,$(WASM_POSTOPT),$@)

wasm-guards: build/guards.wasm
	node wasm/embed.mjs build/guards.wasm ../supabase/functions/_shared/wasm/guards_wasm.ts GUARDS_WASM_B64 --gzip

# Bot module: the rules kernel PLUS every algorithmic bot strategy and the
# choose-move bridge. A superset of rules.wasm, loaded only where bots run
# (edge functions' bot turns, offline harnesses) so the pure rules path keeps
# the smaller module.
# Note: the distilled strategy (research, strength-negative — see
# CORDITE_RESEARCH.md) stays native-arena-only; it is deliberately NOT in the
# production bot module.
# Only the SHIPPED difficulty-ladder bots (Durak Bot Ordnance Chart) that are
# already ported to wasm — random, simple_heuristic, handwritten_prod, cordite,
# octogen — plus the two heuristics the shipped MC bots call as rollout policies:
# espresso and handwritten (arena builds, cordite_strategy.c:591-594). Dropped
# entirely (no ladder slot, no dep of a shipped bot): champion, ultimate_champion,
# hacker, fulminate, semtex, espresso_prod — their code + static data leave the
# module. (firecracker/blackpowder are ladder rungs not yet ported to wasm; add
# their .c + robusta_strategy.c when they land.)
WASM_BOT_SRC := src/game.c src/deal_rng.c src/legal.c src/replay.c src/view.c src/awire.c \
                src/evwire.c src/random_strategy.c \
                src/espresso_strategy.c src/handwritten_strategy.c \
                src/cordite_strategy.c src/cordite_sim.c \
                src/simple_heuristic_strategy.c \
                src/handwritten_prod_strategy.c \
                src/octogen_strategy.c wasm/wasm_api.c wasm/wasm_bots_api.c

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
WASM_BOT_CFLAGS  := $(filter-out -Wl% $(WASM_OPT),$(WASM_FLAGS)) -DCD_WASM_OVERLAY
# bots-only 64 KiB shadow stack (measured high-water 13.2 KiB after M7a +
# M2-stream + ship-trim, see the Stack comment above). Trailing -z stack-size
# wins in wasm-ld, overriding the shared WASM_FLAGS' 256 KiB for bots only.
WASM_BOT_LDFLAGS := $(filter -Wl%,$(WASM_FLAGS)) -Wl,-z,stack-size=65536
WASM_BOT_O3_SRC  := src/cordite_sim.c src/cordite_strategy.c \
                    src/octogen_strategy.c

WASM_BOT_OBJ    := $(addprefix build/botobj/,$(notdir $(WASM_BOT_SRC:.c=.o)))
WASM_BOT_O3_OBJ := $(addprefix build/botobj/,$(notdir $(WASM_BOT_O3_SRC:.c=.o)))

# Default bot-object opt is -Oz; the MC-core objects override to -O3.
build/botobj/%.o: WASM_BOT_FILE_OPT := -Oz
$(WASM_BOT_O3_OBJ): WASM_BOT_FILE_OPT := -O3

build/botobj/%.o: src/%.c $(wildcard src/*.h) $(wildcard wasm/*.h)
	@mkdir -p build/botobj
	$(WASM_CC) $(WASM_BOT_CFLAGS) $(WASM_BOT_FILE_OPT) -c $< -o $@
build/botobj/%.o: wasm/%.c $(wildcard src/*.h) $(wildcard wasm/*.h)
	@mkdir -p build/botobj
	$(WASM_CC) $(WASM_BOT_CFLAGS) $(WASM_BOT_FILE_OPT) -c $< -o $@

build/bots.wasm: $(WASM_BOT_OBJ)
	@mkdir -p build
	$(WASM_CC) --target=wasm32 -nostdlib $(WASM_BOT_LDFLAGS) \
	          $(WASM_API_EXPORTS) $(WASM_BOTS_API_EXPORTS) $(WASM_BOT_OBJ) -o $@
	$(call wasm_postopt,$(WASM_BOTS_POSTOPT),$@)

# bots.wasm ships as a gzip STATIC ASSET (config.toml static_files + deploy
# --use-api, no Docker) — a real 47KB binary, not a 155KB base64 .ts embed.
# Loaded by _shared/wasm/wasm_asset.ts (server-only; bots never run in browser).
wasm-bots: build/bots.wasm
	gzip -9 -c build/bots.wasm > ../supabase/functions/_shared/wasm/bots.wasm.gz

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
COV_FILTERS    := --filter 'src/game\.c' --filter 'src/legal\.c' --filter 'src/replay\.c'

.PHONY: coverage
coverage:
	rm -rf covbuild && mkdir -p covbuild
	$(COV_CC) $(COV_FLAGS) $(CORE_SRC) tests/tests.c          -o covbuild/cnitro_tests    -lm
	$(COV_CC) $(COV_FLAGS) $(CORE_SRC) tests/sim_difftest.c   -o covbuild/sim_difftest    -lm
	$(COV_CC) $(COV_FLAGS) $(CORE_SRC) tests/apply_difftest.c -o covbuild/apply_difftest  -lm
	$(COV_CC) $(COV_FLAGS) $(CORE_SRC) tests/solver_difftest.c -o covbuild/solver_difftest -lm
	$(COV_CC) $(COV_FLAGS) $(CORE_SRC) tests/replay_difftest.c -o covbuild/replay_difftest -lm
	./covbuild/cnitro_tests
	./covbuild/sim_difftest 4 100
	./covbuild/apply_difftest 4 50
	./covbuild/solver_difftest 2 40 "" 5000
	./covbuild/solver_difftest 3 40 "" 5000
	./covbuild/solver_difftest 4 40 "" 5000
	./covbuild/replay_difftest 10
	gcovr --root . $(COV_FILTERS) --txt \
	      --fail-under-line $(COV_LINE_MIN) --fail-under-branch $(COV_BRANCH_MIN)

clean:
	rm -rf build covbuild
