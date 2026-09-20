CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -std=c11
LDLIBS  := -lm
SRC     := src/uttt.c src/uttt_code.c src/uttt_bots.c \
           src/uttt_pen.c src/uttt_draw.c

all: build/uttt_test build/uttt_arena build/uttt_render

build:
	@mkdir -p build

build/uttt_test: $(SRC) tests/uttt_test.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_test.c -o $@ $(LDLIBS)

build/uttt_arena: $(SRC) tests/uttt_arena.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_arena.c -o $@ $(LDLIBS)

run: build/uttt_test
	./build/uttt_test 1000

arena: build/uttt_arena
	./build/uttt_arena 200 200

build/uttt_render: $(SRC) tests/uttt_render.c | build
	$(CC) $(CFLAGS) $(SRC) tests/uttt_render.c -o $@ $(LDLIBS)

render: build/uttt_render
	./build/uttt_render $(PLIES)
	@sips -s format png build/board.ppm --out build/board.png >/dev/null \
	  && echo "build/board.png"


asan: $(SRC) tests/uttt_test.c | build
	$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -std=c11 \
	  $(SRC) tests/uttt_test.c -o build/uttt_asan $(LDLIBS)
	./build/uttt_asan 400

clean:
	rm -rf build

.PHONY: all run arena render asan clean
