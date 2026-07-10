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
# WORLD_LOG_CAP=40: sampled-world/trial/difftest Game slots keep a 40-entry
# discard-only log instead of the full 512 (see cordite_sim.h) — the research
# full-log world modes stay native-only. 3 x 61KB per bots.wasm instance.
# Stack 256KB: with every sizeof(Game)/sizeof(LegalMoves) local moved into
# shared scratch, the deepest real path is the bitboard solver's 48-frame
# recursion (~1.5KB/frame) — 256KB is ~3x that worst case. --stack-first
# keeps any regression a loud trap, and CI's mem job runs unpinned games.
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
#   11 too. Measured worst 12 over 63K games. 24 is ~1.8x the analytic
#   ceiling — snap-ring overflow silently DROPS animation frames (visual, not
#   corruption), so this margin is deliberate. Cuts g_snaps 55.7KB->27.8KB.
WASM_FLAGS := --target=wasm32 $(WASM_OPT) -nostdlib -ffreestanding -mbulk-memory \
              -isystem wasm/include -Isrc -D_Thread_local= -DMAX_LOG_PAIRS=64 \
              -DMAX_LEGAL_MOVES=4096 -DMAX_MOVE_CARDS=28 -DMAX_BATTLES=64 \
              -DWORLD_LOG_CAP=40 \
              -DREPLAY_REC_CAP=4096 -DWASM_REPLAY_IO_CAP=32768 -DMAX_SNAPS=24 \
              -DCD_TT_BITS=13 \
              -Wall -Wextra -Wno-unused-parameter \
              -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=262144 -Wl,--stack-first \
              -Wl,--strip-all
# CD_TT_BITS=13: the cordite endgame solver's transposition table (bump-
# allocated on first play) is the largest driver of bots.wasm's RUNTIME peak —
# it grows, unlike the static caps above. 16 = 1 MiB; 13 = 128 KiB (8,192
# entries). It's a memoization cache, but the solver is node-budget-limited, so
# table size is a bot-STRENGTH knob, sized by the per-game move-divergence study
# (tools/tt_divergence.sh, docs/WASM_L1_BUDGET.md). There is no cliff-to-zero: the
# binding bot octogen (persists the table per game) has an INHERENT ~4e-4 floor of
# size-sensitivity that exists even at TT16-TT19; the collision knee is ~TT10-11.
# So TT16 (production today) is the baseline, not an infinite table — and TT13 vs
# TT16 diverges in 0/1720 games (octogen, pc 2/4/7). TT13 is indistinguishable
# from the shipped table, 2-3 bits above the knee. Drops runtime peak 36 -> 22
# pages (2.25 -> 1.375 MiB). Only bots links cordite_sim, so rules/guards ignore it.

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
  -Wl,--export=wasm_rearrange_hand

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
# Stack 64KB: no solver, no rollouts — the deepest rules path is the cover
# enumeration (~45KB measured worst); 64KB is 1.4x that, and --stack-first
# turns any regression into a loud trap that the fuzz + cover e2e suites
# exercise directly (they drive the enumeration harder than production
# does). The trailing -z stack-size overrides the shared flags' 256KB (last
# one wins in wasm-ld).
# WASM_IO_CAP=24576: this build's widest unchunked io write is the log
# export at MAX_LOGS=128 — 2 + 128 x (4 + 64x2) = 16,898B; 24KB is 1.45x
# that (the 72KB default exists for bots' 512-log session export).
# Together with the shared L1 caps above, rules.wasm linear memory drops
# 53 pages (3.31MB) -> 5 pages (320KB).
WASM_RULES_FLAGS := $(filter-out -DMAX_LEGAL_MOVES=4096,$(WASM_FLAGS)) \
                    -DMAX_LEGAL_MOVES=1024 -DMAX_LOGS=128 -DWASM_IO_CAP=24576 \
                    -Wl,-z,stack-size=65536

# Blanket header dependency, same rationale as the native objects: a header
# edit must rebuild BOTH wasm modules or they ship divergent kernels.
WASM_RULES_SRC := src/game.c src/deal_rng.c src/legal.c src/replay.c src/view.c src/awire.c \
                  src/evwire.c wasm/wasm_api.c

build/rules.wasm: $(WASM_RULES_SRC) $(wildcard src/*.h) $(wildcard wasm/*.h)
	@mkdir -p build
	$(WASM_CC) $(WASM_RULES_FLAGS) $(WASM_API_EXPORTS) $(WASM_RULES_SRC) -o $@

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

wasm-guards: build/guards.wasm
	node wasm/embed.mjs build/guards.wasm ../supabase/functions/_shared/wasm/guards_wasm.ts GUARDS_WASM_B64 --gzip

# Bot module: the rules kernel PLUS every algorithmic bot strategy and the
# choose-move bridge. A superset of rules.wasm, loaded only where bots run
# (edge functions' bot turns, offline harnesses) so the pure rules path keeps
# the smaller module.
# Note: the distilled strategy (research, strength-negative — see
# CORDITE_RESEARCH.md) stays native-arena-only; it is deliberately NOT in the
# production bot module.
WASM_BOT_SRC := src/game.c src/deal_rng.c src/legal.c src/replay.c src/view.c src/awire.c \
                src/evwire.c src/random_strategy.c \
                src/espresso_strategy.c src/handwritten_strategy.c \
                src/cordite_strategy.c src/cordite_sim.c \
                src/simple_heuristic_strategy.c src/champion_strategy.c \
                src/ultimate_champion_strategy.c src/hacker_strategy.c \
                src/fulminate_strategy.c src/espresso_prod_strategy.c \
                src/handwritten_prod_strategy.c src/semtex_strategy.c \
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
WASM_BOT_CFLAGS  := $(filter-out -Wl% $(WASM_OPT),$(WASM_FLAGS))
WASM_BOT_LDFLAGS := $(filter -Wl%,$(WASM_FLAGS))
WASM_BOT_O3_SRC  := src/cordite_sim.c src/cordite_strategy.c \
                    src/octogen_strategy.c src/semtex_strategy.c src/fulminate_strategy.c

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
