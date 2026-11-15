CC = clang
CFLAGS = -O2 -Wall -Wextra -std=c11 -ffp-contract=off -D_DARWIN_C_SOURCE
LIB = rng.c geom.c room.c room_cli.c instrument.c instrument_cello.c torsion.c altstring.c finger.c bow.c control.c radiate_body.c radiate_plate_table.c body_modes.c body.c wav.c sim.c fft.c analysis.c
LIBO = $(LIB:.c=.o)
TESTS = test_rng test_wav test_string_free test_helmholtz test_schelleng test_finger test_determinism \
	test_body_energy test_admittance test_bridge_loss test_sympathetic test_body_helmholtz test_wolf test_torsion test_radiate test_bridge_hill test_hair test_instrument test_knobs test_room

all: altcello geomdump test_geom bowhair_study

bowhair_study: bowhair_study.o $(LIBO)
	nice $(CC) $(CFLAGS) $^ -o $@ -lm

# geometry tool and its test (geom.c is standalone, no altcello objects)
geomdump: geomdump.c geom.c geom.h
	nice $(CC) $(CFLAGS) -o $@ geomdump.c geom.c -lm

test_geom: test_geom.c geom.c geom.h
	nice $(CC) $(CFLAGS) -o $@ test_geom.c geom.c -lm

%.o: %.c $(wildcard *.h)
	nice $(CC) $(CFLAGS) -c $< -o $@

altcello: altcello.o $(LIBO)
	nice $(CC) $(CFLAGS) $^ -o $@ -lm

test_%: test_%.o $(LIBO)
	nice $(CC) $(CFLAGS) $^ -o $@ -lm

test: $(TESTS) test_geom
	@mkdir -p out
	./test_rng_ban.sh
	@for t in $(TESTS); do echo "== $$t"; nice ./$$t || exit 1; done
	@echo "== test_geom"; nice ./test_geom || exit 1
	@echo "ALL TESTS PASSED"

clean:
	rm -f *.o altcello geomdump test_geom bowhair_study $(TESTS)
	rm -rf out
