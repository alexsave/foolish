# Build and test scaffolding - consolidation pass over five products

Investigated in the `shared-consolidation` worktree, read-only.
Products: foolish/c (the card game, shipped), uttt/c (near-shipped), pickemup/c, chuiniu/c, tallybones/c (three new products built in parallel).
werewolf/c is out of scope but cited for comparison where its own Makefile or CI answers the same question the other five answer.
This document covers build systems, test harnesses, codegen wiring, the i18n pipeline, CI lanes and the tools/scripts around them.
It does not re-diff `x_check.h` (pk_check.h / cn_check.h / tb_check.h), the C kernel sources or the wire primitives - other investigators cover those.
It does not redo `ios/scripts/mac_tests.sh`, `rig.env`, `ship.env`/`asc.env`, `project.yml`, entitlements or plists - `docs/shared-consolidation/IOS_MESSAGES_LAYER.md` already covers those at the same rigor this document aims for, and its findings are cited, not repeated.
Every claim below is a diff, a grep result, or a command output, with a file path and a line number where useful.
`make -C pickemup/c run`, `make -C chuiniu/c run` and `make -C tallybones/c run` were all run during this investigation and are green as of this commit (see the Proof section).

## Verdict

The three new products' `c/Makefile`s are not an accident of convergence: chuiniu's and tallybones' own first lines say "The shape is pickemup/c/Makefile's" (`chuiniu/c/Makefile:2`, `tallybones/c/Makefile:2`), and the structgen/datagen/build-layout/sync block (`GEN`, `STRUCTGEN_DIR`, `DATAGEN_DIR`, `sync =`, `build/layout/ios.hash`, `structgen:`, `datagen:`, `swift-smoke:`) is about 70 lines that differ between the three only by a mechanical prefix swap (`pk_`/`cn_`/`tb_`, `Pickemup`/`Chuiniu`/`Tallybones`) plus a few comment trims.
That block, `include ../../shared/tools/ios_xcframework.mk` and its `ios-lib:` caller, and the `WARN`/`CFLAGS`/`ASAN_FLAGS`/wasm-object-compile shape are the product-neutral part of these Makefiles.
`shared/tools/ios_xcframework.mk` is the proof this kind of lift already pays off: it is now included by four Makefiles (`uttt/c/Makefile:196`, `pickemup/c/Makefile:230`, `chuiniu/c/Makefile:150`, `tallybones/c/Makefile:170`), each calling `$(call IOS_XCFRAMEWORK,...)` with only the name/sources/headers arguments differing.

Underneath that convergence, five gaps stand out because each is independently provable and each is the kind of small drift the owner's "double Band-Aid" and "flag-guard" rules exist to catch before it compounds:

1. **`shared/README.md`'s Tools table is stale.** Its `tools/structgen/`, `tools/sgcommon/`, `tools/datagen/` and `tools/llvm.mk` rows (`shared/README.md:46-48`) list "Used by CARDS, THIRD, SHED" (foolish, werewolf, pickemup). Neither chuiniu nor tallybones appears, though both genuinely call these tools (`chuiniu/c/Makefile:96-107`, `tallybones/c/Makefile:106-117`, confirmed by running `make -C tallybones/c structgen datagen` during this investigation, both green). And THIRD (werewolf, the "paused third product" per `shared/README.md:5`) is listed as a consumer though `grep -n "structgen\|datagen\|llvm.mk" werewolf/c/Makefile` returns nothing - werewolf hand-writes its own bindings (`werewolf/docs/CODEGEN_ALTERNATIVES.md:1`: "why this repo generates its own bindings" is about a prototype that explicitly is not the product's own path; werewolf's `sdk/swift/Kernel.swift` is committed source, not a structgen output). The table is wrong in both directions on the same three rows.
2. **`tallybones` has no product-name guard.** `foolish/e2e/validation/shared_is_shared_validation.test.ts`'s `PRODUCT` array (lines 54-73) has `/pickemup/i` and `/pick ?'?em ?up/i` (lines 63-64) and `/chui ?niu/i` (line 66), but no pattern for `tallybones` or `tally ?bones` anywhere in the file (`grep -in "tallybones\|tally" foolish/e2e/validation/shared_is_shared_validation.test.ts` returns nothing). Nothing today stops the word "tallybones" or "Tally Bones" from landing in a comment under `shared/`.
3. **`tallybones.yml`'s own header documents a gap that has already closed but the workflow was not updated.** `.github/workflows/tallybones.yml:14-16` says "KERNEL: once tallybones/c has `structgen` and `datagen` targets ..., copy pickemup.yml's `generated` job here with the Tallybones file names" - but `tallybones/c/Makefile:118-131` already has both targets, and `make -C tallybones/c structgen datagen` runs clean (confirmed above). `pickemup.yml` and `chuiniu.yml` both have a `generated` job that builds the toolchain and asserts the generated files exist; `tallybones.yml` has only a `tests` job. Nothing in CI proves tallybones' structgen/datagen path stays green.
4. **The `wasm-cc-check` safety net has not propagated.** `foolish/c/Makefile:528-556` refuses to build wasm with Apple clang (a silently different, larger, non-reproducible output) and explains why in a comment citing exactly this drift. `pickemup/c/Makefile:89`, `chuiniu/c/Makefile:68` and `tallybones/c/Makefile:66` all define `WASM_CC ?= clang` with no such check - the same weaker fallback `pickemup/docs/REUSE_AUDIT.md` section 7 already flagged for uttt ("That fallback can silently use Apple clang, which is the drift foolish's check was written for") is present, unaddressed, in all three new products.
5. **Warning flags diverged silently.** `pickemup/c/Makefile:18` defines one `WARN` variable (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -std=c11`, DECISION D58, "proven clean under Apple clang and under Linux gcc 13") and reuses it in `CFLAGS`, `ASAN_FLAGS`, the wasm compile and `IOS_CFLAGS` (5 sites, `pickemup/c/Makefile:18,19,64,95,110,128`). `chuiniu/c/Makefile` and `tallybones/c/Makefile` each spell the weaker `-Wall -Wextra -Werror -std=c11` out literally 4 times (`chuiniu/c/Makefile:14,50,73,84`; `tallybones/c/Makefile:15,49,72,85`) with no `-Wpedantic`/`-Wshadow`/`-Wconversion`, and neither product's `docs/DECISIONS.md` mentions D58 or records a decision to use a smaller warning set (`grep -n "Wconversion\|Wshadow\|Wpedantic\|D58" chuiniu/docs/DECISIONS.md tallybones/docs/DECISIONS.md` returns nothing) - this reads as drift, not a recorded choice.

Below those five, the test-harness section has its own finding worth separating out: mutation-checking is a real, disciplined practice in this repo (`c/tests/MUTATIONS.md` and `ios/TESTS_MUTATED.md` exist for all three new products, each with dozens of rows), but the *process* of doing a mutation check is hand-rolled independently three times at the C level with no shared script anywhere in the repo, while at the iOS level a real, reusable script (`~/.claude/skills/ios-sim-verify/mutation_check.sh`) exists and pickemup names it (`pickemup/ios/TESTS_MUTATED.md:2`) but chuiniu and tallybones do not (`chuiniu/ios/TESTS_MUTATED.md:2`: "a throwaway driver"; `tallybones/ios/TESTS_MUTATED.md:2`: "a script") - two products re-invented what a third had already adopted.

## 1. Makefiles

### 1.1 Shape and size

| Product | `c/Makefile` lines | `run`/`tests` target name | ASan lane | wasm lane | structgen/datagen | ios-lib via `ios_xcframework.mk` | native-vs-wasm cross-check |
| --- | --- | --- | --- | --- | --- | --- | --- |
| foolish | 1906 | `tests` (`:155`) | `tests-asan` (`:173`) | yes, extensive (`wasm-msg`, `wasm-bots`, `wasm-oracle`, `wasm-oracle-mt`) | yes, own layout-stamp machinery `foolish/c/Makefile` originates | yes (`:340-353`, two libraries) | not applicable (foolish's wasm IS what ships; the whole `wasm-*` family is the proof) |
| werewolf | not diffed here (out of scope) | `tests` (`:59`) | `tests-asan` (`:68`) | no dedicated freestanding-object target found | none (hand-written bindings, `werewolf/docs/CODEGEN_ALTERNATIVES.md`) | its own `ios-lib:` (`:99`), not the shared `.mk` | no |
| uttt | 255 | `run` (`:57`) | `asan` (`:173`, inline per-test, not `build/asan_%`) | `wasm-web` only, English-only replay renderer (`:233-254`) | none (`grep -n "structgen\|datagen" uttt/c/Makefile` is empty) | yes (`:196-201`, one library) | no |
| pickemup | 242 | `run` (`:51`) | `asan` (`:72`) | `wasm` objects (`:90-97`), plus a real link-and-run `cross` target (`:100-117`) | yes (`:115-166`) | yes (`:230-234`) | **yes** - `build/pk_cross.wasm`, run against native via `tests/pk_cross.mjs` |
| chuiniu | 171 | `run` (`:39`) | `asan` (`:55`) | `wasm` objects only (`:65-75`) | yes (`:96-107`) | yes (`:150-154`) | no |
| tallybones | 205 | `run` (`:39`) | `asan` (`:54`) | `wasm` objects only (`:65-75`) | yes (`:106-117`), plus `ios-lib-catalyst` (`:180-192`, unique to tallybones) | yes (`:170-174`) | no |

`run`/`asan` (uttt, pickemup, chuiniu, tallybones - 4 of 6) is now the majority target-name shape, not `tests`/`tests-asan` (foolish, werewolf - 2 of 6).
This is the opposite of what `pickemup/docs/REUSE_AUDIT.md` section 7 recommended before pickemup was built ("Pickemup should use `tests` / `tests-asan`, matching the majority and `werewolf.yml`") - the recommendation was written when foolish and werewolf were the only two data points and reads as "the majority" from two out of two; once uttt (already `run`/`asan`) is counted, the actual majority went the other way, and chuiniu and tallybones then followed pickemup's shape rather than the audit's advice.
Neither is wrong on its own; the two families simply exist side by side with nothing that reconciles them, and a CI script that shells out `make -C <product>/c tests` (as werewolf's does) would silently no-op on the other four.

### 1.2 The liftable block, quantified

Diffing `pickemup/c/Makefile`, `chuiniu/c/Makefile` and `tallybones/c/Makefile` after normalizing every product prefix (`pk_`/`cn_`/`tb_`, `PK_`/`CN_`/`TB_`, `Pickemup`/`Chuiniu`/`Tallybones`) to one placeholder:

```
pk vs cn normalized diff:  189 of 242/171 lines differ
cn vs tb normalized diff:  138 of 171/205 lines differ
pk vs tb normalized diff:  181 of 242/205 lines differ
```

Most of that surviving diff is genuinely per-product (`SRC`, `TESTS`, `FUZZ_GAMES`/`WIRE_GAMES` defaults, the header banner's `make -C <product>/c ...` usage lines, per-product comment prose).
The block that is NOT per-product - `GEN`, `STRUCTGEN_DIR`, `DATAGEN_DIR`, `STRUCTGEN`, `DATAGEN`, `SG_SPEC`, `SG_ARGS`, `IOS_LAYOUT_TRIPLE(S)`, the `$(STRUCTGEN)`/`$(DATAGEN)` build rules, `sync =`, `build/layout/ios.hash:`, `structgen:`, `datagen:`, `SWIFT_TRIPLE`, `swift-smoke:` - is roughly `pickemup/c/Makefile:115-166` (52 lines), and a line-by-line read of the normalized diff over that range shows only prefix substitution and comment-length differences (tallybones keeps fuller comments; chuiniu's are trimmed), never a logic change.
`include ../../shared/tools/ios_xcframework.mk` plus the `ios-lib:` stanza (`pickemup/c/Makefile:230-234`, `chuiniu/c/Makefile:150-154`, `tallybones/c/Makefile:170-174`) is already lifted and is the model for how the rest of the block should move: a `$(call ...)` with product-supplied arguments, not a copied recipe.

### 1.3 wasm flags: duplicated as flags, not as an export list

The task brief asks whether the wasm export list or link flags are duplicated verbatim.
Answer: the **flags** around a freestanding compile are duplicated verbatim; the **export lists** are not, because pickemup/chuiniu/tallybones do not link a shipped wasm module yet (their `wasm:` targets produce `build/wasm/*.o` objects only, never linked - `pickemup/c/Makefile:90-97`, `chuiniu/c/Makefile:69-75`, `tallybones/c/Makefile:69-75`).

The recurring flag string is:

```
--target=wasm32 [-Oz|-O2] -nostdlib -ffreestanding [-mbulk-memory] ... -isystem <shared>/wasm/include
```

It appears, byte-for-byte on the shared parts, in:
- `pickemup/c/Makefile:95` (object compile) and `:110` (the `cross` link, which does add real exports: `-Wl,--export=pk_cross_run -Wl,--export=pk_cross_hashes -Wl,--export-memory`, `:112`)
- `chuiniu/c/Makefile:73`
- `tallybones/c/Makefile:72`
- `uttt/c/Makefile:246-249` (the web replay link, its own export set: `-Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=131072 -Wl,--strip-all`)
- `foolish/c/Makefile:692-701` (`WASM_FLAGS`, foolish's own base, with a larger stack and `-Wl,--stack-first`)

So: no single export list is copied between products (each product exports its own function names, which is inherently per-product), but the freestanding-compile flag incantation is copied four times with no shared variable, which is exactly the shape `shared/tools/ios_xcframework.mk` already solved for the Xcode side - a `shared/tools/wasm_object.mk` defining `WASM_OBJ_FLAGS` (and ideally wrapping `wasm-cc-check`, finding 4 above) would remove it for the object-only case (pickemup/chuiniu/tallybones' `wasm:` targets) without touching foolish's or uttt's much larger, product-specific linking recipes.

### 1.4 The bot/arena Makefiles are NOT a shared shape

`chuiniu/c/bot/Makefile` (56 lines) and `tallybones/c/bot/Makefile` (52 lines) share the CC/CFLAGS/ASAN_FLAGS boilerplate pattern every other Makefile in this repo does, but their targets are genuinely different tools: chuiniu's is an arena (`bot-test`, `asan`, `arena`, `arena-fast`, each running `cn_arena --seats=N --mix=...`) while tallybones' is an exact solver (`run`, `solve`, `asan`, running `tb_solve play 200000`).
Pickemup has no separate `c/bot/Makefile` at all - its bot (`src/pk_bot.c`, `src/pk_belief.c`) is compiled directly into `pk_bot_test` and `pk_arena` from the main `c/Makefile` (`pickemup/c/Makefile:12-14,44-46,228`), a third shape.
Three products, three different ways of wiring a bot into the build: nothing here is a copy of anything else, and nothing here should be lifted - the CC/CFLAGS repetition is the same four-line boilerplate every Makefile in the repo carries (see 1.2/1.3), not bot-specific duplication.

### 1.5 A stray `.gitignore` duplicate

`chuiniu/ios/Generated/` is ignored twice: once in the root `.gitignore` (the line `chuiniu/ios/Generated/`, in the block that also lists `pickemup/ios/Generated/` and `tallybones/ios/Generated/`) and again in `chuiniu/ios/.gitignore:5` (`/Generated/`).
`git check-ignore -v pickemup/ios/Generated/foo.swift tallybones/ios/Generated/foo.swift chuiniu/ios/Generated/foo.swift` shows pickemup and tallybones resolve through the root file only; chuiniu resolves through its own nested file, so the root entry for chuiniu is now dead weight (harmless, but it is the exact "double Band-Aid" shape: two rules doing one job, no longer with one owner).

## 2. Test harness shape

### 2.1 MUTATIONS.md - a real, repeated ledger with no shared runner

| File | Lines | Process described |
| --- | --- | --- |
| `pickemup/c/tests/MUTATIONS.md` | 400 | "applied alone, with `make -i run` (every test binary, so an early red binary does not hide a later one) ... restored byte for byte" |
| `chuiniu/c/tests/MUTATIONS.md` | 112 | "applied alone by hand with an editor ... Before every run the test binary was deleted so make could not reuse a same-second build" |
| `tallybones/c/tests/MUTATIONS.md` | 102 | "applied alone by a script that kept its own copy of the file, deleted the one test binary so make could not reuse a same-second build" |
| `chuiniu/c/bot/MUTATIONS.md` | 19 | "run with `make bot-test`" |
| `tallybones/c/bot/MUTATIONS.md` | 22 | "run with `build/tb_bot_test 200000`, then restored (the file compared byte-for-byte with a saved copy afterwards)" |

No `pickemup/c/bot/MUTATIONS.md` exists, because (1.4) pickemup has no separate bot Makefile - its bot mutation rows are folded into `pickemup/c/tests/MUTATIONS.md`'s `pk_bot_test.c` section.

All three kernel-level ledgers independently describe the same "same-second rebuild" trap `foolish/c/Makefile:800` already has a named comment for ("The same-second trap: make compares whole-second mtimes...").
None of the three MUTATIONS.md files cites that comment or a shared script; each re-derives its own mitigation (pickemup: `make -i run` so one red binary does not mask the next; chuiniu: manual `rm` of the one test binary before each run; tallybones: "a script that kept its own copy of the file" - not found committed anywhere in the repo).
`grep -rl "mutation" foolish/scripts shared/tools shared/scripts` finds nothing - there is no runnable C-level mutation-check script anywhere in this repo, at any of `foolish/scripts`, `shared/tools`, `shared/scripts`, or under any product's `c/tools`.
`find pickemup chuiniu tallybones -iname "*mutat*"` (excluding the ledgers themselves) returns only `ios/TESTS_MUTATED.md` files - no script.

### 2.2 TESTS_MUTATED.md (iOS/Swift level) - the same story, plus a real fix one product missed

| File | Lines | Names a runnable tool? |
| --- | --- | --- |
| `pickemup/ios/TESTS_MUTATED.md` | 140 | yes - "the ios-sim-verify `mutation_check.sh`" (`:2`) |
| `chuiniu/ios/TESTS_MUTATED.md` | 67 | no - "a throwaway driver" (`:2`) |
| `tallybones/ios/TESTS_MUTATED.md` | 32 | no - "a script" (`:2`) |

`~/.claude/skills/ios-sim-verify/mutation_check.sh` exists and is exactly the tool pickemup names.
It is a user-level skill script, not a repo-tracked file under `foolish/scripts` or `shared/tools`, so it cannot be "included" by a Makefile the way `ios_xcframework.mk` is - but chuiniu and tallybones each independently hand-rolled their own equivalent instead of reusing the one pickemup had already proven, which is the same "two products re-invent what a third already adopted" pattern as finding 4/5 in the Verdict, one level up the stack (Swift instead of C).

### 2.3 `*_check.h` and fuzz drivers - product-specific, correctly so

`pk_check.h` (180 lines), `cn_check.h` (100 lines) and `tb_check.h` (144 lines) exist in all three products; `diff pickemup/c/tests/pk_check.h chuiniu/c/tests/cn_check.h` is 180 of 180/100 lines - this is the other investigator's territory per the task brief, not re-diffed here beyond confirming the files exist and that uttt has no equivalent (`uttt/c/tests/` has no `*_check.h`; its tests inline `printf`/`exit(1)` directly, e.g. `uttt/c/tests/uttt_test.c`, an older and different pattern that predates the pickemup-derived family).
The fuzz drivers (`pk_fuzz.c` 110 lines, `cn_fuzz.c` 94 lines, `tb_fuzz.c` 111 lines) each assert a different set of game invariants (card conservation, turn-boundary rules, replay-equals-apply, termination) that are inherently per-game logic - nothing here is boilerplate duplication, and nothing here should be lifted.

## 3. Codegen (structgen / datagen)

### 3.1 Wiring is copy-with-prefix-swap, already noted in section 1.2

`pickemup/c/Makefile:118-166`, `chuiniu/c/Makefile:98-148` and `tallybones/c/Makefile:108-156` call `structgen` and `datagen` with the identical argument shape:

```
$(DATAGEN) --cwd i18n --header keys.h --table <PFX>_KEY_NAME --require-complete \
  --name <Product>StringKeys --swift build/i18n/<Product>StringKeys.swift \
  && $(call sync,build/i18n/<Product>StringKeys.swift,$(GEN)/i18n/<Product>StringKeys.swift)
```

with `<PFX>` and `<Product>` the only things that change (`pickemup/c/Makefile:144-149`, `chuiniu/c/Makefile:126-131`, `tallybones/c/Makefile:134-139`).
A `PRODUCT_PREFIX`/`PRODUCT_NAME`-parameterized `.mk`, called the way `ios_xcframework.mk` already is, would remove this copy the same way it removed the xcframework recipe's.

### 3.2 "Generated code is a build artifact" - followed identically, three times over

Every product's Makefile comment and `.gitignore` entry agrees: generated Swift is never committed.
`pickemup/c/Makefile:142`: "A BUILD OUTPUT, never committed (../ios/Generated is ignored) ... exactly as foolish's sdk/swift/gen is."
`chuiniu/c/Makefile:98`: "A BUILD OUTPUT, never committed."
`tallybones/c/Makefile:99`: same wording as pickemup's.
The root `.gitignore` has one block per product (`pickemup/ios/Generated/`, `chuiniu/ios/Generated/`, `tallybones/ios/Generated/`, plus `*/ios/vendor/` and `*/ios/*.xcodeproj` beside each) - confirmed working for pickemup and tallybones via `git check-ignore -v`; chuiniu instead resolves through its own `chuiniu/ios/.gitignore` (section 1.5).
`shared/tools/structgen/build/` and `shared/tools/datagen/build/` are also gitignored (root `.gitignore`, the "shared toolchain's own build outputs" block) - confirmed no stray files after running `make -C pickemup/c structgen datagen` and `make -C tallybones/c structgen datagen` during this investigation (`git status --short` shows nothing from either run).

### 3.3 No literal "CONTENTS" file; the say-test validator is the real duplicated tool

No product has a file named `CONTENTS`; datagen instead reads a product's `i18n/keys.h` and `i18n/strings_en.c` directly as "the contents" (the header names, not a manifest file).
What IS a duplicated tool, not yet lifted: the string-table validator each product's `*_say_test.c` hand-rolls.
`pk_say_test.c` (299 lines), `cn_say_test.c` (223 lines) and `tb_say_test.c` (235 lines) each independently implement: a hole check, a width check (`pk_text_cols(s) <= pk_key_max(k)`), a "no em dash / en dash" byte check (`!strstr(s, "\xe2\x80\x94") && !strstr(s, "\xe2\x80\x93")`, `pickemup/c/tests/pk_say_test.c:65`), a "no final full stop" check, a placeholder-name allowlist (`KNOWN[]`), and a case-insensitive protected-word scan (`has_word_ci`, checking against trademark words like "uno"/"mattel" in pickemup's case).
`diff pickemup/c/tests/pk_say_test.c chuiniu/c/tests/cn_say_test.c` is 456 of 299/223 lines and `diff chuiniu/c/tests/cn_say_test.c tallybones/c/tests/tb_say_test.c` is 390 of 223/235 lines - large diffs because the actual string content and key names differ, but the four helper functions (`known`, `has_word_ci`, the dash/full-stop/placeholder scan loop in `t_table`) are the same logic in all three, parameterized only by each product's `KNOWN[]` array and protected-word list.
This is a real "(c) liftable after parameterising" case in REUSE_AUDIT's own taxonomy: a shared `shared/c/i18n/say_check.h`-style header taking a product's `KNOWN[]`, protected-word list and key table would remove roughly 60-80 lines of duplicated validator logic per product while leaving each product's own strings and widths where they are.

## 4. i18n

### 4.1 The three new products are English-only and do not touch `shared/c/i18n/languages.h`

| Product | Languages shipped | Uses `shared/c/i18n/languages.h`? |
| --- | --- | --- |
| foolish | 25 (`foolish/c/i18n/strings_*.c`, one file per language) | not applicable - foolish's generator reads it (`shared/README.md:20`) |
| uttt | 25, named explicitly in `uttt/c/Makefile:27` (`LANGS := en ru ko zh vi es pt fr de it ja pl uk tr id th nl sv da no fi cs ro he ar`) | yes - compiled into its kernel, indexed by `FsLang` |
| werewolf | 25 (`werewolf/c/i18n/strings_*.c`) | no - has its own `werewolf/c/i18n/languages.h`, a separate copy, not the shared one |
| pickemup | 1 (English only - `pickemup/c/i18n/` has only `keys.h` and `strings_en.c`) | no - `grep -rln "languages.h\|FS_L_" pickemup/c chuiniu/c tallybones/c` returns nothing |
| chuiniu | 1 (English only) | no |
| tallybones | 1 (English only) | no |

This is a gap to record, not to fix here: none of the three new products has reached translation yet, so `shared/c/i18n/languages.h` genuinely has nothing to offer them today.
`shared/README.md:20`'s "Used by" column (CARDS, UTTT) is accurate as far as it goes, but is silent on the three new products because they are simply not consumers yet - unlike the structgen/datagen rows (Verdict finding 1), this is not stale documentation, it is an honest gap.

### 4.2 `strings_en.c` / `keys.h` shape is identical across the three

`pickemup/c/i18n/keys.h` (181 lines), `chuiniu/c/i18n/keys.h` (128 lines), `tallybones/c/i18n/keys.h` (153 lines) - `diff pickemup/c/i18n/keys.h chuiniu/c/i18n/keys.h` is 257 of 181/128 lines, entirely content (different key names for a different game), with the same file-header comment shape ("EVERY STRING THE GAME SAYS, by name, with the room each one has").
This is the expected, non-liftable half of i18n: the key set is the game.

## 5. CI

### 5.1 Per-product C lanes: pickemup.yml and chuiniu.yml are near-identical; tallybones.yml and uttt-c.yml are missing a job each

| Workflow | Lines | `tests` job | `generated` job (structgen/datagen) | Trigger paths |
| --- | --- | --- | --- | --- |
| `pickemup.yml` | 83 | `make -C c run` + `make -C c asan` | yes - toolchain, `make -C c structgen datagen`, then asserts `ios/Generated/PickemupKernel.swift` etc. exist and contain `SG_LAYOUT_HASH` | `pickemup/**`, `shared/**`, self |
| `chuiniu.yml` | 74 | `make -C c run` + `make -C c asan` | yes - same shape, `ChuiniuKernel.swift` | `chuiniu/**`, `shared/**`, self |
| `tallybones.yml` | 61 | `make -C c run` + `make -C c asan` | **no** - header comment says to add it once structgen/datagen exist; they already do (Verdict finding 3) | `tallybones/**`, `shared/**`, self |
| `uttt-c.yml` | 43 | `make -C c run` + `make -C c asan` + `make -C c ios-smoke` | not applicable - uttt has no structgen/datagen | `uttt/c/**`, `shared/c/**`, self |
| `werewolf.yml` | 65 | `make -C c tests` + `make -C c tests-asan` + `ios/scripts/release_gate.sh` | not applicable | `werewolf/**`, `shared/c/**`, self |

`chuiniu.yml`'s header comment says explicitly it is written "in pickemup.yml's shape" (`chuiniu.yml:2`).
`diff` of the two files' `generated:` jobs shows the only changes are `pickemup`→`chuiniu` in `working-directory`/comments and `PickemupKernel`/`PickemupStringKeys`/`PickemupStringsEn` → `ChuiniuKernel`/`ChuiniuStringKeys`/`ChuiniuStringsEn` in the four `test -s`/`grep` assertion lines - a mechanical copy, confirmed by direct read of both files during this investigation.
`tallybones.yml` never got that job copied over even though its own file explicitly says to do so once the Makefile targets exist, and they now do - this is CI coverage that should exist and currently does not: a structgen/datagen regression in tallybones (a bad header, a missing key) would only be caught locally or in an Xcode build, never in CI.

Every one of the three new products' workflows independently repeats the same three-line rationale for NOT running `make ... wasm` (citing `foolish/e2e/validation/ci_toolchain_validation.test.ts`'s assumption that any `make ... wasm` line in any workflow is a build of foolish's test module) - `pickemup.yml:18-21`, `chuiniu.yml:14-16`, `tallybones.yml:22-24`, worded near-identically.
This is a real, working guard against accidentally tripping a foolish-specific gate, correctly copied three times; it is a candidate for a one-line shared comment/doc reference rather than three independent restatements, but is low value to lift on its own.

### 5.2 `validate.yml` covers the shared-guard tests, gated on `shared/**`

`npm run test:validate` (`foolish/package.json:31`) runs `e2e/validation/*.test.ts`, which includes `shared_is_shared_validation.test.ts` and `shared_headers_reachable_validation.test.ts`.
`.github/workflows/validate.yml` triggers on `foolish/**` or `shared/**` (not on `pickemup/**`/`chuiniu/**`/`tallybones/**` alone) - correct, since the guard is about what lands under `shared/`, and a product-only change cannot violate it without also touching `shared/`.
Combined with finding 2 (no `tallybones` pattern in `PRODUCT`), this means the gate that exists and runs on every `shared/**` change would not catch "tallybones" appearing in a `shared/` file today, because the pattern list itself is missing that product.

### 5.3 No CI lane at all: coverage/metrics/memory/wasm workflows are foolish-only

`grep -n "pickemup\|chuiniu\|tallybones" .github/workflows/coverage.yml .github/workflows/validate.yml .github/workflows/metrics.yml .github/workflows/memory.yml` returns nothing.
None of the three new products has a wasm-build CI lane (none ships a browser replay yet, so `wasm.yml` correctly does not reach them) - recorded as a gap consistent with the products' current scope, not a defect.

## 6. Tools and scripts outside `IOS_MESSAGES_LAYER.md`'s coverage

`c/tools/` has content in only one of the three products: `pickemup/c/tools/pk_arena.c` (the arena driver, `arena:` target, `pickemup/c/Makefile:228-234`).
`chuiniu/c/tools/` and `tallybones/c/tools/` do not exist; chuiniu's and tallybones' equivalent tools live under `c/bot/` instead (section 1.4), a directory-placement difference, not a duplication.
`ios/Tools/` has only `rig.env` in all three products, already covered by `IOS_MESSAGES_LAYER.md`'s config-files section ("do not lift, per-product knob"); no `scripts/` directory exists at any of the three products' root.

`grep -rn "COPIED" pickemup chuiniu tallybones` (excluding `.swift` files, which `IOS_MESSAGES_LAYER.md` already covers) finds only:
- `chuiniu/ios/scripts/mac_tests.sh:2` and `tallybones/ios/scripts/mac_tests.sh:2` - both already covered by `IOS_MESSAGES_LAYER.md` item 10 ("lift-now for the wrapper conversion").
- `tallybones/c/src/tb_msg.h:4` ("PICK 'EM UP'S ENVELOPE, COPIED AND SHRUNK") - a C wire-format copy, the other investigator's territory.
- A cluster of `docs/*.md` references to the `COPIED from <path> at <commit>` convention itself (`pickemup/docs/ORCHESTRATION.md`, `REUSE_AUDIT.md`, `DECISIONS.md`, `IOS_DECISIONS.md`, `ios/README.md`; `tallybones/ios/README.md`) - process documentation, not files to lift.

Nothing new to report in this section beyond what `IOS_MESSAGES_LAYER.md` already covers.

## Proof

Baseline, run during this investigation (read-only: build outputs land in each product's gitignored `c/build/` and `ios/Generated/`, nothing tracked changed):

```
make -C pickemup/c run     -> green (pk_bot_test 107612 assertions 0 failed; bridge 869 checks 0 failed; full log tail confirms every binary passed)
make -C chuiniu/c run      -> green (cn_msg_test 89178 assertions 0 failed; ios_smoke 77 checks 0 failed)
make -C tallybones/c run   -> green (tb_beats_test 459317 assertions 0 failed; tb_api smoke 67 checks 0 failed)
LLVM_PREFIX=/opt/homebrew/opt/llvm make -C pickemup/c structgen datagen    -> wrote PickemupKernel.swift, PickemupStringKeys.swift, PickemupStringsEn.swift
LLVM_PREFIX=/opt/homebrew/opt/llvm make -C tallybones/c structgen datagen -> wrote TallybonesKernel.swift, TallybonesStringKeys.swift, TallybonesStringsEn.swift
git status --short   -> no change from any of the above (all outputs gitignored)
```

For any future Makefile refactor that moves the structgen/datagen block or the wasm-object block into a shared `.mk`, the byte-identical proof is:
`cmp` each product's `ios/Generated/<Product>Kernel.swift` before and after (the layout hash embedded in the file, e.g. `SG_LAYOUT_HASH`, must be unchanged), and for the wasm objects, compare `build/wasm/*.o` by content in a fixed directory rather than by filename - `wasm-ld` embeds the output basename, so two links compared by varying only the directory (never the filename) are the correct A/B, per the existing repo note on this trap.
`make -C <product>/c run` green, byte-for-byte, is the cheapest end-to-end proof that a Makefile refactor changed nothing observable.

## Ranked list

**Lift now:**
- Copy `pickemup.yml`'s `generated` job into `tallybones.yml` with the Tallybones file names, matching what `chuiniu.yml` already did. The Makefile targets it needs already exist and are proven green (Proof section); this is the lowest-risk item in the whole document because it only adds a CI job, changing no build behavior.
- Add `/tallybones/i` and `/tally ?bones/i` to `PRODUCT` in `foolish/e2e/validation/shared_is_shared_validation.test.ts`, the same way `/pickemup/i`/`/pick ?'?em ?up/i` and `/chui ?niu/i` were added for the other two new products (mirrors REUSE_AUDIT step S0, which pickemup already ran for itself).
- Add a `wasm-cc-check`-equivalent guard (or a shared `.mk` providing one) to pickemup's, chuiniu's and tallybones' `wasm:` targets, closing the Apple-clang silent-fallback gap `foolish/c/Makefile:528-556` exists to prevent and that REUSE_AUDIT section 7 already flagged for uttt.
- In `chuiniu/c/Makefile` and `tallybones/c/Makefile`, replace the four literal repetitions of `-Wall -Wextra -Werror -std=c11` with one `WARN`/`CFLAGS`-style variable, the way `pickemup/c/Makefile:18-19` already does. This alone is a same-product, zero-risk cleanup; whether to also adopt `-Wpedantic -Wshadow -Wconversion` (pickemup's D58) is a product decision for the owner, not implied by the cleanup itself.
- Update `shared/README.md`'s `tools/structgen/`, `tools/sgcommon/`, `tools/datagen/` and `tools/llvm.mk` rows to add chuiniu and tallybones (they are real consumers, confirmed by running their `structgen`/`datagen` targets) and to reconsider whether THIRD (werewolf) belongs in those rows at all, since werewolf hand-writes its own bindings rather than calling structgen/datagen.

**Lift later:**
- A `shared/tools/wasm_object.mk` (or an addition to `ios_xcframework.mk`'s file) providing a `WASM_OBJ_FLAGS` variable and the freestanding-object-compile loop, for pickemup/chuiniu/tallybones' `wasm:` targets. Proof needed: `build/wasm/*.o` byte-identical before and after, compared in a fixed directory (never by filename, per the wasm-ld basename trap).
- The structgen/datagen/build-layout/sync block (`pickemup/c/Makefile:115-166` and its chuiniu/tallybones equivalents, ~52 shared lines) as a `PRODUCT_PREFIX`-parameterized `.mk`, on the `ios_xcframework.mk` pattern. Proof needed: `cmp` every generated `.swift` file, and the `SG_LAYOUT_HASH` each embeds, before and after, for all three products.
- The `*_say_test.c` string-table validator (hole/width/dash/full-stop/placeholder/protected-word checks - `pickemup/c/tests/pk_say_test.c`'s `known`/`has_word_ci`/`t_table` shape) as a shared header taking a product's `KNOWN[]` and protected-word list. Needs its own design pass (what a product supplies vs. what stays fixed), not a path move.
- A committed C-level mutation-check script (there is none anywhere in the repo today), so `pickemup/c/tests/MUTATIONS.md`, `chuiniu/c/tests/MUTATIONS.md` and `tallybones/c/tests/MUTATIONS.md` stop each hand-describing their own version of the same-second-rebuild workaround. The iOS-level equivalent already exists as a user-level skill (`~/.claude/skills/ios-sim-verify/mutation_check.sh`); chuiniu and tallybones should at minimum start naming it in their `TESTS_MUTATED.md` files the way pickemup does, even before any C-level script exists.
- Decide and reconcile the `run`/`asan` vs. `tests`/`tests-asan` Makefile target-name split (4 products vs. 2) so that any future cross-product CI or tooling script does not have to special-case which name a given product answers to.

**Do not lift:**
- The bot/arena Makefiles (`chuiniu/c/bot/Makefile`, `tallybones/c/bot/Makefile`, pickemup's in-tree bot targets) - three genuinely different shapes (arena vs. exact solver vs. in-tree), confirmed by reading all three; only the ordinary CC/CFLAGS/ASAN_FLAGS boilerplate is shared, and that boilerplate is already covered by the WARN-variable recommendation above.
- The fuzz drivers (`pk_fuzz.c`, `cn_fuzz.c`, `tb_fuzz.c`) and the `*_check.h` families - each is the correct, product-specific expression of that product's own rules; no shared shape exists to lift, and `*_check.h` is explicitly another investigator's territory.
- `i18n/keys.h` / `strings_en.c` content - the key set is the game; only the validator logic around it (already listed under lift-later) is duplicated.
- Per-product CI trigger paths and working-directory blocks in `pickemup.yml`/`chuiniu.yml`/`tallybones.yml` - necessarily per-product (each names its own product's path), and the repeated `ci_toolchain_validation.test.ts` rationale comment is working documentation, not a defect worth the churn of consolidating into one place.
- The stray double `.gitignore` entry for `chuiniu/ios/Generated/` (section 1.5) - real, but trivial; worth a one-line cleanup whenever someone is already editing that file, not its own task.
