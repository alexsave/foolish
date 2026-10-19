# Mutation checks for foolish/c's tests added with the queue batch

Mutants were applied to a copy of foolish/c and shared/ and built with the same Makefile; "killed" names the first assertion that went red.

## tests/hand_contract_test.c (make hand-contract-test, part of make tests)

| Mutant | Result |
|---|---|
| Makefile CFLAGS without -ffp-contract=off | killed: the canary ("a*b+c fused: this build tree lacks -ffp-contract=off (-5.55e-17)") and the pin ("the sweep digest is c9bbb2ef34e2dc97") |
| hand_layout.c: `(double)n + 1.0` to `+ 1.0000001` | killed: the pin |
| hand_layout.c alone fuses (`#pragma clang fp contract(fast)`) while the test file does not | killed: the pin only (the canary reads this file's flags) |
| the fused comparison object built with -ffp-contract=off | killed: "the pin has teeth" |

The pin is the FNV-1a digest of every slot over 40 counts and 2400 widths for HAND_IOS, computed with contraction off: ab35cb265c121b11.
The teeth check runs only where the compiler fuses (`__FP_FAST_FMA`, arm64 clang), the canary and pin run everywhere.

## tests-wasm-caps (make tests-wasm-caps, part of make volume)

tests/tests.c built with -DWASM_CAPS_TESTS -DWORLD_LOG_CAP=40 -DCD_TT_BITS=12 -DCD_TT_2WAY -DCD_TT_PACK8 runs the world-clone cap, the short-log slot, the podkidnoy solver and book, the solver table seat and fill tests, the belief probe, the table bot drive and the octogen session-log cases.
The build refuses to compile (static asserts and #error) when the Makefile loses one of the shipped defines.

| Mutant | Result |
|---|---|
| bot_common.h bot_clone_world: `nl >= WORLD_LOG_CAP` to `nl > WORLD_LOG_CAP` | killed: "a world keeps no more than WORLD_LOG_CAP logs" |
| bot_common.h: the cap line removed | killed: same, and "the kept logs are the first discards" |
| bot_common.h: `dst->log_cap = 0` | killed: "the slot is marked short" |
| game.c log_alloc: short slots keep non-discards | killed: "what it kept is discards only" (added to test_short_log_instance, which runs in every build) |
| cordite_sim.c CD_TT_KEYTAG keeps 4 bits | killed: "a warm, full table gives the cold table's value (144 of 150)" and the podkidnoy table-entry check |
| cordite_sim.c CD_TT_KEYTAG always 0 | killed: same (134 of 150) |
| CD_TT_KEYTAG shifted to bits 52..63 (a 12-bit tag) | survives: a false hit needs the same slot and the same 12 tag bits, about 2^-12 per probe, none in 150 endgames |
| CD_TT_MASK twice the table (index past the allocation) | survives without ASan: the heap slack absorbs the writes; only a sanitized build of this target would see it |
| CD_TT_MASK half the table | equivalent (the tag still tells the entries apart; only the capacity halves) |

The selection runs in about 11 s: the belief-log test plays 2 games a width (the native run plays 6) because under the 4096-entry table a game costs about 3 s.
