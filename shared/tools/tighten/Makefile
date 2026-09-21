# tighten: one C file and libc. Everything that touches video is ffmpeg, run as
# a child process, so this builds anywhere a compiler does and needs no headers
# from anywhere else in the tree.
CC      ?= clang
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter

build/tighten: tighten.c
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@

tighten: build/tighten
.PHONY: tighten

clean:
	rm -rf build
.PHONY: clean

.DEFAULT_GOAL := tighten
