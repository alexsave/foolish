CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -std=c11
LDLIBS  := -lm
SRC     := src/uttt.c src/uttt_code.c

all: build/uttt_test

build:
	@mkdir -p build

build/uttt_test: $(SRC) tests/uttt_test.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_test.c -o $@ $(LDLIBS)

run: build/uttt_test
	./build/uttt_test 1000

asan: $(SRC) tests/uttt_test.c | build
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(SRC) tests/uttt_test.c -o build/uttt_asan $(LDLIBS)
	./build/uttt_asan 400

clean:
	rm -rf build

.PHONY: all run asan clean
