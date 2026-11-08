# soundgen - every sound the game plays (soundgen.c says what they are and why
# nothing it writes is committed). docs/AUDIO.md is the user's guide.
#
#   make sounds    every take as AAC into ../../public/audio/gen/ (rendered to
#                  build/wav/, encoded by ffmpeg, checked), and the take table
#                  the web reads into ../../src/audio/gen/cueTakes.ts
#   make table     the take table alone: what type-checks and tests need, with
#                  no audio written and no ffmpeg
#   make ctable CTABLE=<path>
#                  the same table for a C host (iOS: c/src/cue_voice.h), no audio
#                  written and no ffmpeg; c/Makefile runs it for the iOS kernel
#   make bundle    `sounds`, but only when soundgen changed since the last one
#                  (the iOS build runs it on every build, ios/scripts/bundle_sounds.sh)
#   make check     hold every shipped file to its take (aac's priming declared,
#                  the length to the sample)
#   make list      the files it writes, one a line (the cue-table test reads this)
#   make stats     the same, measured: peak and RMS
#   make parts     each model alone into build/parts/, for measure.py
#   make audition  play each one in turn, by name (macOS: afplay)
#
# ffmpeg (its native AAC encoder) is needed by `sounds` only: macOS has it from
# Homebrew, Linux from its packages (the CI lanes that build the site install it).
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
WAV     := build/wav
SRC     := soundgen.c c/dsp.c c/models.c c/placeholders.c c/m4a.c
HDR     := c/dsp.h c/models.h c/placeholders.h c/m4a.h c/tables.h ../../c/src/anim_plan.h ../../../shared/c/rng.h

.PHONY: sounds table ctable bundle check list stats audition parts clean

build/soundgen: $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $@ -lm

# The directory is emptied first: a recipe that was removed (its cue now plays
# a recording) must not leave its old file behind to be served.
sounds: build/soundgen
	@rm -rf $(OUT) $(WAV)
	@mkdir -p $(OUT) $(WAV) $(dir $(TS))
	@./build/soundgen write $(WAV) $(TS)
	@./build/soundgen encode $(WAV) $(OUT)
	@./build/soundgen check $(WAV) $(OUT)

table: build/soundgen
	@mkdir -p $(dir $(TS))
	@./build/soundgen table $(TS)

ctable: build/soundgen
	@test -n "$(CTABLE)" || { echo "make ctable CTABLE=<path>" >&2; exit 2; }
	@mkdir -p $(dir $(CTABLE))
	@./build/soundgen ctable $(CTABLE)

# The stamp is written after a whole `sounds` succeeded, so a failed or
# interrupted run is redone next time rather than trusted.
bundle: build/sounds.stamp
build/sounds.stamp: build/soundgen
	@$(MAKE) -s sounds
	@touch $@

check: build/soundgen
	@./build/soundgen check $(WAV) $(OUT)

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
	@for f in $(OUT)/*.m4a; do echo "$$(basename $$f)"; afplay "$$f"; sleep 0.35; done

clean:
	rm -rf build $(OUT) $(TS)
