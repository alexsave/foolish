# soundgen - the placeholder sounds (soundgen.c says what they are and why
# nothing it writes is committed). docs/AUDIO.md is the user's guide.
#
#   make sounds    write every placeholder into ../../public/audio/gen/
#   make list      the files it writes, one a line (the cue-table test reads this)
#   make stats     the same, measured: peak, RMS, spectral centroid
#   make audition  play each one in turn, by name (macOS: afplay)
#
# -ffp-contract=off so a multiply and an add are never fused into one rounding:
# the same source then makes the same samples at -O0 and -O2 on one machine.

CC      ?= cc
CFLAGS  := -O2 -std=c11 -Wall -Wextra -Werror -ffp-contract=off -I../../c/src
OUT     := ../../public/audio/gen

.PHONY: sounds list stats audition clean

build/soundgen: soundgen.c ../../c/src/anim_plan.h
	@mkdir -p build
	$(CC) $(CFLAGS) soundgen.c -o $@ -lm

# The directory is emptied first: a recipe that was removed (its cue now plays
# a recording) must not leave its old file behind to be served.
sounds: build/soundgen
	@rm -rf $(OUT)
	@mkdir -p $(OUT)
	@./build/soundgen write $(OUT)

list: build/soundgen
	@./build/soundgen list

stats: build/soundgen
	@./build/soundgen stats

audition: sounds
	@for f in $(OUT)/*.wav; do echo "$$(basename $$f)"; afplay "$$f"; sleep 0.35; done

clean:
	rm -rf build $(OUT)
