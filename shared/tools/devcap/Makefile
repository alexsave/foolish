# devcap - film a USB iPhone's screen for the motion tool (devcap.m).
CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra -Werror -fobjc-arc
.PHONY: all clean
all: build/devcap
build:
	mkdir -p build
build/devcap: devcap.m | build
	$(CC) $(CFLAGS) devcap.m -o $@ -framework Foundation -framework AVFoundation -framework CoreMediaIO -framework CoreMedia
clean:
	rm -rf build
