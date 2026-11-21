# Game Boy Durak. `make` builds the ROM, `make test` runs the differential test
# against foolish's engine and the RNG ban, `make gbrun` builds the headless
# runner. Nothing in ../foolish is written: its engine is compiled into build/.
GBDK    ?= /Users/alex/Dev/gb-hello/toolchain/dl/gbdk
LCC      = $(GBDK)/bin/lcc
# SDCC 4.5 (SM83) miscompiled a variable-shift bit test, `(byte >> (c & 7)) & 1`
# through a mask pointer: it shifted the bit index instead of the byte it had
# addressed, so a card in hand read as absent (README.md "Quirks"). The bit tests
# go through a table now (gd_rules.h GD_HAS), and `make selfplay` - clang and
# SDCC must play identical games - is the check that keeps codegen honest.
LCCFLAGS =
HOSTCC  ?= clang
B        = build
FOOLISH  = ../foolish/c/src
SHARED   = ../shared/c

CORE     = src/gd_rules.c src/gd_menu.c src/gd_play.c src/gd_rng.c src/gd_botlib.c src/gd_bot_hw.c src/gd_sim.c
CORE_H   = src/gd_bank.h src/gd_botlib.h src/gd_sim.h $(B)/gd_simtab.h src/gd_rules.h src/gd_play.h src/gd_rng.h src/gd_bot_hw.h $(B)/gd_binom.h
# Link order matters: the fixed bank 0 fills first (the rules core, the RNG,
# the input interrupt, then GBDK's library); each other file names its bank.
ROM_SRC  = src/gd_rules.c src/gd_rng.c src/gd_botlib.c src/input.c src/main.c src/gfx.c src/gd_play.c src/gd_menu.c src/gd_bot_hw.c src/gd_sim.c src/gd_bot_seoul.c
ROM_BANK1 = main gfx gd_play
ROM_BANK4 = gd_bot_hw
ROM_BANK5 = gd_menu
ROM_BANK6 = gd_sim
ROM_BANK7 = gd_bot_seoul
ROM_H    = src/gfx.h src/input.h $(CORE_H) src/gd_bot_seoul.h $(wildcard art/gbdk/*.h)
ART      = art/gbdk
# All the art (the table's tile banks, 2.3 KB, and the four full-screen
# pictures, 9.5 KB) goes to ROM bank 2 of an MBC1 cart (gfx.c ART_BANK): with
# it the ROM does not fit the 32 KB ROM-only cart, and the code alone nearly
# fills the 32 KB the CPU sees without banking. _HOME (the library's
# interrupt-safe routines and gfx.c's bank loader) is placed first, in bank 0,
# because the loader maps bank 2 over 0x4000-0x7FFF while it runs; the check
# script fails the build if _HOME ever lands past 0x3FFF.
# Bank 2: the table's banks, the fonts and small sprites, the dashboards and
# the lose picture; bank 3: the four win pictures. gfx.c reads each file's bank
# with BANK(), so this list is the one place that decides it.
ART_BANK2 = cards ui sprites lacquer paper plane fireworks dashboard dashboard_new lose
ART_BANK3 = win_miami win_new_york win_seoul win_moscow
ROM_OBJ   = $(patsubst %.c,$(B)/rom/%.o,$(notdir $(ROM_SRC))) \
            $(patsubst %,$(B)/rom/%.o,$(ART_BANK2) $(ART_BANK3))

HOSTFLAGS = -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
# foolish's production caps (its README): a menu as wide as the shipped one.
FOOLISH_DEFS = -DMAX_LEGAL_MOVES=65536 -DMAX_MOVE_CARDS=40 -DMAX_LOGS=64
FOOLISH_SRC  = $(FOOLISH)/game.c $(FOOLISH)/legal.c $(FOOLISH)/card.c $(SHARED)/deal_rng.c

all: $(B)/durak.gb

$(B):
	mkdir -p $(B)

$(B)/gen_binom: tools/gen_binom.c | $(B)
	$(HOSTCC) -O2 -o $@ $<

$(B)/gd_binom.h: $(B)/gen_binom
	$(B)/gen_binom > $@

$(B)/gen_simtab: tools/gen_simtab.c | $(B)
	$(HOSTCC) -O2 -o $@ $<

$(B)/gd_simtab.h: $(B)/gen_simtab
	$(B)/gen_simtab > $@

$(B)/rom:
	mkdir -p $@

$(B)/rom/%.o: src/%.c $(ROM_H) $(B)/gd_binom.h Makefile | $(B)/rom
	$(LCC) $(LCCFLAGS) -DGD_BANKED_BUILD $(call bank_of,$*) -I$(B) -I$(ART) -c -o $@ $<

bank_of = $(if $(filter $1,$(ROM_BANK1)),-Wf-bo1,$(if $(filter $1,$(ROM_BANK4)),-Wf-bo4,$(if $(filter $1,$(ROM_BANK5)),-Wf-bo5,$(if $(filter $1,$(ROM_BANK6)),-Wf-bo6,$(if $(filter $1,$(ROM_BANK7)),-Wf-bo7)))))

$(B)/rom/%.o: $(ART)/%.c Makefile | $(B)/rom
	$(LCC) $(LCCFLAGS) -Wf-bo$(if $(filter $*,$(ART_BANK3)),3,2) -c -o $@ $<

$(B)/durak.gb: $(ROM_OBJ)
	$(LCC) -Wl-m -Wl-j -Wl-b_HOME=0x0200 -Wl-b_CODE=0x0800 -Wm-yt0x01 -Wm-yo8 -Wm-yn"DURAK" -o $@ $(ROM_OBJ) 2>&1 \
	    | tee $(B)/link.log
	@! grep -i overflow $(B)/link.log || { rm -f $@; echo "the code overflowed its ROM area"; exit 1; }
	sh tests/home_in_bank0.sh $(B)/durak.map

romusage: $(B)/durak.gb
	$(GBDK)/bin/romusage $(B)/durak.map -g

# ---- the differential test -------------------------------------------------------
# foolish's handwritten_prod bot, with its random draws handed over by the test
# (difftest.c dt_random): the same draws New York made.
$(B)/hw_prod.o: $(FOOLISH)/handwritten_prod_strategy.c | $(B)
	$(HOSTCC) $(HOSTFLAGS) -w $(FOOLISH_DEFS) -Drandom_strategy_random=dt_random -I$(FOOLISH) -c -o $@ $<

$(B)/difftest: tests/difftest.c $(CORE) $(CORE_H) $(B)/hw_prod.o | $(B)
	$(HOSTCC) $(HOSTFLAGS) $(FOOLISH_DEFS) -I$(B) -I$(FOOLISH) -o $@ tests/difftest.c $(CORE) $(FOOLISH_SRC) $(B)/hw_prod.o

# The rollout engine against foolish's cordite_sim, with foolish's default
# caps (its sim refuses the production menu cap) and its draws handed over.
SIM_FOOLISH = $(FOOLISH)/game.c $(FOOLISH)/legal.c $(FOOLISH)/card.c $(SHARED)/deal_rng.c
$(B)/cordite_sim.o: $(FOOLISH)/cordite_sim.c | $(B)
	$(HOSTCC) $(HOSTFLAGS) -w -DMAX_LOGS=64 -Dgame_random=dt_sim_random -I$(FOOLISH) -c -o $@ $<

$(B)/simtest: tests/simtest.c tests/foolish_map.h $(CORE) $(CORE_H) $(B)/cordite_sim.o | $(B)
	$(HOSTCC) $(HOSTFLAGS) -DMAX_LOGS=64 -I$(B) -I$(FOOLISH) -o $@ tests/simtest.c src/gd_rules.c src/gd_menu.c src/gd_rng.c src/gd_botlib.c src/gd_sim.c $(SIM_FOOLISH) $(B)/cordite_sim.o

test: $(B)/difftest $(B)/simtest
	sh tests/rng_ban.sh
	$(B)/difftest 3000 1
	$(B)/simtest 20000 1

print-core:
	@echo $(CORE)

mutants: $(B)/gd_binom.h $(B)/gd_simtab.h $(B)/hw_prod.o $(B)/cordite_sim.o
	bash tests/mutants.sh

rom-mutants: $(B)/durak.gb $(B)/gbrun
	bash tests/rom_mutants.sh

# ---- the headless runner ------------------------------------------------------------
$(B)/gbrun: tools/gbrun.c | $(B)
	$(HOSTCC) $(HOSTFLAGS) -DUSE_PNG -I/opt/homebrew/include -o $@ $< -L/opt/homebrew/lib -lmgba -lpng

gbrun: $(B)/gbrun

clean:
	rm -rf $(B)

.PHONY: print-core all test mutants rom-mutants gbrun romusage clean

# ---- the cross-compiler check: clang and SDCC must play the same games --------------
$(B)/selfplay_host: tests/selfplay.c tests/selfplay_host.c tests/selfplay.h $(CORE) $(CORE_H) | $(B)
	$(HOSTCC) $(HOSTFLAGS) -I$(B) -o $@ tests/selfplay.c tests/selfplay_host.c $(CORE)

$(B)/selfplay.gb: tests/selfplay.c tests/selfplay_rom.c tests/selfplay.h $(CORE) $(CORE_H) | $(B)
	$(LCC) $(LCCFLAGS) -Wl-j -I$(B) -o $@ tests/selfplay.c tests/selfplay_rom.c $(CORE)

selfplay: $(B)/selfplay_host $(B)/selfplay.gb $(B)/gbrun
	sh tests/selfplay.sh

scripts: $(B)/durak.gb $(B)/gbrun
	sh tests/scripts.sh

.PHONY: selfplay scripts

# ---- one random playout, timed on the Game Boy (Seoul and Moscow are sized from it) ----
$(B)/playout_host: tests/playout.c tests/playout_host.c tests/playout.h $(CORE) $(CORE_H) | $(B)
	$(HOSTCC) $(HOSTFLAGS) -I$(B) -o $@ tests/playout.c tests/playout_host.c src/gd_rules.c src/gd_menu.c src/gd_rng.c

$(B)/playout.gb: tests/playout.c tests/playout_rom.c tests/playout.h $(CORE) $(CORE_H) | $(B)
	$(LCC) $(LCCFLAGS) -Wl-j -I$(B) -o $@ tests/playout_rom.c src/gd_rules.c src/gd_menu.c src/gd_rng.c

playout: $(B)/playout_host $(B)/playout.gb $(B)/gbrun
	sh tests/playout.sh

.PHONY: playout

# ---- the rollout engine timed on the Game Boy -----------------------------------------
SIMBENCH_DEPS = tests/simbench.c src/gd_sim.c $(CORE_H)
$(B)/simbench_host: $(SIMBENCH_DEPS) | $(B)
	$(HOSTCC) $(HOSTFLAGS) -DSIMBENCH_HOST -I$(B) -o $@ tests/simbench.c src/gd_rules.c src/gd_rng.c src/gd_botlib.c

$(B)/simbench.gb: $(SIMBENCH_DEPS) | $(B)
	$(LCC) $(LCCFLAGS) -Wl-j -I$(B) -o $@ tests/simbench.c src/gd_rules.c src/gd_rng.c src/gd_botlib.c

$(B)/simprof.gb: $(SIMBENCH_DEPS) | $(B)
	$(LCC) $(LCCFLAGS) -Wl-j -DSIM_PROF -I$(B) -o $@ tests/simbench.c src/gd_rules.c src/gd_rng.c src/gd_botlib.c

simbench: $(B)/simbench_host $(B)/simbench.gb $(B)/simprof.gb $(B)/gbrun
	sh tests/simbench.sh

.PHONY: simbench

# ---- bots against each other on the Mac (Seoul sized by SEOUL_SAMPLES) -----------------
MATCH_SRC = tests/match.c src/gd_rules.c src/gd_menu.c src/gd_rng.c src/gd_botlib.c src/gd_bot_hw.c src/gd_sim.c src/gd_bot_seoul.c
$(B)/match: $(MATCH_SRC) $(CORE_H) src/gd_bot_seoul.h | $(B)
	$(HOSTCC) $(HOSTFLAGS) -I$(B) -o $@ $(MATCH_SRC)

# ---- Seoul's decisions timed on the Game Boy, by section ------------------------------
# The game ROM again, with Seoul's section marks calling a bank-0 clock
# (tests/gb_prof.c, in this build only); scripts/game_seoul.txt plays it.
$(B)/prof:
	mkdir -p $@

$(B)/prof/gb_prof.o: tests/gb_prof.c | $(B)/prof
	$(LCC) $(LCCFLAGS) -c -o $@ $<

$(B)/prof/gd_bot_seoul.o: src/gd_bot_seoul.c $(ROM_H) | $(B)/prof
	$(LCC) $(LCCFLAGS) -DGD_BANKED_BUILD -DSEOUL_PROFILE -Wf-bo7 -I$(B) -I$(ART) -c -o $@ $<

PROF_OBJ = $(B)/prof/gb_prof.o $(filter-out $(B)/rom/gd_bot_seoul.o,$(ROM_OBJ)) $(B)/prof/gd_bot_seoul.o
$(B)/durak_prof.gb: $(PROF_OBJ)
	$(LCC) -Wl-m -Wl-j -Wl-b_HOME=0x0200 -Wl-b_CODE=0x0800 -Wm-yt0x01 -Wm-yo8 -Wm-yn"DURAK" -o $@ $(PROF_OBJ)

# ---- Seoul's cross-compiler check (a banked ROM, laid out like the game) --------------
$(B)/seoulcheck_host: tests/seoulcheck.c $(MATCH_SRC) $(CORE_H) | $(B)
	$(HOSTCC) $(HOSTFLAGS) -DSEOULCHECK_HOST -I$(B) -o $@ tests/seoulcheck.c src/gd_rules.c src/gd_menu.c src/gd_rng.c src/gd_botlib.c src/gd_bot_hw.c src/gd_sim.c src/gd_bot_seoul.c

$(B)/rom/seoulcheck.o: tests/seoulcheck.c $(CORE_H) src/gd_bot_seoul.h | $(B)/rom
	$(LCC) $(LCCFLAGS) -DGD_BANKED_BUILD -I$(B) -c -o $@ $<

SC_OBJ = $(B)/rom/gd_rules.o $(B)/rom/gd_rng.o $(B)/rom/gd_botlib.o $(B)/rom/seoulcheck.o \
         $(B)/rom/gd_menu.o $(B)/rom/gd_bot_hw.o $(B)/rom/gd_sim.o $(B)/rom/gd_bot_seoul.o
$(B)/seoulcheck.gb: $(SC_OBJ)
	$(LCC) -Wl-m -Wl-j -Wl-b_HOME=0x0200 -Wl-b_CODE=0x0800 -Wm-yt0x01 -Wm-yo8 -o $@ $(SC_OBJ)

seoulcheck: $(B)/seoulcheck_host $(B)/seoulcheck.gb $(B)/gbrun
	sh tests/seoulcheck.sh

.PHONY: seoulcheck
