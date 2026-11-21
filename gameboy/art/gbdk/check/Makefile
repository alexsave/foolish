# Builds the art check ROM from the png2asset output (run ../../export.py first).
GBDK ?= /Users/alex/Dev/gb-hello/toolchain/dl/gbdk
LCC = $(GBDK)/bin/lcc
SRC = check.c check_screens.c ../cards.c ../ui.c ../sprites.c \
      ../dashboard.c ../dashboard_new.c ../win_miami.c ../win_new_york.c ../win_seoul.c ../win_moscow.c ../lose.c ../lacquer.c ../paper.c
check.gb: $(SRC)
	$(LCC) -Wm-yn"DURAKART" -o $@ $(SRC)
clean:
	rm -f check.gb *.o *.map *.lst *.sym *.asm *.ihx *.noi
.PHONY: clean
