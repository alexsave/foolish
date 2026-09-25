# rs_scan - the byte scanner behind shared/tools/release_strings.sh.
CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror
.PHONY: all test clean
all: build/rs_scan
build:
	mkdir -p build
build/rs_scan: rs_scan.c | build
	$(CC) $(CFLAGS) rs_scan.c -o $@
test: build/rs_scan
	bash test.sh
clean:
	rm -rf build
