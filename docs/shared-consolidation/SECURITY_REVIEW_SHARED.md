# Security review: shared/

Scope: everything under `shared/` on branch `shared-consolidation` as of 2026-09-27.
Threat model: every decoder in `shared/c` is fed bytes chosen by a hostile peer in the same group chat, and the same C runs as wasm in browsers and on a server edge.
Method: every claim below rests on a probe that was compiled and run, not only on reading.
Probes live in the session scratchpad under `secreview/` (paths given per section; `P` below stands for that directory), were built with `-fsanitize=address,undefined -fno-sanitize-recover=all`, again at `-O2`, and where the code is freestanding also as wasm32 with the shipping flags (`/opt/homebrew/opt/llvm/bin/clang` 22.1.8, `--target=wasm32 -Oz -nostdlib -ffreestanding -mbulk-memory -mno-nontrapping-fptoint`) and run under node 26.

No source file was changed.
A later instruction asked for fixes on a new `sc-secfix` worktree, but creating that worktree was refused by the permission system, so every fix below is a proposal with its regression test described, not a commit.

## Verdict

No memory-safety defect was found in any shared decoder on hostile input: b32, mixrad, sha256 and deal_rng survived 1 to 3 million randomized and boundary iterations each under ASan and UBSan, and the wasm32 builds (at -Oz, -O2 and without bulk memory) produce a digest over 200,000 mixed iterations identical to the native one.
The highest finding is **Medium**: the freestanding `snprintf` in `shared/c/wasm/libc.c` loops forever whenever a `%s` or `%d` conversion is truncated, including every call with `cap == 0`.
No shipped caller truncates today, so it is a latent denial of service in a shared primitive whose header promises real-snprintf behavior.
The second **Medium** is that `release_strings.sh` cannot see a Swift string literal of 15 UTF-8 bytes or fewer, which is every dev-file name in use, so its dev-file check gives false assurance; the real control (DevFlags compiled out of Release) was verified to hold.
The third **Medium** is supply chain: CI downloads and runs `apt.llvm.org/llvm.sh` as root with no pin or checksum, and extracts an unchecksummed binaryen tarball as root.
Everything else is Low or Informational.
The shared C is safe to keep shipping; the snprintf fix is one line and should land before any new caller formats peer-controlled text into a fixed buffer.

## 1. shared/c: b32, mixrad, sha256, deal_rng

### b32 (`shared/c/b32.c`, `b32.h`)

Probe `P/p_b32.c`, outputs `P/p_b32_asan.out` and `P/p_b32_o2.out`.
It round-trips random payloads of 0 to 299 bytes with the exact capacity and with capacity minus one, then feeds `b32_decode` arbitrary non-NUL byte strings of up to 400 bytes with capacities from -4 to 255.
ASan and UBSan: 2,000,000 round trips and 2,000,000 hostile decodes, no report; `-O2`: 3,000,000 of each, all consistent.

SAFE, verified:
- `b32_decode` never writes at or past `cap` (b32.c:18); a negative or zero `cap` returns -1 on the first output byte or 0 for text with no alphabet characters.
- `value` is masked to 12 bits at every step (b32.c:15, :30), so no shift overflows.
- `b32_encode` refuses `cap - 1` exactly and always NUL-terminates; `n == 0` writes an empty string; `NULL` text decodes to 0 bytes.
- A long run of stray characters costs linear time only.

Findings:
- **Informational**, b32.c:9-14: decoding is not canonical.
  Case, stray characters and nonzero trailing bits are all accepted (`"MY"`, `"MZ"` and `"m.y/"` all decode to the single byte 0x66).
  Every product's echo test compares the bubble TEXT (`url == staged || url == sent`), so a re-cased copy of my bubble is classified as an arrival rather than an echo; the decoded game is identical, so nothing is gained, but any future dedupe, cache key or hash must be taken over the decoded bytes, not the text.
- **Informational**, b32.h:19: `B32_LEN(n)` overflows `int` for `n > 268,435,455` (UBSan flagged `B32_LEN(300000000)`); no caller comes near it.

### mixrad (`shared/c/mixrad.c`, `mixrad.h`)

Probe `P/p_mixrad.c`, outputs `P/p_mixrad_asan.out`, `P/p_mixrad_o2.out`; contract edges in `P/p_mixrad_edge.c`, `P/p_mixrad_edge.out`.
Each iteration starts from the sentinel 1, multiplies in digits with bases drawn from {1, 2, 255, 256, 2^23-1, random below 2^23} until the buffer (1 to 64 bytes) is full, then divides every digit back out and checks the digit and the sentinel; a second walk divides arbitrary hostile bytes by arbitrary bases, including bases far outside the contract.
ASan and UBSan: 1,000,000 full round trips (every one hit the capacity refusal) and 1,000,000 hostile walks; `-O2`: 3,000,000 of each; no report, no mismatch.

SAFE, verified:
- `mixrad_mul_add` never writes at or past `cap` (mixrad.c:13), and refusing leaves nothing past the buffer.
- `mixrad_div_mod` on arbitrary bytes always returns a remainder below the base and never grows or underflows `*len`.
- Every consumer's decoder refuses a base of 0 or 1 before calling `mixrad_div_mod` (UTTT `uttt_code.c:77` and a constant base at :71, SHED `pk_code.c:30`, LIAR `cn_code.c:29`, BONES `tb_code.c:70`), so a hostile menu of size 0 never reaches the division.

Findings:
- **Low**, mixrad.c:25-26: a base of 0 divides by zero (UBSan: "division by zero"; SIGFPE natively, a wasm trap in a browser or edge worker).
  **Low**, mixrad.c:13-14: a negative `*len` passes the `*len >= cap` test and writes `v[-1]` (ASan: heap-buffer-overflow WRITE of size 1).
  Both are outside the documented contract and unreachable from today's callers; the fix is `if (base == 0) return 0;` in `mixrad_div_mod` and `if (*len < 0 || *len > cap) return 0;` at the top of `mixrad_mul_add`, which change no output for any in-contract input.
- **Informational**, mixrad.h:22: a base of 2^23 or more, or a digit not below its base, wraps silently (the probe shows `base 2^25, digit 2^32-1` returns success with a wrong number); it stays inside the buffer.

### sha256 (`shared/c/sha256.c`, `sha256.h`)

Probe `P/p_sha.c`, outputs `P/p_sha_asan.out`, `P/p_sha_o2.out`.

SAFE, verified:
- The FIPS 180-4 / NIST vectors pass: empty, "abc", the 448-bit and 896-bit messages, one million "a", and the NIST long message (the 64-byte pattern 16,777,216 times, 2^30 bytes = 2^33 bits, digest `50e72a0e...fcd055e`), which proves the length field is correct well past 2^32 bits.
- Every length 0 to 1100, then 1,000,000 (ASan) and 2,000,000 (`-O2`) random messages fed in random chunkings including zero-length updates, all agree with CommonCrypto `CC_SHA256`.
- The bit length is a `uint64_t` built from `(uint64_t)len * 8` (sha256.c:62), so a wasm32 `size_t` cannot truncate it.
- No constant-time claim is made, and none is needed: the inputs are public game data.

Findings: none.

### deal_rng (`shared/c/deal_rng.c`, `deal_rng.h`)

Probe `P/p_rng.c`, output `P/p_rng_asan.out`.

SAFE, verified:
- The block function matches RFC 8439 section 2.3.2, and `deal_rng_seed` / `deal_rng_seed_at(…, 1)` match RFC 8439 A.1 vectors 1 and 2 (`ade0b876`, `bee7079f`).
- `deal_rng_seed_at(B)` equals the sequential stream after `16*B` draws for B = 0..4095 (65,536 words), and across the 32-bit counter carry (block 2^32-1 followed by 2^32).
- `deal_rng_bounded` returns 0 for n = 0 and 1, never returns a value at or above n over 1,600,000 draws at n in {2, 3, 6, 7, 52, 2^31+1, 2^32-2, 2^32-1}, and is uniform (chi-square 64.3 at n = 52, df 51, and 3.1 at n = 6, df 5, over 10,000,000 draws each; the 0.001 critical values are 87.0 and 20.5).
- The header's claim that an observer of some outputs cannot run the generator backwards holds for the code as written: the probe inverts the bare 20-round permutation from one block and recovers the key, and shows that with the feed-forward at deal_rng.c:37 the same inversion does not reach the known constants when the key is unknown.
  Line 37 is therefore load-bearing, and the RFC vectors would catch its removal.

Findings:
- **Informational**, deal_rng.h:15-19: the reversibility argument is correct but, in the iMessage products, beside the point.
  The 32-byte seed travels in every bubble (LIAR `cn_msg.c:289` writes it; SHED `RULES_AND_KERNEL.md:946` and LIAR `DECISIONS.md` K13 say so), so any peer with a decoder computes every hand and die directly, no reversal needed.
  The products document this as casual trust ("grade B"); the shared header should say it too, so no future product reads "a player cannot recover other hands" as a property it gets for free.
- **Informational**, deal_rng.c:63: the 64-bit counter wraps to block 0 after 2^64 blocks (confirmed by seeking to 2^64-1); unreachable.
- Could not verify: that every product's seed comes from an OS CSPRNG, as deal_rng.h:19 says; seed generation lives in the products, outside `shared/`.

## 2. shared/c/wasm (the freestanding libc and libm)

Probes `P/p_wasm.c` and `P/p_wasm.mjs` (output `P/p_wasm.out`), `P/p_snprintf.c` (output `P/p_snprintf.out`), `P/p_snprintf_spec.c` (output `P/p_snprintf_spec.out`).
`p_wasm.c` links the four primitives with `libc.c` and `libm.c` into three wasm modules (the shipping flags at -Oz, the same at -O2, and -Oz with `-mno-bulk-memory` as a flag-less consumer would get) and a native build.

SAFE, verified:
- All three wasm builds and the native build produce the same SHA-256 digest (`24ac9781...4012db`) over 200,000 iterations of deal_rng draws, bounded draws, b32 round trips and mixrad round trips, with zero failures.
- `memcpy` and `memset` compile to one `memory.copy` / `memory.fill` behind a zero-length test, and do not recurse into themselves in any of the three builds (checked in the disassembly and at run time).
- Overlapping `memcpy` in wasm behaves exactly like `memmove` (20,000 random overlapping copies against a byte-at-a-time reference); zero-length copies and fills touch nothing.
- `memset` truncates `c` to a byte; `memcmp`, `strcmp` and `strncmp` compare as unsigned bytes and stop at `n` or NUL; `strlen("")` is 0.
- libm never traps on NaN, infinities, 1e308, 2^63, the smallest subnormal or the exp overflow and underflow edges: LLVM guards every float-to-int truncation with a range test under `-mno-nontrapping-fptoint`.
  Accuracy against JavaScript's `Math` over sweeps: sin 3.95e-13 absolute on [-100, 100], exp 3.94e-14 relative, log 8.88e-16 absolute.
- No shim takes a length from anywhere but its caller; nothing in `shared/c/wasm` parses input.

Findings:
- **Medium**, libc.c:60, :67, :74: `snprintf` loops forever once the buffer is full and a `%s` or `%d` still has characters to write.
  The `PUT(ch)` macro evaluates `ch` only inside `if (n + 1 < cap)`, and the callers pass `*s++` and `d[--k]`, so once the condition is false the pointer and the digit index never advance.
  `P/p_snprintf.c` compiles the shim natively under another name and runs each call in a child with a 2 s alarm: `(cap 0, "%s", "hello")`, `(4, "%s", "hello")`, `(4, "%d", 123456)` and `(8, "%d|%s", INT_MIN, "xy")` all hang at -O0, -O2 and -Oz; only literal format text truncates correctly (it passes `*f`, which the loop advances itself).
  In the wasm build the cap-0 `%s` case hangs node indefinitely (found when the first wasm probe timed out and bisected to that call).
  Reach today: UTTT's web module links this `snprintf`, and its only calls (`uttt_say.c:28`, `:40`) format an `int` into `char num[12]`, which always fits; CARDS links `libc.c` into two wasm modules and its analysis code carries its own `snprintf`.
  So no shipped path hangs, but the header (stdio.h:2-4, libc.c:53-54) promises the real function's behavior, and the first caller that formats a peer's name into a fixed buffer turns a long nickname into a frozen browser tab or a wedged edge request.
  Fix: `#define PUT(ch) do { char c_ = (char)(ch); if (n + 1 < cap) out[n] = c_; n++; } while (0)`, which changes no output for any call that did not hang.
  Regression test: `P/p_snprintf.c` as `shared/c/wasm/libc_test.c`, compiled natively with the shim renamed, run from a product's `make run` (UTTT already links `libc.c`); the Makefile line is for the orchestrator.
  Mutation check: the unfixed macro is itself the mutation, and the probe shows the hang on four of six cases.
- **Low**, libc.c:61-79: an unsupported conversion (`%u`, `%x`, `%c`, `%ld`) is skipped without consuming its argument, so every later conversion reads the wrong one.
  `(sizeof b, "%u|%d", 7u, 42)` yields `"|7"`; `("%c%s", 'A', "ok")` dereferences 0x41 as a string (ASan: SEGV natively; in wasm it silently reads linear memory at address 65 into the output).
  Fix: trap on an unknown conversion (`__builtin_trap()`), so misuse fails on the first test run instead of printing memory.
- **Informational**, libc.c:17-21: overlapping `memcpy` is `memmove` in wasm and undefined natively, so a kernel that passes overlapping spans would diverge between its native tests and the wasm build.
  No `memmove` is provided, so a call to it fails at link time rather than at run time, which is the safe way round.
- **Informational**, libm.c:25-30, :111: `sin(1e300)` returns 0 with the shipping flags and -1 without bulk memory, against a true -0.818, because the range reduction loses every bit for large arguments; `lroundf(NaN)` and `lroundf(3e9)` return `INT_MIN` with `-mno-nontrapping-fptoint` and 0 / `INT_MAX` without it.
  The header already says a kernel whose decisions depend on these would need a stricter libm; no finding beyond that.

## 3. shared/c/msg_stage

Probes: the shipped `msg_stage_test.c` (60 checks, 0 failed, output `P/msg_stage_test.out`), and `P/p_stage.c` / `P/p_stage2.c` (outputs `P/p_stage.out`, `P/p_stage2.out`), which drive the loop with random events carrying the current, previous, next or a random try number.
Two runs: 2,000,000 sequences of 60 events (120,000,000 events) biased toward landing, and 1,000,000 sequences of 200 events (200,000,000 events) biased toward silence and errors, which reached the door 188 times and the revert 952,120 times.

SAFE, verified, every invariant held on every event:
- The state is always one of the seven; `try_no` never goes back and moves only with an insert, and every insert leaves the stage WAITING on a new try.
- At most 12 inserts go out between the first try (or a door tap) and the next door tap: 1 first, 9 silence re-tries, 2 error re-tries, exactly the two budgets of INSERT_GATING.md never touching.
- LANDED and DONE are absorbing until a reset.
- A silence, error or `due` for a try that is not current changes nothing; only a yes from any try lands, as the header says.
- `ms_drawer_up` rejects NaN and non-positive heights.
- A peer's bubble cannot drive the loop at all: the loop's inputs are host timers, completions, transitions and taps.
  `ms_receive` classifies a received bubble only from the product's own `mine` and `staged` booleans, and every product computes them by exact text equality with its own staged or last-sent bubble (UTTT `MessagesViewController.swift:323`, SHED :215, LIAR :160, BONES :174).
  A peer can make my device call its bubble an echo only by sending byte-identical text: replaying my last sent bubble is ignored, which is correct, and pre-sending my exact staged move requires the move I had already chosen and gains nothing.

Findings:
- **Informational**, msg_stage.h:152-169: a yes that arrives after the third error has reverted the draft is dropped (try 1 silent, tries 2 to 4 error, then try 1 answers yes: the machine returns NONE in state DONE), so the field could hold a bubble the board has reverted.
  INSERT_GATING.md's evidence says a compact-drawer silence is a refusal that never answers later, so this needs the host to break its own model; worth one line in the header.
- **Informational**, msg_stage.h:126-130: try numbers restart at 1 on every stage, so a previous stage's late yes for its try 1 lands the new stage unless the host filters by its own generation, which the Swift face (InsertStaging.swift:32-34) says the host must.
  A generation counter inside `ms_stage` would give that rule one owner and a test.

## 4. shared/c/i18n/languages.h and shared/c/motion_ruler

- motion_ruler.h: every accessor bounds-checks its index; `P/p_ruler.c` called them 4,000,156 times over the whole range from -2,000,000 to 2,000,000 plus `INT_MIN` and `INT_MAX` under ASan and UBSan with no report (output `P/p_ruler.out`). SAFE.
- **Informational**, languages.h:77: `FS_LANGUAGES` is a bare array with no accessor, so any index a consumer takes from a bubble, a file or a locale must be bounds-checked by that consumer; a `fs_language(int)` that returns English for an out-of-range index would put the check in one place.

## 5. shared/swift

### PackedBytes.swift

Probe `P/pb/main.swift`, outputs `P/pb_fuzz.out`, `P/pb_huge.out`, `P/pb_neg.out`: 2,000,000 random buffers of 0 to 39 bytes, 12 random reads each (24,000,000 reads, 16,711,945 of them returning nil).

SAFE, verified: no read ever returns bytes past the end or moves the cursor past the end, and the writer refuses a blob over its prefix width without writing anything.
Swift's own bounds checks make every failure a trap, never memory corruption.

Findings:
- **Low**, PackedBytes.swift:101-112: a failed `blob`, `text` or `blob8` read has already consumed its length prefix, so the cursor moves by 1 or 2 even though the read returned nil (5,656,672 of the fuzz's nil reads did).
  The header (lines 9-13) promises "the whole record or nothing"; a caller that stops at the first nil is unaffected, but one that treats nil as "field absent" and reads on would misparse.
  Fix: save `at` before the prefix and restore it on failure.
- **Low**, PackedBytes.swift:97 and :67-68: `u8s(Int.max)` overflows `at + n` and a reader constructed with a negative `at` indexes `b[-1]`; both trap (exit 133, SIGTRAP).
  Neither is reachable from bytes, since every length the reader takes from the buffer is at most 65,535.

### MessagesKit

- DevFlags.swift: SAFE, verified.
  The whole file is inside `#if DEBUG || SOLO_TESTING` (line 27); no project, xcconfig or script in the repo defines `SOLO_TESTING` or adds any active compilation condition, so a Release build defines neither.
  Compiled without `-D DEBUG`, the file yields 0 `DevFlags` symbols and no `containerURL` reference; with it, 47 symbols (`P/df/`).
  A Release caller outside the guard cannot compile ("cannot find 'DevFlags' in scope"), and all five product callers (`UtttDev.swift`, `MessageDevBoard.swift`, `PickemupDev.swift`, `ChuiniuDev.swift`, `TallybonesDev.swift`) sit inside `#if DEBUG` or `#if DEBUG || SOLO_TESTING`.
- InsertStaging.swift: a direct mapping of the C machine; covered by section 3.
- SendHint, SendHintView, SendHintMetrics, CollapseSlide, MotionRuler: presentation only, no input from bubbles, files or defaults; CollapseSlide's record and all of MotionRuler (lines 29-230) are DEBUG-only. SAFE.

### Textures

SAFE, verified: compiled without `-D TEXTURE_BAKE`, the four texture files export 0 `renderCGImage` symbols (10 with the flag); only the palettes and canvas-size constants ship (`P/tex/`).

## 6. shared/tools, shared/scripts, shared/rig

Probes: a scratch copy of `release_strings.sh` and `rs_scan.c` (`P/rs/`, own tests 8/8 ok in `P/rs_selftest.out`), hostile bundles in `P/rsb/` (output `P/rs_bypass.out`, `P/rs_pipefail.out`), and a scratch copy of `ship.sh` under a fake root and a fake `HOME` (`P/shiproot/`, `P/fakehome/`, env files `P/hostile_source.env` and `P/hostile_py.env`).
The copies were used so that no build output landed in the worktree.

### release_strings.sh and rs_scan.c

What the scan does catch (confirmed): a long Swift literal naming `dev.ruler` or holding an em dash, an em dash in a binary plist, and a forbidden framework directory.

- **Medium**, rs_scan.c:45-88 with release_strings.sh:40-55: the scan reads raw bytes, and several ways text is stored never put the bytes in a row. Each case below is a bundle built for real and passed as clean:
  - a Swift literal of 15 UTF-8 bytes or fewer, such as `"dev.ruler"` or `"Go U+2014 now"`: Swift's small-string form builds it from instruction immediates, so the bytes never appear contiguously (`grep` finds neither in the binary).
    Every dev-file name in use (`dev.ruler`, `dev.seed`, `dev.anchors`) is this short, so the dev-file check cannot fire on a Swift leak at all;
  - an Objective-C `@"Tap U+2014 to play"` (stored as UTF-16 in `__ustring`);
  - a UTF-16 text resource;
  - anything inside `Assets.car` (skipped at release_strings.sh:46);
  - an em dash in the last three or four bytes of a file (the loop at rs_scan.c:52 stops at `i + 4 < n`);
  - `dev.` followed by an upper-case letter (rs_scan.c:28 accepts only lower case and digits);
  - a name built at run time (`"dev." + name`);
  - an English double dash `U+2014 U+2014` (skipped on purpose for Chinese at rs_scan.c:69-74).
  Security impact is bounded because the real control, DevFlags compiled out of Release, was verified in section 5; but the gate's comment (DevFlags.swift:7) names it as the check, and it is not one.
  Fix: make the dev-file check structural instead of textual.
  Fail the build if the Release binary exports or references any `DevFlags` symbol (`nm | grep DevFlags`), which catches every leak whatever the literal length.
  For the em-dash check, also scan for the UTF-16LE pair `14 20`, scan `.car` text through `assetutil --info`, and extend the loop to `i + 2 < n`.
- **Low**, release_strings.sh:63-64: under `set -o pipefail`, `otool -L | tail | grep -q` returns failure when `grep -q` exits on the first match and `tail` then takes SIGPIPE, so a forbidden framework is reported clean.
  With a stand-in `otool` the check fails from about 40 KB of load-command output (500 lines) upward and works at 4 KB; a real app binary lists about 3 KB, so today it works by margin.
  Fix: capture the output first (`L=$(otool -L "$f")`), then `grep -q` the variable, which is what ship.sh:267-268 already does for `codesign`.

### ship.sh

- **Low**, ship.sh:70 and devlogs.sh:31, devcap.sh:21: the product env file is `source`d, so it is shell code with the signing keychain password in the environment.
  `P/hostile_source.env` containing `touch …` ran under `--dry-run --build 5`, which the usage line (:11-12) says "touch nothing"; the dry run also creates `build/ship/<name>` (:141).
  The env files are committed, reviewed repository content, so this is no new trust boundary; the dry-run wording should say that the env file runs.
- **Low**, ship.sh:131-134, :167-185, :302-305: env values (`SHIP_APP_ID`, profile names, bundle ids, `SHIP_TEAM`) and `$PROFILES_DIR` are spliced into Python source that `asc()` then `exec`s (:119).
  `P/hostile_py.env` with a quote in `SHIP_APP_ID` wrote a file from the injected Python during `--dry-run`.
  The realistic harm is breakage, not attack: a profile named with an apostrophe breaks the store-profile step.
  Fix: pass values as `sys.argv` or environment variables and keep the Python text constant.
- **Low**, ship.sh:243-244: `security unlock-keychain -p "$SIGNING_KEYCHAIN_PASSWORD"` and `set-key-partition-list -k "$SIGNING_KEYCHAIN_PASSWORD"` put the password in argv, readable by any process on the Mac through `ps` for the life of each command; the keychain is also left unlocked when the script ends.
  Fix: lock the keychain in an `EXIT` trap; for the argv exposure, run the export on a dedicated build keychain whose password is not reused anywhere.
- **Informational**, ship.sh:210, :271: `grep -qvx "$SHIP_APP"` treats the app name as a regular expression, and the signature check is a prefix match on `TEAM.bundle`; both are sanity checks on our own archive.
- SAFE: API credentials never appear in output.
  The `.p8` path goes to `xcodebuild` and only the key id and issuer id go to `altool`; the script prints `altool`'s last five lines, which do not echo credentials, and failing API calls print status and response bodies, not headers.
  `BUILD` is validated as an integer (:136) before it reaches any Python text.

### asc/testflight.py

- SAFE: it holds no credentials, loads the client from `~/.appstoreconnect/asc.py` (or `ASC_PY`), sends only fixed paths with ids from the environment, and prints status codes and API bodies, never keys or tokens.
- Could not verify: how `asc.py` reads the `.p8`, builds and signs the ES256 JWT (header `kid`, `iss`, `aud: appstoreconnect-v1`, `exp` at most 20 minutes), and whether it logs or caches the token.
  It lives outside this repository and outside the review's scope, so it was not opened.

### ios_mac_tests.sh, devlogs.sh, devcap.sh, motion, textures, rig/lib, structgen, datagen

- **Informational**, ios_mac_tests.sh:128, :134: `eval "$PRE_CMD"` and `eval "$LIB_CMD"` run commands from the product's own script; same trust as `source` above.
- **Informational**, motion_take.sh:11-15, motion_grid.sh:11-16, devcap.sh:62: temporary files are created with `mktemp` (no predictable names) but are left behind when `set -e` stops the script early, and `devcap.sh` never removes its `mktemp -d` directory.
- SAFE: every other temporary file uses `mktemp` with an `EXIT` trap; `rig/lib/ax.py` calls `subprocess.run` with an argument list (no shell); `newbar.py` uses `ast.literal_eval`, not `eval`; no script in `shared/` disables TLS verification, and no script uses `curl`.
- structgen and datagen parse this repository's own headers through libclang on a developer machine or CI; they take no input from users or peers and were not fuzzed.

## 7. Supply chain

- **Medium**, shared/scripts/ci_llvm.sh:81-82: `sudo wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh` then `sudo bash /tmp/llvm.sh`, with no pinned revision or checksum, is `curl | sudo sh` in two steps.
  Anyone who controls that URL, or its hosting, runs code as root in the CI job that builds the wasm modules deployed to the server and the web.
  The packages it then installs are GPG-verified by apt; the script that adds the key is not.
  Fix: vendor the few lines `llvm.sh` performs (add the apt.llvm.org key by its pinned fingerprint and the one repository line) or pin the script by SHA-256.
- **Low**, ci_llvm.sh:98-100: the binaryen release tarball is pinned by version but not by checksum, and `sudo tar` extracts it into `/opt`; a replaced release asset would execute in CI.
  Fix: check a committed SHA-256 before extracting.
- **Low**, ci_llvm.sh:81, :98: fixed `/tmp` names, one written as root; harmless on single-tenant ephemeral runners, a symlink or swap race on a shared machine.
  Fix: `mktemp`.
- SAFE: every download is HTTPS with verification on (`wget` defaults, no `--no-check-certificate`); nothing else in `shared/` downloads anything; `brew install` appears only in error messages.

## What could not be verified, and why

- `~/.appstoreconnect/asc.py` (key loading, JWT construction, token logging): outside the repository and the scope.
- Seed generation in each product (deal_rng.h:19 says the OS CSPRNG): product code.
- Every CARDS wasm call site of `snprintf`: only the UTTT web module's calls were traced to a fixed-width integer; CARDS links `libc.c` into two modules and was not traced call by call.
- iOS device behavior of the insert loop: the review exercised the state machine, not ChatKit.
- The keychain password's exposure through `ps` was established from the command line the script builds, not observed during a real export.

## Findings

| Severity | Where | Needs | What happens | Fix |
| --- | --- | --- | --- | --- |
| Medium | shared/c/wasm/libc.c:60, :67, :74 | a caller whose `%s` or `%d` output does not fit (any `cap == 0` call); none shipped today | infinite loop: a hung browser tab or edge request | evaluate `ch` once in `PUT`; land `P/p_snprintf.c` as a test |
| Medium | shared/tools/release_strings/rs_scan.c:45-88, release_strings.sh:46 | a dev-file name or em dash in a Swift literal of 15 bytes or fewer, ObjC UTF-16, UTF-16 resources, `.car`, file tail, upper case | the Release gate passes it | check for `DevFlags` symbols; scan UTF-16 and `.car`; fix the loop bound |
| Medium | shared/scripts/ci_llvm.sh:81-82 | control of apt.llvm.org or its hosting | root code execution in the CI that builds shipped wasm | vendor the steps or pin by SHA-256 |
| Low | shared/tools/release_strings.sh:63-64 | more than about 40 KB of `otool -L` output | forbidden framework reported clean | capture, then grep |
| Low | shared/tools/ship/ship.sh:131-134, :167-185, :302-305 | a quote in an env value | Python injection or breakage | pass values as argv |
| Low | shared/tools/ship/ship.sh:70, :141 | edit the committed env file | runs even under `--dry-run`, which also writes `build/ship` | document; no new trust boundary |
| Low | shared/tools/ship/ship.sh:243-244 | a local process during export | keychain password visible in `ps`; keychain left unlocked | lock in an `EXIT` trap; dedicated keychain |
| Low | shared/scripts/ci_llvm.sh:98-100, :81 | a replaced release asset; a shared runner | CI code execution; `/tmp` race | checksum; `mktemp` |
| Low | shared/c/mixrad.c:25-26, :13-14 | a caller passing base 0 or `*len < 0` (all callers guard) | wasm trap or SIGFPE; one-byte write before `v` | reject both at entry |
| Low | shared/c/wasm/libc.c:61-79 | an unsupported conversion in a format | varargs misaligned; native SEGV; wasm reads memory into output | trap on unknown conversions |
| Low | shared/swift/PackedBytes.swift:101-112 | a caller reading on after a nil | cursor moved past a length prefix despite nil | restore `at` on failure |
| Low | shared/swift/PackedBytes.swift:97, :67-68 | a caller passing `Int.max` or a negative `at` | Swift trap | clamp or document |
| Informational | shared/c/deal_rng.h:15-19 | a peer with a decoder | seed is in every bubble, so hands are computable; the header implies otherwise | say so in the header |
| Informational | shared/c/b32.c:9-14 | any peer | many texts decode to the same bytes; text-keyed logic sees them as different | key on decoded bytes |
| Informational | shared/c/b32.h:19 | `n > 268,435,455` | `int` overflow in `B32_LEN` | compute in `size_t` |
| Informational | shared/c/msg_stage/msg_stage.h:152-169, :126-130 | a yes after revert; a stale stage's yes | drawer holds a bubble the board reverted; a new stage lands on an old yes | note in header; generation inside `ms_stage` |
| Informational | shared/c/wasm/libc.c:17-21, libm.c:25-30, :111 | overlapping `memcpy`; huge or NaN arguments | memmove in wasm, UB natively; build-dependent libm results, no traps | document |
| Informational | shared/c/i18n/languages.h:77 | an unchecked index in a consumer | out-of-bounds read | a bounds-checked accessor |
| Informational | shared/scripts/ios_mac_tests.sh:128, :134; motion_*.sh; devcap.sh:62 | product env; early exit | `eval` of product commands; temp files left behind | `EXIT` traps |

Beyond the findings: `sha256.c` (:4) and `deal_rng.c` (:4) point at known-answer tests that live in one product's suite, so `shared/` has no test of its own for its four primitives.
The probes above (`p_b32.c`, `p_mixrad.c`, `p_sha.c`, `p_rng.c`) are ready to become `shared/c/*_test.c` beside the files they cover, run by a product's `make run` the way UTTT runs `msg_stage_test`.
