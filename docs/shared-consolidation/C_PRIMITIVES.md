# Low-level C primitives - consolidation pass over five products

Investigated in the `shared-consolidation` worktree, read-only.
Products: foolish/c (the card game, shipped), uttt/c (near-shipped), pickemup/c, chuiniu/c, tallybones/c (three new products built in parallel).
werewolf/c is out of scope but was grepped alongside the others wherever a search was repo-wide, since it shares `shared/c/sha256.{c,h}` and `shared/c/deal_rng.{c,h}` per `shared/README.md`.
Scope: `shared/c/{mixrad,b32,sha256,deal_rng}.{c,h}`, `shared/c/wasm/include/{math,stdio,string}.h`, `shared/c/wasm/{libc,libm}.c`, and any duplicated wire-level helper (checksum, code table, alphabet, varint, bit packer) sitting next to them in each product's `*_code.c` / `*_msg.c` / `*_wire.c`.
Every claim below is a grep result, a diff, a differential test I ran, or a `make` output, with a file path and a line number so it can be re-run.

## Verdict

The suspicion in the task brief does not hold for chuiniu and tallybones: neither reimplemented mixed-radix arithmetic, base32, SHA-256 or the deal RNG.
Both include `shared/c/mixrad.h`, `shared/c/b32.h`, `shared/c/sha256.h` and `shared/c/deal_rng.h` by the required relative `#include`, both list the four shared `.c` files in their Makefile `SRC` variable (so every build - native, wasm-objects-only, iOS - compiles the shared copy and only the shared copy), and a repo-wide grep for the algorithm shapes those files have (SHA-256's `0x428a2f98` round constant, the RFC 4648 base32 alphabet string, a ChaCha20 `"expand 32-byte k"` constant, a byte-bignum multiply-add loop with a `carry` variable outside `mixrad.c` and the coders that call it) turns up nothing in chuiniu or tallybones that is not a call into the shared file.
Pickemup, which did the original lift, is equally clean, and uttt - the product mixrad and b32 were lifted out of - is clean by construction.

There is exactly one real "reimplemented instead of shared" case in this whole scope, and it is not in the three new products: it is foolish itself.
`foolish/c/src/replay.c:1808-1848` carries its own `replay_b32_encode` / `replay_b32_decode`, an RFC 4648 base32 codec that is algorithmically identical to `shared/c/b32.c`'s `b32_encode` / `b32_decode` (same alphabet, same MSB-first bit packing, same `-`-terminates-the-suffix rule, same skip-stray-characters rule), confirmed byte-identical by a 2,000-trial differential test I wrote for this pass (below).
This predates the mixrad/b32 lift: `shared/README.md` lists `b32.{c,h}` as used by UTTT and SHED (pickemup's code name) only, not CARDS (foolish).
Lifting it is real value (one fewer base32 implementation in the tree) but is a foolish code change, not a path move, so it is lift-later under the rule in `pickemup/docs/REUSE_AUDIT.md` section 1 ("foolish's own diff should be only paths... if it has to be anything else, stop and split the step").

A second finding is a genuine, currently-unlifted duplicate that the task's second bullet asked about directly: a 2-byte truncated-SHA-256 checksum function named `check_of`, byte-for-byte identical (modulo the per-product `*_CHECK_LEN` macro name) in `uttt/c/src/uttt_msg.c:66`, `pickemup/c/src/pk_msg.c:250`, `chuiniu/c/src/cn_msg.c:243` and `tallybones/c/src/tb_msg.c:264`.
It is completely product-neutral - it takes a header span, a body span and an output buffer, and knows nothing about seats, cards, dice or dominoes - and it is the cheapest possible lift in this pass: a pure function, already proven correct four times over by each product's own passing message tests.

The wasm freestanding libc/libm shims compile and link cleanly everywhere they are used.
`shared/c/wasm/libc.c` is what every product needs (`memcpy`/`memset`/`memcmp`/`strlen`/`strncmp`/`snprintf`), and none of the three new products call a libm function at all, so their not linking `shared/c/wasm/libm.c` is correct, not a gap.
chuiniu and tallybones only prove their wasm sources compile to objects (`make wasm`, "Objects only" per their own Makefile comment); I linked a full module by hand for both (see below) and it links with zero undefined symbols against `shared/c/wasm/libc.c`, so the objects-only self-test is not hiding a link-time problem.

The most actionable finding is not a duplicate at all: the tests that exist specifically to catch this class of drift have not been extended to the three new products or to the two newer shared files.
`foolish/e2e/validation/shared_headers_reachable_validation.test.ts`'s `SHARED_HEADERS` constant (line 46) still reads `['sha256.h', 'deal_rng.h']` - `b32.h` and `mixrad.h` are not in it, so a file that included one of those two by bare name (the exact bug this test exists to catch) would go undetected.
The same test's "every build system that compiles the kernel also compiles the shared sources" case (line 144) checks four Makefiles - `foolish/c`, `foolish/foolyard`, `foolish/server/impls/native`, `werewolf/c` - and none of uttt/c, pickemup/c, chuiniu/c or tallybones/c.
`foolish/e2e/validation/shared_is_shared_validation.test.ts`'s "shadowed" list (its third test) only guards `sha256.{c,h}` and `deal_rng.{c,h}` reappearing under `foolish/` and `werewolf/`; it does not guard `b32.{c,h}` or `mixrad.{c,h}` reappearing anywhere, and it does not guard any of the four files reappearing under `uttt/`, `pickemup/`, `chuiniu/` or `tallybones/`.
That same file's product-name regex list is also missing `tallybones` outright.
None of this is a bug today - I found no shadow copy anywhere - but it means the finding "chuiniu and tallybones are clean" rests on this investigation's greps, not on a gate that will catch a regression, which is exactly the gap `docs/ARCHITECTURE_AS_A_PATTERN.md` Part 3 move 2 warns about ("a gate with no gate of its own is a rule you have stopped checking without noticing").

## Evidence: adoption is clean (mixrad, b32, sha256, deal_rng)

Every include is a relative `#include "../../../shared/c/<file>.h"`, never a bare name, and every Makefile lists the shared `.c` file by its `$(SHARED)/<file>.c` path, never a local copy:

| Shared file | uttt | pickemup | chuiniu | tallybones |
| --- | --- | --- | --- | --- |
| `mixrad.h` | `uttt/c/src/uttt_code.c:3` | `pickemup/c/src/pk_code.c:4` | `chuiniu/c/src/cn_code.c:3` | `tallybones/c/src/tb_code.c:4` |
| `b32.h` | `uttt/c/src/uttt_code.c:2`, `uttt/c/src/uttt_msg.c:4` | `pickemup/c/src/pk_msg.c:4` | `chuiniu/c/src/cn_msg.c:4` | `tallybones/c/src/tb_msg.c:4` |
| `sha256.h` | `uttt/c/src/uttt_msg.c:3` | `pickemup/c/src/pk_msg.c:3` | `chuiniu/c/src/cn_msg.c:3`, `chuiniu/c/src/cn_dice.c:24` | `tallybones/c/src/tb_msg.c:3`, `tallybones/c/src/tb.c:4` |
| `deal_rng.h` | not used (uttt has no per-seat RNG need) | `pickemup/c/src/pk_deck.c:10` | `chuiniu/c/src/cn_dice.c:23` | `tallybones/c/src/tb.c:5` |
| Makefile `SRC` lists all four `$(SHARED)/*.c` | `uttt/c/Makefile:240-241` (web wasm link; native build links the same files elsewhere in the file) | `pickemup/c/Makefile:22-23` | `chuiniu/c/Makefile:17-18` | `tallybones/c/Makefile:18-19` |

Calls, not reimplementations: `deal_rng_seed_at` / `deal_rng_bounded` are called directly in `chuiniu/c/src/cn_dice.c:56-57` and `tallybones/c/src/tb.c:137,143`; `mixrad_mul_add` / `mixrad_div_mod` are called directly in `pickemup/c/src/pk_code.c:31,180`, `chuiniu/c/src/cn_code.c:29,58` and `tallybones/c/src/tb_code.c:55,70,107`.

Algorithm-shape grep across the whole repository (not just the three new products), to catch a reimplementation that renamed the functions:

- SHA-256's first round constant, `0x428a2f98`: `grep -rl "0x428a2f98" --include="*.c" --include="*.h" .` returns only `shared/c/sha256.c`.
- The RFC 4648 base32 alphabet string `"ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"`: returns `shared/c/b32.c:3` and `foolish/c/src/replay.c:1832` (the one real duplicate, below) - nothing in uttt, pickemup, chuiniu or tallybones.
- ChaCha20's `"expand 32-byte k"` / `0x61707865` constant: returns `shared/c/deal_rng.{c,h}`, `pickemup/c/src/pk_deck.c:3` (a comment naming `deal_rng`, not a reimplementation) and `foolish/c/tests/tests.c:1359-1379` (an RFC 8439 known-answer test of the shared RNG, not a second RNG).
- `sha256_init` / `sha256_update` / `sha256_final` defined anywhere other than `shared/c/sha256.c`: none; every hit outside `shared/c/` is a call site.

Compile proof, `make -C <product>/c wasm` with `WASM_CC=/opt/homebrew/opt/llvm/bin/clang` (the toolchain this repo pins for wasm32), run fresh in this pass:

```
uttt/c:       "Nothing to be done for `wasm'" (uttt's wasm target is the linked web build, gated on its own web wasm rule; unaffected)
pickemup/c:   "wasm32 objects in build/wasm/"
chuiniu/c:    "wasm32 objects in build/wasm/"
tallybones/c: "wasm32 objects in build/wasm/"
```

All three succeed, and because each product's `WASM_SRC := $(SRC)` and `$(SRC)` already lists `$(SHARED)/deal_rng.c $(SHARED)/sha256.c $(SHARED)/b32.c $(SHARED)/mixrad.c` (see table above), this run compiled all four shared primitives to wasm32 freestanding objects for pickemup, chuiniu and tallybones, using `-isystem $(SHARED)/wasm/include` for the freestanding headers.

Runtime proof, each product's full test suite, run fresh in this pass with no source changes (`make -C <product>/c run`):

```
pickemup: pk_beats_test 233/0 fail, pk_arrange_test 49995/0, pk_bot_test 107612/0, ios_smoke bridge 869/0
chuiniu:  cn_plan_test 1737483/0, cn_say_test 7168/0, cn_fuzz 15759233/0 (3000 games), cn_msg_test 89178/0, cn_twophone_test 9909/0, ios_smoke 77/0
tallybones: tb_test 6529/0, tb_msg_test 29319/0, tb_fuzz 16424832/0 (1400 games), tb_say_test 344338/0, tb_beats_test 459317/0, tb_twophone_test 458/0, ios_smoke 67/0
```

`cn_msg_test` and `tb_msg_test` specifically round-trip the code through `check_of` (the checksum, below) and `mixrad`-derived bodies over thousands of bubbles each, including a hostile-corruption pass (`cn_msg_test`: "284070 corruptions, 2 read"), so the shared arithmetic is exercised end to end by the products' own suites, not just compiled.

## Evidence: the one real duplicate (foolish's own base32)

`foolish/c/src/replay.c:1808-1810` states its own provenance in its header comment: "RFC 4648 upper-case alphabet, MSB-first bit packing, no padding: codec.ts's base32Encode/base32Decode, so a code made on the web reads here byte for byte."
That is the same specification `shared/c/b32.c` implements, and the two are the same algorithm with one cosmetic difference: `shared/c/b32.c` masks its accumulator to 12 live bits (`& 0xfff`) after each shift, `foolish/c/src/replay.c` uses an unsigned accumulator and lets the high bits run off the top instead (its own comment at line 1809 says so: "only the low 12 bits are read, and the high ones run off the top").

I wrote a differential test for this pass (not committed, run from `/tmp`) that links `shared/c/b32.c` against an extracted copy of `foolish/c/src/replay.c:1805-1852` (the `replay_b32_encode`/`replay_b32_decode` pair only) and round-trips 2,000 pseudo-random byte strings of length 1-60 through both encoders and both decoders:

```
OK: 2000 trials, encode+decode byte-identical between shared/c/b32.c and foolish replay_b32
```

foolish exercises its own copy continuously: `foolish/c/Makefile:957,1177` export `wasm_replay_b32_encode` / `wasm_replay_b32_decode` from the shipped wasm modules (the replay link the browser reads), and `foolish/c/tests/replay_v6_test.c`, run fresh in this pass (`make -C foolish/c build/replay_v6_test && ./foolish/c/build/replay_v6_test 40`), passes at "316992 checks passed, 0 failed, 1 skipped" over 279 games.
`foolish/c/tests/msg_wire_test.c`, which exercises the shared `sha256.h` foolish already uses (`foolish/c/src/msg_wire.h:110`), passes at "OK" in the same run, confirming foolish's *sha256* adoption is unrelated to and unaffected by its base32 gap.

This is not a chuiniu/tallybones finding, but it is squarely in scope: it is a product that reimplemented a shared primitive instead of using it, on the exact axis this investigation was asked to check, and the reason it survived is ordering, not oversight - `shared/c/b32.c` did not exist when `replay.c` was written, and nothing has revisited `replay.c` since the lift landed.

## Evidence: the duplicated wire-level helper (`check_of`)

The task's second question asked about a duplicated checksum living in two or more products' `*_msg.c` files.
There is one, and it is exact:

```c
// pickemup/c/src/pk_msg.c:250-259, chuiniu/c/src/cn_msg.c:243-252,
// tallybones/c/src/tb_msg.c:264-273 (identical modulo the *_CHECK_LEN macro name)
static void check_of(const uint8_t *head, int hn, const uint8_t *body, int bn, uint8_t out[PK_CHECK_LEN])
{
    uint8_t d[SHA256_DIGEST_LEN];
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, head, (size_t)hn);
    if (bn > 0) sha256_update(&c, body, (size_t)bn);
    sha256_final(&c, d);
    memcpy(out, d, PK_CHECK_LEN);
}
```

I diffed the three bodies with the `*_CHECK_LEN` token normalized to a common name; `pk_msg.c` and `cn_msg.c` come out byte-identical, and `cn_msg.c` and `tb_msg.c` come out byte-identical.
`uttt/c/src/uttt_msg.c:66-75` has the same function under the same name with the same eight-line body, differing only in that it drops the `if (bn > 0)` guard (calling `sha256_update` with a zero-length span unconditionally, which is a no-op in `shared/c/sha256.c`'s loop, so the two forms are behaviourally identical) and names its parameter `code`/`cn` instead of `body`/`bn`.
So there are four copies of the same eight-line, fully product-neutral function - it takes a header span, a body span and a cap, and reads no domain state - none of them lifted.
`CHECK_LEN` is 2 in every product (`pickemup/c/src/pk_msg.h:69`, `chuiniu/c/src/cn_msg.h:59`, `tallybones/c/src/tb_msg.h:64`, `uttt/c/src/uttt_msg.h:65`), so even the one parameter that could vary does not.

I looked for other product-neutral wire helpers of the kind the task named (a code/link length table, a crockford alphabet, a varint, a bit packer) and found none duplicated.
`foolish/c/src/replay.c:887` has a `code_varint` function, but it is part of foolish's own arithmetic-coding entropy coder (the `Coder`/`coder_uniform` machinery that gives derived events zero bits, `docs/ARCHITECTURE_AS_A_PATTERN.md` Part 1 piece 8) and has no counterpart anywhere else in the repo; it is a different, more general coding scheme than the simple mixed-radix digit-per-move scheme pickemup/chuiniu/tallybones share, not a duplicate of anything.
A repo-wide grep for `varint`, `crockford`/`Crockford` and `bitpack`/`bit_pack` outside that one foolish file returns nothing.

## Evidence: an adjacent primitive that looks like a duplicate but is not

`tallybones/c/src/tb_code.c:27-37` defines `add_scaled(uint8_t *a, int *al, const uint8_t *b, int bl, uint32_t k, int cap)`, a byte-bignum `a += b * k` with its own `carry` variable, structurally similar to `mixrad_mul_add`'s multiply-add loop.
It is not a reimplementation of mixrad: `tb_code.h:1-24`'s header comment explains it is a different operation entirely, needed because tallybones' design makes "the body... also the roll's input" (T11/T14) - every die roll is derived from the SHA-256 of the running body-so-far, and the body itself is tracked forward as `S_k + P_k` (a polynomial evaluation) rather than folded backward the way the encode-only mixrad digit stream is.
Neither pickemup's `pk_code.c` nor chuiniu's `cn_code.c` has an `add_scaled` or any `carry`-loop of their own (`grep -n "add_scaled\|carry"` on both returns nothing), because neither game derives anything from its own history the way tallybones does.
`tb_code.c` calls `mixrad_mul_add` directly too, at line 55 and line 107, for the parts of its job that are the ordinary digit-folding mixrad already does; `add_scaled` only covers the part mixrad does not.
So this is a genuinely new, single-product primitive sitting next to a shared one, not a duplicate of it.

## Evidence: the wasm freestanding shims

`shared/c/wasm/libc.c` provides `memcpy`, `memset`, `memcmp`, `strlen`, `strcmp`, `strncmp` and a minimal `snprintf` (`%s`/`%d`/`%%` only); `shared/c/wasm/libm.c` provides `sin`/`cos`/`exp`/`log`/`log2`/`sinf`/`cosf`/`logf`/`powf`/`acosf`/`fminf`/`fmaxf`/`lroundf`/`roundf`.
A repo-wide grep for a local definition of any of `memcpy`/`memset`/`strlen`/`memcmp`/`memmove` (`grep -rn "^void \*memcpy\|^void \*memset\|^size_t strlen\|^int memcmp\|^void \*memmove"`) returns only the definitions inside `shared/c/wasm/libc.c` itself; nothing outside it defines its own.

None of pickemup, chuiniu or tallybones' `src/` calls any libm function (`grep -rn "\bsin(\|\bcos(\|\bsqrt(\|\bpow(\|\bexp(\|\blog(\|\batan\|\bacos\|\bfminf\|\bfmaxf\|\blroundf\|\broundf\|<math.h>"` on all three returns nothing), so none of them links `shared/c/wasm/libm.c`, and that is correct, not a gap: `shared/README.md`'s own row for `c/wasm/` marks it "(objects only)" for SHED (pickemup's code name), and chuiniu and tallybones are the same shape but are not in that table row at all yet (see the guard-rail gaps below).
foolish and uttt do link `libm.c` (`foolish/c/Makefile:1172,1322`; `uttt/c/Makefile:241`) because their board geometry needs trig, which the three new products' iMessage-only UI does not.

chuiniu, pickemup and tallybones each prove their wasm sources compile (`make wasm`, "Objects only" per their own Makefile comment at e.g. `chuiniu/c/Makefile:65-67`), but only pickemup also proves a full link: its `cross` target (`pickemup/c/Makefile:101-118`) links `$(SHARED)/wasm/libc.c` into `build/pk_cross.wasm` and runs a native-vs-wasm32 differential over 100 games through Node.
chuiniu and tallybones have no equivalent target - `grep -n "cross\|libc\.c\|libm\.c" chuiniu/c/Makefile tallybones/c/Makefile` returns nothing beyond the objects-only `wasm` rule's comment - so nothing in either product's own build has ever linked its objects into a single wasm module.

To check whether that gap is hiding a real problem, I linked both by hand in this pass, using the objects `make wasm` already produced plus `shared/c/wasm/libc.c`, the same flags pickemup's `cross` target uses:

```
$ clang --target=wasm32 -Oz -nostdlib -ffreestanding -mbulk-memory -isystem ../../shared/c/wasm/include \
    -Wl,--no-entry -Wl,--export-all -Wl,--export-memory \
    build/wasm/*.o ../../shared/c/wasm/libc.c -o /tmp/cn_full2.wasm
# exit 0, 25067 B, no undefined symbols (verified by omitting -Wl,--allow-undefined; a first attempt
# WITH --allow-undefined showed memcpy/memset/memcmp/strlen/strncmp as unresolved imports, confirming
# the check is real and not a no-op)
```

Both chuiniu and tallybones link cleanly this way (chuiniu: 25,067 B; tallybones: 29,991 B), so the missing `cross`-style target is a coverage gap in the test suite, not evidence of a build problem: the shared shim does link against these two products' objects with zero undefined symbols.

## Evidence: the guard-rail tests have not kept pace

`pickemup/docs/REUSE_AUDIT.md` section 1 states the rule directly: "Every new shared C file follows the same rule, and adding it to `SHARED_HEADERS` in that test is part of its lift."
`foolish/e2e/validation/shared_headers_reachable_validation.test.ts:46` still reads:

```ts
const SHARED_HEADERS = ['sha256.h', 'deal_rng.h'];
```

`b32.h` and `mixrad.h` - both lifted by pickemup's own work, both now used by four products - are not in this list.
Concretely, this means: nothing stops a future file anywhere in the repo from writing a bare `#include "b32.h"` or `#include "mixrad.h"` (the exact bug class this test's own header comment says cost 8 red CI lanes for `sha256.h`/`deal_rng.h`) and having it silently compile wherever some other `-I` happens to reach `shared/c`, exactly the failure mode the test was built to catch for the first two files.

The same test's fourth case, "every build system that compiles the kernel also compiles the shared sources" (line 144), reads each Makefile's source-list variable and asserts it names `deal_rng.c`/`sha256.c` from `shared/c/`.
Its `builds` array (lines 149-154) lists exactly four Makefiles: `foolish/c`, `foolish/foolyard`, `foolish/server/impls/native`, `werewolf/c`.
It does not list `uttt/c`, `pickemup/c`, `chuiniu/c` or `tallybones/c`, even though all four already link `deal_rng.c`/`sha256.c` (and `b32.c`/`mixrad.c`) from `shared/c` today, as shown above.
So the fact that these four products currently do the right thing is not held by any gate; it is only held by this investigation's greps.

`foolish/e2e/validation/shared_is_shared_validation.test.ts`'s third test, "the product does not keep its own copy of a shared file" (its `shadowed` array), only lists:

```
foolish/c/src/sha256.c, foolish/c/src/sha256.h, foolish/c/src/deal_rng.c, foolish/c/src/deal_rng.h,
werewolf/c/src/sha256.c, werewolf/c/src/deal_rng.c
```

It does not list a `b32.c`/`b32.h`/`mixrad.c`/`mixrad.h` shadow path under any product, and it does not list any shadow path at all under `uttt/`, `pickemup/`, `chuiniu/` or `tallybones/`.
That means if any of the three new products had done exactly what this investigation was commissioned to rule out - kept a local `b32.c` or `mixrad.c` next to the shared one - no test in the repo would have caught it.
The same file's product-name `PRODUCT` regex array (its first test) already covers `pickemup` and `chui ?niu` but has no entry for `tallybones` at all (`grep -n "tally\|Tally\|TALLYBONES"` on the file returns nothing), so a product name leaking into `shared/` from tallybones specifically would also go uncaught, independent of the primitives question.

All of the above tests currently pass (I ran `shared_headers_reachable_validation.test.ts` fresh in this pass: 5/5 green), which is expected and is exactly the point: they pass because their scope has not been extended, not because they checked the thing this task asked about and found it clean.

## Ranked list

**Lift now:**
- `check_of` (the 2-byte truncated-SHA-256 checksum): move it into `shared/c`, e.g. as a new pair `shared/c/msgcheck.{c,h}` (or as two more exports of `shared/c/sha256.h` if that reads better to whoever does the lift) with a signature like `void wire_checksum(const void *head, size_t hn, const void *body, size_t bn, uint8_t *out, size_t check_len)`.
  It is byte-for-byte identical across `uttt/c/src/uttt_msg.c:66`, `pickemup/c/src/pk_msg.c:250`, `chuiniu/c/src/cn_msg.c:243` and `tallybones/c/src/tb_msg.c:264`, it is a pure function with a trivial parameter surface, and each product already proves it round-trips correctly under corruption via its own passing message test (`cn_msg_test`: 284,070 corruptions probed; `pk_msg_test`/`tb_msg_test` equivalent).
  Each product's build system needs only a relative `#include "../../../shared/c/msgcheck.h"` in its own `*_msg.c` (never `-I`, per `shared_headers_reachable_validation.test.ts`) and the new `.c` file added to that product's Makefile `SRC`/`WASM_SRC` list the same way `mixrad.c` already is.
  Proof the source products are unchanged: run `make -C uttt/c run`, `make -C pickemup/c run`, `make -C chuiniu/c run`, `make -C tallybones/c run` before and after and require identical pass/fail counts (baseline captured in this pass: pickemup 869/0 bridge checks plus the numbers above, chuiniu 1737483+15759233+89178+9909/0, tallybones 6529+29319+16424832+344338+459317+458/0); the checksum is a wire encoding, so the stronger proof is byte-identical output, which the differential style used for foolish's base32 above (link both the old inline `check_of` and the new shared one against the same random head/body spans, assert equal output over a few thousand trials) would need to be written as part of the lift.
- Extend the guard-rail tests to match what has already shipped: add `'b32.h'` and `'mixrad.h'` to `SHARED_HEADERS` in `foolish/e2e/validation/shared_headers_reachable_validation.test.ts:46`; add `uttt/c`, `pickemup/c`, `chuiniu/c` and `tallybones/c` (with their own source-list variable name, `SRC`) to the `builds` array at line 149; add `b32.c`/`b32.h`/`mixrad.c`/`mixrad.h` shadow paths for every one of the five products to the `shadowed` array in `shared_is_shared_validation.test.ts`; add `tallybones` (and `\btally\b` guarded the way `\bwolf\b` is) to that file's `PRODUCT` regex array.
  This is not a primitives lift by itself, but it is the specific safety net this task's premise assumed exists and does not; every fact in this document's Verdict currently rests on a one-time grep, and this change would make it rest on CI instead.
  Low risk: it only widens test coverage, touches no product source, and the mutation check is trivial (temporarily reintroduce a bare `#include "b32.h"` or a `foolish/c/src/b32.c` stub and confirm the test goes red, then remove it).

**Lift later:**
- `foolish/c/src/replay.c`'s `replay_b32_encode`/`replay_b32_decode` (lines 1808-1848) into `shared/c/b32.c`, confirmed byte-identical to the shared codec by this pass's 2,000-trial differential test.
  This is a foolish behaviour change, not a path move (foolish does not use `shared/c/b32.c` today), so per `pickemup/docs/REUSE_AUDIT.md` section 1 it needs its own step with foolish's full proof suite, not a ride-along on the other lifts here.
  Proof commands: `make -C foolish/c build/replay_v6_test && ./foolish/c/build/replay_v6_test 40` (baseline this pass: 316,992 checks, 0 failed, 1 skipped) must match exactly before and after; because the browser reads this codec directly (`wasm_replay_b32_encode`/`wasm_replay_b32_decode`, `foolish/c/Makefile:957,1177`), the stronger proof is a byte-identical check over the exported wasm functions specifically, not just the native test binary, and the masking difference noted above (`& 0xfff` vs. an unmasked accumulator) should be re-verified at the 12-bit boundary with an adversarial input (`n` large enough that `value` would differ if the masking mattered) rather than trusted from the 2,000 random trials alone.
- Nothing else in this scope needs a later lift; `tb_code.c`'s `add_scaled` is genuinely single-product today (see below) and should only move if a fourth product turns out to need the same forward-derivation scheme tallybones does.

**Do not lift:**
- `tallybones/c/src/tb_code.c:27-37`'s `add_scaled` (byte-bignum `a += b * k`).
  It is not a reimplementation of `mixrad_mul_add`/`mixrad_div_mod` - it is a different operation, needed only because tallybones derives every die roll from a forward-tracked polynomial of its own move history (`tb_code.h`'s "the body is also the roll's input", T11/T14), a design no other product in this repo has.
  Neither pickemup nor chuiniu has anything like it.
- The larger `*_msg.c` "envelope header plus Rule P" family beyond `check_of` (the header layout, the sealed/unsealed tag comparison, the seat-resolution logic).
  This is real, large, and already tracked as its own, much higher-risk item: `pickemup/docs/REUSE_AUDIT.md` section 3.4 classifies it "(c), high risk" and `werewolf/COMMON.md` item 3 describes the "3-function body vtable" a shared version would need.
  It is out of this pass's low-level-primitives scope and should stay a separate, dedicated lift step.
- `foolish/c/src/replay.c:887`'s `code_varint`.
  It is part of foolish's own arithmetic-coding entropy coder, has no duplicate anywhere else in the repo, and is a different (more general, bit-level) coding scheme from the mixed-radix digit-per-move approach the three new products share, not a candidate for merging with either.
