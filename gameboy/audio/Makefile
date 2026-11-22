# gameboy/audio - the sound design's tools (DESIGN.md). Nothing here builds the ROM.
#
#   make preview    every step below, then build/preview.wav
#   make web        the website's takes: soundgen (foolish/tools/soundgen, read only) into build/web_wav
#   make tables     snd_tab.c (committed, 60 Hz ticks) from the takes and jingles.txt; build/snd_tab241.c, the 241 Hz variant
#   make render     approach A (60 Hz, and 241 Hz) and B into build/gb_a, build/gb_a241, build/gb_b; build/context.wav
#   make compare    build/compare.tsv: each render against the real take it was fitted to
#
# Python is run with -I; numpy is the only dependency. The C is plain C11.

SOUNDGEN := ../../foolish/tools/soundgen
CC       ?= cc
CFLAGS   := -O2 -std=c11 -Wall -Wextra -I. -DSND_HOST
PY       := python3 -I

.PHONY: preview web tables render compare clean test tablecheck

build/soundgen: $(wildcard $(SOUNDGEN)/*.c $(SOUNDGEN)/c/*.c $(SOUNDGEN)/c/*.h $(SOUNDGEN)/tuning.tsv)
	@mkdir -p build
	$(CC) -O2 -std=c11 -ffp-contract=off -I$(SOUNDGEN) -I$(SOUNDGEN)/../../c/src -I$(SOUNDGEN)/../../../shared/c \
	  $(SOUNDGEN)/soundgen.c $(SOUNDGEN)/c/dsp.c $(SOUNDGEN)/c/models.c $(SOUNDGEN)/c/placeholders.c $(SOUNDGEN)/c/m4a.c -o $@ -lm

build/web.stamp: build/soundgen
	rm -rf build/web_wav && mkdir -p build/web_wav
	cd $(SOUNDGEN) && SOUNDGEN_JOBS=1 $(abspath build/soundgen) write $(abspath build/web_wav)
	cd $(SOUNDGEN) && $(abspath build/soundgen) ctable $(abspath build/cue_takes.c)
	touch $@
web: build/web.stamp

tables: build/web.stamp
	$(PY) tools/gen_tab.py build/web_wav build/cue_takes.c jingles.txt snd_tab.c build 60
	$(PY) tools/gen_tab.py build/web_wav build/cue_takes.c jingles.txt build/snd_tab241.c build/t241 241

build/render: host/render.c host/apu.c host/apu.h snd.c snd.h snd_tab.c
	$(CC) $(CFLAGS) host/render.c host/apu.c snd.c snd_tab.c -o $@ -lm
build/render241: host/render.c host/apu.c host/apu.h snd.c snd.h build/snd_tab241.c
	$(CC) $(CFLAGS) -DSND_TMA=239u -DSND_TICK_HZ=241u -DSND_STAGGER=11u host/render.c host/apu.c snd.c build/snd_tab241.c -o $@ -lm

render: build/render build/render241
	rm -rf build/gb_a build/gb_a241 build/gb_b && mkdir -p build/gb_a build/gb_a241 build/gb_b
	build/render a build/gb_a
	build/render241 a build/gb_a241
	build/render b build/pcm build/gb_b
	build/render context build/context.wav

compare:
	$(PY) tools/compare.py build > build/compare.tsv

preview: tables render compare
	$(PY) tools/preview.py build

clean:
	rm -rf build

# The driver's rules, on the Mac (host/snd_test.c), and each rule broken in a
# copy of snd.c: the test must fail on every one (tests/mutants.sh).
build/snd_test: host/snd_test.c host/scratch.h snd.c snd.h snd_tab.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Ihost -DSND_OVERLAY='"scratch.h"' host/snd_test.c snd.c snd_tab.c -o $@
test: build/snd_test
	build/snd_test
	sh tests/mutants.sh
	sh tests/snd_no_di.sh

# snd_tab.c must be what the generator makes of the takes and jingles.txt,
# byte for byte (it is committed; this regenerates it into build/ and compares)
tablecheck: build/web.stamp
	$(PY) tools/gen_tab.py build/web_wav build/cue_takes.c jingles.txt build/snd_tab_check.c build/check 60 > /dev/null
	cmp build/snd_tab_check.c snd_tab.c && echo "tablecheck: snd_tab.c regenerates byte for byte"
