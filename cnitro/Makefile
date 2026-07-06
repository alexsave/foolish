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
CORE_SRC := src/game.c src/legal.c src/random_strategy.c src/espresso_strategy.c \
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

.PHONY: all clean tests
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
difftests: build/sim_difftest build/apply_difftest build/solver_difftest
	./build/sim_difftest 4 1000 && ./build/apply_difftest 4 200 && \
	./build/solver_difftest 2 40 "" 200000 && \
	./build/solver_difftest 3 40 "" 200000 && \
	./build/solver_difftest 4 40 "" 200000

# ---------------------------------------------------------------------------
# WebAssembly rules kernel: game.c + legal.c + the wasm bridge, compiled
# freestanding (no libc; wasm/include has the tiny shims, wasm_api.c provides
# memcpy/memset). This is THE production rules engine: the TS server imports
# the resulting module via supabase/functions/_shared/wasm/. `-D_Thread_local=`
# strips TLS (single-threaded module); the bridge sets the production
# deck-size boundary at init (see card.h).
WASM_CC    ?= clang
# -mbulk-memory: memcpy/memset (incl. clang-lowered struct copies) become the
# wasm memory.copy/memory.fill instructions, which every 2020+ runtime (V8,
# JSC, SpiderMonkey, Deno edge) executes as native memmove — the hand-rolled
# word-loop shim showed up at ~5% of cordite's wasm profile.
# -O3 (but NO -flto: wasm-ld LTO corrupts the indirect-function table for
# the address-taken hook/strategy pointers — engine_snap_hook calls trap with
# "table index out of bounds"; caught by the fuzz suite). No -ffast-math
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
WASM_FLAGS := --target=wasm32 -O3 -nostdlib -ffreestanding -mbulk-memory \
              -isystem wasm/include -Isrc -D_Thread_local= -DMAX_LOG_PAIRS=64 \
              -DMAX_LEGAL_MOVES=4096 -DMAX_MOVE_CARDS=28 -DMAX_BATTLES=64 \
              -DWORLD_LOG_CAP=40 \
              -Wall -Wextra -Wno-unused-parameter \
              -Wl,--no-entry -Wl,--export-all -Wl,-z,stack-size=262144 -Wl,--stack-first

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
# Stack 128KB: no solver, no rollouts — the deepest rules path is the cover
# enumeration (~45KB measured worst); 128KB is ~3x that. The trailing
# -z stack-size overrides the shared flags' 256KB (last one wins in wasm-ld).
WASM_RULES_FLAGS := $(filter-out -DMAX_LEGAL_MOVES=4096,$(WASM_FLAGS)) \
                    -DMAX_LEGAL_MOVES=1024 -DMAX_LOGS=128 -Wl,-z,stack-size=131072

build/rules.wasm: src/game.c src/legal.c wasm/wasm_api.c src/game.h src/legal.h src/card.h
	@mkdir -p build
	$(WASM_CC) $(WASM_RULES_FLAGS) src/game.c src/legal.c wasm/wasm_api.c -o $@

# Regenerates the base64 module the TS side imports (works unchanged in
# Deno edge functions, Node and browsers — no asset pipeline needed).
wasm: build/rules.wasm
	node wasm/embed.mjs build/rules.wasm ../supabase/functions/_shared/wasm/rules_wasm.ts

# Bot module: the rules kernel PLUS every algorithmic bot strategy and the
# choose-move bridge. A superset of rules.wasm, loaded only where bots run
# (edge functions' bot turns, offline harnesses) so the pure rules path keeps
# the smaller module.
# Note: the distilled strategy (research, strength-negative — see
# CORDITE_RESEARCH.md) stays native-arena-only; it is deliberately NOT in the
# production bot module.
WASM_BOT_SRC := src/game.c src/legal.c src/random_strategy.c \
                src/espresso_strategy.c src/handwritten_strategy.c \
                src/cordite_strategy.c src/cordite_sim.c \
                src/simple_heuristic_strategy.c src/champion_strategy.c \
                src/ultimate_champion_strategy.c src/hacker_strategy.c \
                src/fulminate_strategy.c src/espresso_prod_strategy.c \
                src/handwritten_prod_strategy.c src/semtex_strategy.c \
                src/octogen_strategy.c wasm/wasm_api.c wasm/wasm_bots_api.c

WASM_BOT_FLAGS := $(WASM_FLAGS)

build/bots.wasm: $(WASM_BOT_SRC) src/game.h src/legal.h src/card.h src/strategy.h
	@mkdir -p build
	$(WASM_CC) $(WASM_BOT_FLAGS) $(WASM_BOT_SRC) -o $@

wasm-bots: build/bots.wasm
	node wasm/embed.mjs build/bots.wasm ../supabase/functions/_shared/wasm/bots_wasm.ts BOTS_WASM_B64

clean:
	rm -rf build
