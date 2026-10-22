# Mutation checks for shared/c's tests

Each mutant was applied to a COPY of shared/c (never the working tree), the test was built and run, and the file restored.
"Killed" names the first assertion that went red.
"Equivalent" means the mutated code gives the same answers for every input, so no test can tell it from the original.

## wasm/libm_test.c (sincos, sincosf, sc_sincos)

| Mutant | Result |
|---|---|
| libm.c sincos: case 1 outputs swapped to the case 3 pair (`*s = -b; *c = a`) | killed: "sincos sin bits" / "sincos cos bits" in the sweep |
| libm.c sincos: case 3 (default) outputs swapped to the case 1 pair | killed: same assertions, first at x = -121.7 (quadrant 3) |
| libm.c sincos: cos series fed `r + 1e-15` | killed: "sincos cos bits" |
| libm.c sincosf: sin and cos casts swapped | killed: "sincosf sin bits" |

Left out on purpose: NaN and the infinities are skipped under UBSan (quad() casts floor(x / (pi/2)) to long long, undefined for them, a trap in the wasm module); the native run holds that sin and sincos agree on them.

## stats/percentile_test.c

| Mutant | Result |
|---|---|
| tolerance `1e-9` to `0` | killed: "p7 of 100 is rank 7" (0.07 * 100 is 7.000000000000001) |
| top clamp `rank > n` removed | killed: "p over 100 is the maximum" |
| f64 form reads rank - 1 | killed: "f64 p95 of 20" and three more |
| comparator reversed | killed: "sorted" and "the three answers" |
| comparator never returns 0 | killed: "the three answers" |
| `p <= 0` guard removed | equivalent (rank clamps to 1) |
| `n <= 0` guard to `n < 0` | equivalent (rank clamps to 0, index -1) |

## util_test.c

Killed: clampi bound order swapped ("int 5 in [10, 0]"), clampf and clampd end tests, mini, maxi, mind, maxd, popcount64 narrowed to 32 bits, ARRAY_LEN by `sizeof(int)`.
No survivors.

## clock_test.c

| Mutant | Result |
|---|---|
| sleep nanoseconds * 1000 instead of * 1000000 | killed: "a sleep of 30 ms is 30 ms" (slept 58 us) |
| `host_mono_us` divides by 100 | killed: "us is ns / 1000" |
| `host_real_ms` divides by 1000 | killed: "real ms ... is a date" |
| process cpu clock id replaced by CLOCK_MONOTONIC | killed: "30 ms of sleeping is not 25 ms of cpu" |
| deadline carry removed | killed: "nanoseconds in range" |
| deadline seconds `ms / 100` | killed: "1500 ms ahead" |
| sleep seconds `ms / 100` | NOT killed: the seconds part is 0 for any sleep under 100 ms; seeing it needs a sleep of a second or more, and the test is held to about 60 ms |
| `ms <= 0` guard removed | equivalent (nanosleep refuses a negative time at once) |

## text_util/text_util_test.c (utf8.h)

Killed: surrogate range dropped, surrogate top 0xdfff and bottom 0xd800 shifted by one (the edge checks U+D7FF, U+D800, U+DFFF, U+E000), three-byte minimum 0x800 to 0x400, four-byte minimum 0x10000 to 0x8000, maximum 0x10ffff to 0x110000, C1 top and bottom shifted, C1 range dropped, DEL, C0 upper edge, continuation check, the length check (needs the sequence's last byte present in memory: "cut at 2 with its last byte in memory"), UTF-16 count for astral, outputs untouched on refusal.
Equivalent: two-byte lead 0xc2 widened to 0xc0 (the minimum check refuses the overlong value anyway), lead limit 0xf4 widened to 0xf7 (the maximum check refuses), the ASCII skip's `i + 8 <= n` to `<` (speed only).

## ease_test.c

Killed: either clamp removed or returning x, the bisection fallback removed (needs the flat-tangent curves added here: {0,0,0,0}, {1,0,0,1}, {1,1,0,0}, {0,1,0,1}, {1,0,1,1}, {1,.2,0,.8}), bisection bounds swapped, tolerance 1e-6 to 1e-2, y coefficient.
Equivalent: `x <= 0` to `x < 0` and `x >= 1` to `x > 1` (the solver returns exactly 0 and 1 at the ends for every curve, asserted bit for bit), the derivative stall guard (the bisection fallback gives the same answer).

## le_bytes_test.c (le_bytes.h u64, be_bytes.h)

Killed: le_put_u64 high shift, word order, le_get_u64 high shift and low-word sign extension, be_put_u16 byte order, be_put_u32 second byte, be_get_u32 second byte.
Equivalent: be_get_u16 with a sign-extended high byte (the cast to uint16_t discards it).

## test/check_test.c and test/check_ok_test.c (check.h)

Killed: CHECK_PRINT_CAP `<=` to `<`, to `<= cap + 1`, default 5 to 6, a single global counter instead of one per test, the name table compared by pointer, g_fails not counted, report() always 0.
check.h's own OK(c, what) was not held at all (check_test.c includes twophone.h, which redefines it): check_ok_test.c holds its message verbatim; mutants `CHECK(c, "x")` and `CHECK(c, what)` are killed by "prints the message as written".

## fnv_test.c

Killed: 64-bit prime, 64-bit offset, 32-bit prime, 32-bit offset, xor and multiply order.

## sha256_openssl_test.c and deal_rng_openssl_test.c (make -C shared/c openssl-tests)

| Mutant | Result |
|---|---|
| deal_rng_block: feed-forward add dropped | killed: "block equals OpenSSL's" |
| deal_rng_block: 10 double rounds to 9 | killed: same |
| deal_rng_seed: sigma words 0 and 1 swapped | killed: "words from block ... equal OpenSSL's" |
| deal_rng counter carry into word 13 dropped | not by this test (it keeps the counter under 2^32 by design); killed by deal_rng_test.c "the counter carries from block 2^32-1 into 2^32" |
| sha256 padding byte 0x80 to 0x81 | killed: "one call, 0 bytes" |
| sha256 pad to 55 mod 64 instead of 56 | killed: same |
| sha256 bit length counted * 4 | killed: "one call, 1 bytes" |
| sha256 length byte shift 56 to 48 | killed: same |
| sha256 K[0] changed | killed: "one call, 0 bytes" |
| sha256 buffer flush at 65 | killed by a hang (the run times out) |
| sha256 `c->bitlen = bits` removed | equivalent (the length bytes are taken from the local `bits`) |

## stats/hist_test.c, proc_test.c (2026-10-22)

The machinery of a server's own numbers, lifted from two apps on c_base. Each mutant was applied to the working file, the test rebuilt and run, and the file restored by the reverse edit; c_base's fuzz target over the registry (fz_metrics.c) found the two defects named below before any mutation was tried.

| Mutant | Result |
|---|---|
| hist.c bucket fraction `* PER` made `* 4` (four per octave again) | killed: "3" (stat_hist_bucket(3) == 12) |
| hist.c pct's clamp to the max removed | killed: "p99 of 1..100 reads 100 ... clamped to the max" |
| hist.c pct's rank interpolation flattened to the bucket's floor | killed: "within 1% of the nearest-rank truth on a uniform spread" |
| hist.c pct's p100 branch (the max itself) removed | killed: "p100 is the outlier" |
| hist.c the max CAS removed | killed: "max 100" |
| hist.c bucket 0's floor 1 (the fuzzer's find: a p99 of 1 over nothing but zeros) | killed: "bucket 0 holds 0 and 1: its floor is 0" |
| hist.c per_s counts a slot whose second is not the one asked | killed: "ten seconds of silence: 0" |
| hist.c a slot taken over by a new second not reset | killed: "a reused slot was reset" |
| hist.c slot_of made a plain `%` (the fuzzer's find: second -2 indexes before the array) | killed: "and holds its count" (the out-of-bounds write lands in count[8]; the per_s assertions alone survive, since the wrong slot reads back consistently, so the slot is checked by name) |
| proc.c rss answers 0 for pid 0 | killed: "this process: at least a megabyte" |
| proc.c cpu_us for pid 0 answers a thousandth | killed: "the reading moved by at least 5 ms of work" |
