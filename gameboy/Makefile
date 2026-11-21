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

CORE     = src/gd_rules.c src/gd_play.c src/gd_rng.c
CORE_H   = src/gd_rules.h src/gd_play.h src/gd_rng.h $(B)/gd_binom.h
ROM_SRC  = src/main.c src/gfx.c src/input.c $(CORE)
ROM_H    = src/gfx.h src/input.h $(CORE_H) $(wildcard art/gbdk/*.h)
ART      = art/gbdk
# All the art (the table's tile banks, 2.3 KB, and the four full-screen
# pictures, 9.5 KB) goes to ROM bank 2 of an MBC1 cart (gfx.c ART_BANK): with
# it the ROM does not fit the 32 KB ROM-only cart, and the code alone nearly
# fills the 32 KB the CPU sees without banking. _HOME (the library's
# interrupt-safe routines and gfx.c's bank loader) is placed first, in bank 0,
# because the loader maps bank 2 over 0x4000-0x7FFF while it runs; the check
# script fails the build if _HOME ever lands past 0x3FFF.
ART_TABLE = $(ART)/cards.c $(ART)/ui.c $(ART)/sprites.c
ART_FULL  = $(ART)/dashboard.c $(ART)/dashboard_new.c $(ART)/win.c $(ART)/lose.c
ROM_OBJ   = $(patsubst %.c,$(B)/rom/%.o,$(notdir $(ROM_SRC) $(ART_TABLE))) \
            $(patsubst %.c,$(B)/rom/%.o,$(notdir $(ART_FULL)))

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

$(B)/rom:
	mkdir -p $@

$(B)/rom/%.o: src/%.c $(ROM_H) $(B)/gd_binom.h | $(B)/rom
	$(LCC) $(LCCFLAGS) -I$(B) -I$(ART) -c -o $@ $<

$(B)/rom/%.o: $(ART)/%.c Makefile | $(B)/rom
	$(LCC) $(LCCFLAGS) -Wf-bo2 -c -o $@ $<

$(B)/durak.gb: $(ROM_OBJ)
	$(LCC) -Wl-m -Wl-j -Wl-b_HOME=0x0200 -Wl-b_CODE=0x0800 -Wm-yt0x01 -Wm-yo4 -Wm-yn"DURAK" -o $@ $(ROM_OBJ) 2>&1 \
	    | tee $(B)/link.log
	@! grep -i overflow $(B)/link.log || { rm -f $@; echo "the code overflowed its ROM area"; exit 1; }
	sh tests/home_in_bank0.sh $(B)/durak.map

romusage: $(B)/durak.gb
	$(GBDK)/bin/romusage $(B)/durak.map -g

# ---- the differential test -------------------------------------------------------
$(B)/difftest: tests/difftest.c $(CORE) $(CORE_H) | $(B)
	$(HOSTCC) $(HOSTFLAGS) $(FOOLISH_DEFS) -I$(B) -I$(FOOLISH) -o $@ tests/difftest.c $(CORE) $(FOOLISH_SRC)

test: $(B)/difftest
	sh tests/rng_ban.sh
	$(B)/difftest 3000 1

mutants: $(B)/gd_binom.h
	sh tests/mutants.sh

# ---- the headless runner ------------------------------------------------------------
$(B)/gbrun: tools/gbrun.c | $(B)
	$(HOSTCC) $(HOSTFLAGS) -DUSE_PNG -I/opt/homebrew/include -o $@ $< -L/opt/homebrew/lib -lmgba -lpng

gbrun: $(B)/gbrun

clean:
	rm -rf $(B)

.PHONY: all test mutants gbrun romusage clean

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
