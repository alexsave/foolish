# The ruler finder and the ride scorer (motion.h). Product-free; the palette is
# shared/c/motion_ruler/motion_ruler.h, which the Swift ruler paints from.
CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
INC := -I../../c/motion_ruler
.PHONY: all test clean
all: build/motion
build:
	mkdir -p build
build/motion: motion.c motion_main.c motion.h ../../c/motion_ruler/motion_ruler.h | build
	$(CC) $(CFLAGS) $(INC) motion.c motion_main.c -o $@ -lm
build/motion_test: motion.c motion_test.c motion.h ../../c/motion_ruler/motion_ruler.h | build
	$(CC) $(CFLAGS) $(INC) motion.c motion_test.c -o $@ -lm
test: build/motion_test
	./build/motion_test
clean:
	rm -rf build
