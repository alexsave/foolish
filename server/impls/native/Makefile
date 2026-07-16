# The native (dedicated-process) Foolish server. Links the C KERNEL directly —
# same c/src/*.c the wasm and iOS builds use — so the game logic is shared and
# this backend only adds a socket + an in-memory store. See README.md.

KDIR   := ../../../c
CFLAGS ?= -O2 -ffast-math -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
          -I$(KDIR)/src -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK
LDFLAGS ?= -lm -lpthread -framework Accelerate

# Every kernel source except the standalone tools (their own main()).
KERNEL_SRC := $(filter-out $(wildcard $(KDIR)/src/main_*.c), $(wildcard $(KDIR)/src/*.c))

foolish_server: foolish_server.c $(KERNEL_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: foolish_server
	./foolish_server 8099

clean:
	rm -f foolish_server

.PHONY: run clean
