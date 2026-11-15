CC = clang
CFLAGS = -O2 -Wall -Wextra -std=c11 -D_DARWIN_C_SOURCE
LIB = rng.c string_params.c string.c finger.c bow.c control.c sim.c fft.c analysis.c
LIBO = $(LIB:.c=.o)
TESTS = test_rng test_string_free test_helmholtz test_schelleng test_finger test_determinism

all: altcello

%.o: %.c $(wildcard *.h)
	nice $(CC) $(CFLAGS) -c $< -o $@

altcello: altcello.o $(LIBO)
	nice $(CC) $(CFLAGS) $^ -o $@ -lm

test_%: test_%.o $(LIBO)
	nice $(CC) $(CFLAGS) $^ -o $@ -lm

test: $(TESTS)
	@mkdir -p out
	./test_rng_ban.sh
	@for t in $(TESTS); do echo "== $$t"; nice ./$$t || exit 1; done
	@echo "ALL TESTS PASSED"

clean:
	rm -f *.o altcello $(TESTS)
	rm -rf out
