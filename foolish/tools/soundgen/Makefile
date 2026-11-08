# soundgen - every sound the game plays (soundgen.c says what they are and why
# nothing it writes is committed). docs/AUDIO.md is the user's guide.
#
#   make sounds    write every take into ../../public/audio/gen/, and the take
#                  table the web reads into ../../src/audio/gen/cueTakes.ts
#   make list      the files it writes, one a line (the cue-table test reads this)
#   make stats     the same, measured: peak and RMS
#   make parts     each model alone into build/parts/, for measure.py
#   make audition  play each one in turn, by name (macOS: afplay)
#
# -ffp-contract=off so a multiply and an add are never fused into one rounding:
# the same source then makes the same samples at -O0 and -O2 on one machine.
#
# NO -Werror, deliberately. `npm run build` runs this on whatever cc the lane
# has, and a warning a newer compiler invents must not be what stops the
# website building. Clean under Apple clang 21 and gcc 16 with these flags.

CC      ?= cc
CFLAGS  := -O2 -std=c11 -Wall -Wextra -ffp-contract=off -I. -I../../c/src -I../../../shared/c
OUT     := ../../public/audio/gen
TS      := ../../src/audio/gen/cueTakes.ts
SRC     := soundgen.c c/dsp.c c/models.c c/placeholders.c
HDR     := c/dsp.h c/models.h c/placeholders.h c/tables.h ../../c/src/anim_plan.h ../../../shared/c/rng.h

.PHONY: sounds list stats audition parts clean

build/soundgen: $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $@ -lm

# The directory is emptied first: a recipe that was removed (its cue now plays
# a recording) must not leave its old file behind to be served.
sounds: build/soundgen
	@rm -rf $(OUT)
	@mkdir -p $(OUT) $(dir $(TS))
	@./build/soundgen write $(OUT) $(TS)

list: build/soundgen
	@./build/soundgen list

stats: build/soundgen
	@./build/soundgen stats

# Each model on its own, 12 takes at 48 kHz: the C half of the comparison with
# the Python (docs/AUDIO.md, "The card sounds").
parts: build/soundgen
	@mkdir -p build/parts
	@./build/soundgen parts build/parts

audition: sounds
	@for f in $(OUT)/*.wav; do echo "$$(basename $$f)"; afplay "$$f"; sleep 0.35; done

clean:
	rm -rf build $(OUT) $(TS)
