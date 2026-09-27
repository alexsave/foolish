# Security review: what the consolidation added to shared/

Scope: the code the consolidation stream added to `shared/` on branch `shared-consolidation` as of 2026-09-27 (tip `ae44def8`), and the fixes that landed from the first review, re-checked as regressions.
The modules are `shared/c/text_util`, `wire_check`, `msg_seat_tag`, `msg_lobby_roster`, `stats`, `collapse`, `test`, and `shared/tools/wasm_cc.mk`.
The first review (`SECURITY_REVIEW_SHARED.md`) covered what was there before and sets the threat model used here: every function that takes peer bytes is fed bytes chosen by a hostile peer in the same group chat, and the same C runs as wasm32 in browsers and on a server edge.
Method: every claim below rests on a probe that was compiled and run.
Probes live in the session scratchpad under `secreview2/` (`P` below stands for that directory).
Each was built with `-fsanitize=address,undefined -fno-sanitize-recover=all`, again at `-O2`, and the freestanding modules also as wasm32 with `/opt/homebrew/opt/llvm/bin/clang` 22 (the shipping flags `--target=wasm32 -Oz -nostdlib -ffreestanding -mbulk-memory -mno-nontrapping-fptoint`, the same at `-O2`, and `-Oz -mno-bulk-memory`), run under node 26.
Line numbers for the two fixed modules are at `ae44def8`, before the fixes.
Every allocation a probe hands a function under test is a heap block of exactly the size the contract allows, so ASan reports a single byte past it.

## Verdict

No memory-safety defect is reachable from a bubble in any lifted module.
text_util, msg_seat_tag, msg_lobby_roster and wire_check held over 3 to 5 million randomized iterations each (billions of individual checks) under ASan and UBSan and at `-O2`, and the wasm32 builds produce the same digest as the native one over 300,000 mixed iterations.
The text_util column table is identical to the three pre-lift copies at every code point from U+0000 to U+1100FF, and its writers agree with the pre-lift copies on every random input.
Two **Low** findings were fixed on this branch, one commit each: `msg_seat_resolve` and `msg_seat_same_name` read past the roster on out-of-contract arguments, and `msg_lobby_roster` let a group capacity past 8 write past `who[]`.
Every product bounds both today (decoders refuse a name over 48 bytes and a roster over the cap; group capacities are compile-time constants with static asserts), so neither was reachable from a bubble; the fixes give each bound one owner and a test.
The first review's fixes all hold: the snprintf loop, the mixrad guards, the Python splicing in ship.sh, the PackedBytes cursor, the pinned CI downloads, and the Swift small-string scan, which found every short literal in sixteen compiled patterns.
One **Low** gap remains in the Release gate and is recorded as a proposal: an em dash with fewer than three characters of text around it (a lone `"U+2014"` literal) is not reported.

## 1. shared/c/text_util

Probe `P/p_text.c` (outputs `P/p_text_asan.out`, `P/p_text_o2.out`) links the shared module against the pre-lift copies taken from git at `320cabb0^` (`P/old_pk_text.inc`) and against an independent `text_fill` written from the header's words.
ASan and UBSan: 3,000,000 iterations, 320,220,072 checks; `-O2`: 5,000,000 iterations, 528,400,840 checks; 0 failed.
Each iteration steps and counts a random NUL-terminated byte string of 0 to 79 bytes (any byte, UTF-8 fragments including overlong leads, `C0`, `F8`-`FF` and stray continuations, or high bytes only), fills a random template of braces and names with a random list whose values are hostile names (braces, `{name}`, `{game}`, up to 290 bytes, NULL values), and calls every writer at caps -1, 0, 1, 2, the exact length, one and two past it, and a random cap.

SAFE, verified:
- `text_cp_cols` equals the pre-lift table (the one the three dice and card kernels carried) at every code point 0 to U+1100FF and at 0x1FFFFF, 0x3FFFFFF, 0x7FFFFFFF, 0x80000000 and 0xFFFFFFFF; the characters the product tests pin (`"X to play"` 9, Cyrillic 7, kana 5, Hangul 5) count the same.
  The UTTT kernel keeps its own table on purpose (it zeroes Hebrew, Arabic and Thai marks and has no emoji row) and steps with the shared `text_next_cp`; its tests were not changed.
- `text_next_cp` never steps over the NUL: every multi-byte lead stops at the first byte that is not a continuation, and the NUL is not one; the value is below 2^21 and `*len` is 1 to 4.
  `text_cols` never exceeds twice the byte length.
- `text_itoa` is exact for `INT_MIN`, `INT_MAX` and 200,000 random values at every cap from -2 to 13, refuses `n + 1 > cap`, and never writes at or past `cap`.
- `text_put` at cap 0 refuses, at cap 1 writes only `""`, and never writes past `cap`.
- `text_fill` never writes at or past `cap`, returns -1 exactly when the result and its NUL do not fit, and matches both the reference and the pre-lift fill with `{game}` as the fallback.
  A value is written as is and never expanded again, so a peer named `{game}{name}` is printed literally; an unterminated `{`, a `{{`, a `}` and an empty `{}` are copied as written; 200,000 `{` in a row cost linear time.

Findings:
- **Informational**, text_util.h:43: `text_put` and `text_fill` take a NULL source string as a contract violation (`strlen(NULL)`); every caller passes a table string or guards (tb_say.c:52 checks `names[seat]`).
- **Informational**, text_util.c:66: a name whose value is NULL counts as "the name is here, write nothing", so a list must still end in a NULL at a name position; the header says both lists are NULL-terminated pairs, which is this.

## 2. shared/c/wire_check

Probe `P/p_misc.c` (output `P/p_misc_asan.out`): 1,000,000 random head and body spans (0 to 199 and 0 to 4,999 bytes, empty spans passed as NULL) at every `check_len` from 0 to 40, each output a heap block of exactly `min(check_len, 32)` bytes, against `sha256` of the concatenation.
0 failed under ASan and UBSan.

SAFE, verified: the check is the leading bytes of SHA-256(head || body) for every span split; `check_len` above 32 (including `SIZE_MAX`) writes exactly 32; `check_len` 0 writes nothing; empty spans are never read.
Every consumer's decoder checks that `CHECK_LEN` bytes remain before calling it (for example tb_msg.c:332).

Findings: none.

## 3. shared/c/msg_seat_tag

Probe `P/p_seat.c` (outputs `P/p_seat_asan.out`, `P/p_seat_o2.out`, `P/p_seat_oob.out`, `P/p_seat_oob_fixed.out`).
It checks `msg_seat_utf8_chars` against an independent strict decoder written from Unicode table 3-7 with the header's control-character rule, on random names of 0 to 69 bytes built from valid, overlong, surrogate, past-U+10FFFF, C1 and truncated fragments, and exhaustively on every 1-, 2- and 3-byte string (16,777,216 x 3) and 16,777,216 4-byte strings over every `F0`-`FF` lead.
Then it calls `msg_seat_resolve` over every combination of roster size 0 to 8, started or not, sender -2 to n-1, record -3 to n+1 (so `MSG_SEAT_REC_GONE` and records past the roster), tag -2 to n+1, DM or group, `i_sent` -1 to 2, and a name absent, empty, present or with a negative length, with the rows in an exact-size heap array.
ASan and UBSan: 200,000 iterations, 367,506,048 resolve combinations; `-O2`: 2,000,000 iterations, 3,740,067,584 combinations, 13,437,568,711 checks; 0 failed.

SAFE, verified:
- The code point count and the verdict agree with the reference everywhere, at and past both caps: 16 ASCII is OK and 17 too long; 16 three-byte ideographs (48 bytes) OK, the same cut to 47 bytes BAD; 12 emoji (48 bytes) OK, 49 bytes too long; NULL or a length of 0 or less is EMPTY.
- Inside the contract `msg_seat_resolve` answers -1 or a row, reports the witness it used, lets a record on the roster win, and never lets the sender or the name overrule `MSG_SEAT_REC_GONE`.
- Every product decoder bounds a row's length to 1..48 and runs the verdict on every row (pk_msg.c:55, cn_msg.c:55, tb_msg.c:322), and every sender comes from a roster size or from the replayed game, which only records seats it dealt.

Findings:
- **Low, FIXED** (commit `70c42e05`), msg_seat_tag.c:59-65: a sender at or past `n` was taken as the witness; the function returned that seat, and before the start the lobby gate read `rows[sender]` (ASan: heap-buffer-overflow READ in `msg_seat_resolve`, `P/p_seat_oob.out`).
  `msg_seat_same_name` (msg_seat_tag.c:44) compared `name_len` bytes whatever `name_len` said, so a row claiming 255 read 207 bytes past its 48-byte buffer.
  Neither is reachable from a bubble, for the reasons above.
  Fix: a sender at or past `n` is no witness; a length past `MSG_SEAT_NAME_MAX_BYTES` matches nothing.
  Regression checks in `msg_seat_tag_test.c` (99 to 107 checks); removing either bound turns two of them red.

## 4. shared/c/msg_lobby_roster

Probe `P/p_lobby.c` (outputs `P/p_lobby_asan.out`, `P/p_lobby_o2.out`, `P/p_lobby_oob.out`): random sequences of 40 joins by handles that are and are not seated, leaves by seats -3 to 10, starts by seats -3 to 10 through a callback that says no a third of the time, and join-and-start queries, in a DM and in groups of every legal capacity, with the roster in an exact-size heap block.
After every operation it checks the invariants (1 to cap seats, cap at most 8, `newest` a seat or `NO_SEAT`, no handle twice, zero past the roster, exactly one control per viewer, START only for a seated viewer), that a refused operation changes no byte, that the callback runs exactly when START is offered and the lobby starts only when it says yes, and that `plan` reports exactly the one change.
ASan and UBSan: 1,000,000 sequences, 40,000,000 operations (2,825,677 joins, 1,007,549 leaves, 608,509 starts), 1,084,415,961 checks; `-O2`: 3,000,000 sequences, 120,000,000 operations; 0 failed.

SAFE, verified, inside the contract: every invariant above; a join past the cap, by a seated handle, or after the start changes nothing; a leave by a seat that does not exist, by the last seated player, or after the start changes nothing; the worst legal `plan` (eight leave, eight arrive) is 16 changes.
The products build the roster only from a decoded message whose seat count the decoder bounded, with a compile-time group capacity (pk_lobby.h:30, cn_lobby.h:25, tb_lobby.h:31 assert it is at most 8).

Findings:
- **Low, FIXED** (commit `51a37bb8`), msg_lobby_roster.c:5-7: `msg_lobby_roster_cap` returned `group_cap` as given, so a roster made with a capacity past 8 let `join` write `who[8]` and beyond (ASan: heap-buffer-overflow WRITE, `P/p_lobby_oob.out`); a hand-built roster whose `n_seats` was past 8 made `seat_of` and `plan` read past `who[]`, `plan` write past its 16-entry `out`, and `leave` shift and zero past `who[]`.
  Fix: the capacity is held to 2..8, every walk over the seats stops at 8, and a roster past 8 cannot be left; the in-contract sequences above give the same counts before and after.
  Regression checks in `msg_lobby_roster_test.c` (3,427 to 3,437 checks, a guard tail after `who[]`); the unfixed file fails all nine.

## 5. shared/c/stats, shared/c/stats/seed_hash.h, shared/c/collapse

Probe `P/p_misc.c` (output `P/p_misc_asan.out`), also built with `-fsanitize=float-cast-overflow`.

SAFE, verified:
- `stat_mean`, `stat_variance` and `stat_stderr` return 0 at n = 0 and n = 1 with no undefined behavior; DBL_MAX samples give a mean of inf and a variance of 0 (inf minus inf is NaN, which the `v > 0` test sends to 0); `stat_wilson` returns [0, 1] for no trials and stays inside [0, 1] around k/n for every k from 0 to 1000 of 1000.
- `seed_hash.h` is unsigned 64-bit arithmetic only; no undefined behavior is possible.
- `collapse_push` is finite, at most the travel in magnitude and monotone for every finite travel from `-FLT_MAX` to `FLT_MAX` at every time from `INT32_MIN` to `INT32_MAX`; it is exactly the travel at or before 0 and exactly 0 from 600 ms on, even for an infinite or NaN travel.
  No double-to-float conversion can overflow: the factor is between 0 and 1.

Findings:
- **Informational**, stats.c:31-39: outside the contract `stat_wilson` returns NaN when k is outside [0, n] or not finite, and [-inf, inf] for a fractional n near 1e-300 (4n^2 underflows to 0; IEEE division, not undefined behavior).
  A variance of NaN samples is reported as 0.
  These are dev-tool numbers from counts the arena itself made.
- **Informational**, collapse.h:31-41: an infinite or NaN travel is passed through until 600 ms; the travel is the host's own geometry, not peer input.

## 6. shared/c/test and shared/tools/wasm_cc.mk

- `check.h` and `twophone.h` are test-only and never linked into a shipped build; `first_fails_of` stops at `CHECK_NAMED_CAP` names and compares by `strcmp` on string literals. SAFE.
- **Informational**, wasm_cc.mk:21-47: `WASM_CC` is spliced unquoted into the recipe, as `CC` is everywhere in make; it comes from the developer's environment or CI's `ci_llvm.sh`, so setting it to a command runs that command with the developer's own rights, which is no new boundary.
  The guard refuses Apple clang by its version string, and a wrapper that prints another string passes it; that is a guard against accidents, as its comment says, not against a hostile toolchain.

## 7. The first review's fixes, as regressions

- `shared/c/wasm/libc.c` snprintf: the first review's hang probe, rebuilt from the current function (`P/p_snprintf.c`, `P/p_snprintf.out`), returns at `-O0`, `-O2` and `-Oz` on every case that used to hang, with the real snprintf's lengths and cuts.
  No shipped wasm source reaches a conversion the shim traps on: UTTT's web module formats `"%d"` only (uttt_say.c:21, :33); `uttt_analyse.c`'s `"%3d%%"` is native-only; the CARDS modules do not reference `snprintf`.
- `shared/c/mixrad.c`: base 0 now returns `MIXRAD_REFUSED` and a negative `*len` returns 0 without writing (`P/p_mixrad_edge.out`, ASan clean).
- `shared/tools/ship/ship.sh`: the first review's hostile env file (a quote and Python in `SHIP_APP_ID`) under a scratch root and fake HOME, run without `--build` so the build-number query runs, reached "stopping before the upload" with exit 0 and wrote nothing (`P/ship_dry_nob.out`); every Python text is a constant and values arrive as `sys.argv`.
  The keychain is locked by an `EXIT` trap.
- `shared/scripts/ci_llvm.sh`: `llvm.sh` comes from a commit-pinned URL and both downloads are checked against committed SHA-256 values before use; curl is `--proto '=https' --tlsv1.2`; temporary files come from `mktemp`.
- `shared/swift/PackedBytes.swift`: the first review's fuzz, rebuilt against the current file (`P/pb.out`): 24,000,000 reads, 15,359,172 nil, 0 nil reads that moved the cursor; `u8s(Int.max)` and `init(at: -1)` return nil instead of trapping.
- `shared/tools/release_strings/rs_scan.c`: an in-process harness (`P/rs/fz.c`) ran the three scanners on 1,000,000 hostile buffers of up to 200,000 bytes (random bytes; Mach-O headers over storms of `movz`/`movk`/`movn` with string-shaped immediates; text of `dev.`, dashes, NULs and UTF-16 units) under ASan and UBSan with no report (`P/rs_fuzz.out`).
  An empty file, a four-byte Mach-O magic and an unreadable path behave (exit 0, 0, 2).
  Sixteen Swift patterns compiled at `-O`, `-Osize` and `-Onone` and stripped (`P/rs/a.swift`, `P/rs_swift.out`): every dev-file name was reported, whether returned from a switch, stored in an array, a dictionary key, a static, a default argument, compared with `==`, a 15-byte literal, or built at run time from `"dev."`, and so was `"Go U+2014 now"`.

Findings:
- **Low, proposal**, rs_scan.c:129: an em dash is reported only with at least `MIN_CTX` (3) printable bytes around it, so a lone `"U+2014"` or `"1 U+2014 2" (no spaces)` Swift literal passes (`P/rs/b.swift`: only `"ok U+2014 go" (no spaces)` of the three is reported).
  A small string is text by construction (its discriminator was checked), so the small-string channel could report any em dash with no context rule.
  Not fixed here, because it changes what the gate reports on a real bundle.
- **Informational**, rs_scan.c:50-62: `slurp` does not check `ferror`, so a read error part-way through a file scans what was read and can pass; a directory argument reads as empty.
  `release_strings.sh` passes only `find -type f` results, and the input is our own build output.
- **Informational**, rs_scan.c:181-260: the small-string rebuild follows move-wide immediates only; a word built another way (an `orr` logical immediate for a repeating pattern, arithmetic, a load) is not rebuilt.
  None of the sixteen patterns needed it, and the structural control (no `DevFlags` symbol or type name in a Release binary) does not depend on it.

## What could not be verified, and why

- Every sender a product hands `msg_seat_resolve` was traced to a roster size or a replayed game by reading, not by fuzzing each product decoder again; the product fuzzers (`pk_fuzz`, `cn_fuzz`, `tb_fuzz`, millions of assertions each) run in each `make run`.
- `wire_check` with a NULL `out` and `check_len` 0 calls `memcpy(NULL, d, 0)`, which C calls undefined; no caller passes NULL, and UBSan's `nonnull-attribute` check was not run against it.
- The Release gate was checked against compiled Swift on this Mac's toolchain; other compiler versions may build small strings differently.

## Findings

| Severity | Where | Needs | What happens | Fix |
| --- | --- | --- | --- | --- |
| Low, fixed `70c42e05` | shared/c/msg_seat_tag/msg_seat_tag.c:44, :59-65 | a caller passing a sender at or past `n`, or a row with `name_len` over 48 (every product bounds both) | out-of-roster read; a seat past the roster returned | a sender past `n` is no witness; a length past 48 matches nothing |
| Low, fixed `51a37bb8` | shared/c/msg_lobby_roster/msg_lobby_roster.c:5-7, :17, :37-47, :82-91 | a caller passing `group_cap` over 8 or a roster with `n_seats` over 8 (every product uses a constant at most 8) | write past `who[]` and past `plan`'s `out` | capacity held to 2..8; walks stop at 8 |
| Low, proposal | shared/tools/release_strings/rs_scan.c:129 | an em dash with under three characters of text around it | the Release gate passes it | report any em dash in the small-string channel |
| Informational | shared/c/text_util/text_util.h:43, text_util.c:66 | a NULL source; a NULL value mid-list | crash on NULL; list must still end on a name position | contract, documented |
| Informational | shared/c/stats/stats.c:31-39 | k outside [0, n], n near 1e-300, NaN samples | NaN or infinite interval; NaN variance read as 0 | dev tool; none |
| Informational | shared/c/collapse/collapse.h:31-41 | a NaN or infinite travel | passed through until 600 ms | host geometry; none |
| Informational | shared/tools/wasm_cc.mk:21-47 | a developer's own `WASM_CC` | runs as given; the guard is a version-string check | none |
| Informational | shared/tools/release_strings/rs_scan.c:50-62, :181-260 | a read error; a word not built from move-wide immediates | partial scan passes; small string not rebuilt | check `ferror`; structural check remains the control |

Every consumer suite was run before and after each fix: `make -C uttt/c run`, `make -C pickemup/c run`, `make -C chuiniu/c run`, `make -C tallybones/c run` and `asan`, `make -C foolish/c tests`, `make -C werewolf/c tests`, and `npm run test:validate` in foolish against a throwaway Postgres on 55432 (removed after).
Every output is identical line for line except the two shared test lines, and validation is 150 of 150 both times.
