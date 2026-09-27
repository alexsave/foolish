# shared/

Code that more than one product in this repo builds, kept once.
A fix made here is a fix everywhere, which lasts only as long as nothing here names a product: `e2e/validation/shared_is_shared_validation.test.ts` refuses a product name anywhere under `shared/`.
So products are named below by what they are: CARDS is the card game (the sibling folder with `c/`, `ios/`, `sdk/` and `server/` in it, and the largest), UTTT is `uttt/`, THIRD is the paused third product, SHED is the shedding card game still being built, LIAR is the liar's-dice game, and BONES is the dice solitaire.
A path given for CARDS is inside that folder.

A product reaches a shared C header by a relative `#include` from its own file, never by an include path (`shared_headers_reachable_validation.test.ts`).
Swift reaches a header-only C module through its `module.modulemap` on `SWIFT_INCLUDE_PATHS`.

## C (`c/`)

| Path | What | Used by |
| --- | --- | --- |
| `c/sha256.{c,h}` | SHA-256 | CARDS, UTTT, THIRD, SHED, LIAR, BONES |
| `c/deal_rng.{c,h}` | the deal's RNG | CARDS, THIRD, SHED, LIAR, BONES |
| `c/b32.{c,h}` | base32 codes | UTTT, SHED, LIAR, BONES |
| `c/mixrad.{c,h}` | mixed-radix arithmetic on a byte bignum, the history-as-code body under the game coders | UTTT, SHED, LIAR, BONES |
| `c/wasm/` | the freestanding libc and libm a wasm32 build compiles against | CARDS, UTTT (both link `libc.c` and `libm.c`), SHED (its objects, and `libc.c` in its native-vs-wasm cross-check link), LIAR, BONES (headers only: their wasm build is objects only) |
| `c/text_util/` | the text under a say layer, every write into a fixed buffer with no `snprintf`: UTF-8 stepping (`text_next_cp`), the column table a string limit is checked against (`text_cp_cols`, `text_cols`), `text_itoa`, `text_put` and the `{placeholder}` filler `text_fill`; `text_util_test.c` the test | UTTT (the stepping, the copy and the filler; it keeps its own column table), SHED, LIAR, BONES |
| `c/wire_check/` | the envelope's wire check (`wire_check`): the first few bytes of SHA-256 over the head and the body, the bytes an envelope carries between the two; each product keeps its own length; `wire_check_test.c` the test | UTTT, SHED, LIAR, BONES |
| `c/stats/` | the arithmetic under a bot arena: a mean with its Bessel-corrected variance and standard error over running sums (`StatSums`, `stat_mean`, `stat_variance`, `stat_stderr`), the Wilson score interval for a win rate (`stat_wilson`, `STAT_Z95`), and in the header-only `seed_hash.h` the splitmix64 per-game seed (`seed_hash32`, `seed_hash32_from`, `seed_splitmix64`); in its own directory so the builds that wildcard `c/*.c` pick up neither it nor `stats_test.c`, the test | CARDS (the paired A/B evaluator's standard error), SHED (its arena's seeds and its tests' seeds), LIAR (its arena's seeds and win-rate interval, its tests' seeds), BONES (the solver's simulation statistics, its tests' seeds); UTTT runs the test (its arena keeps its own seed hash, which is not splitmix64) |
| `c/i18n/languages.h` | the language registry: every language's code, endonym and direction, one row each | CARDS (the string generator reads it), UTTT (compiled into its kernel, which indexes its tables by it) |
| `c/motion_ruler/` | the debug ruler's palette and geometry (`CMotionRuler`), painted by both products and read by `tools/motion` | CARDS, UTTT |
| `c/msg_stage/` | when a Messages insert may go, what a silent one means, and whether a received bubble is my own echo (`CMsgStage`); `INSERT_GATING.md` is the evidence, `msg_stage_test.c` the test | UTTT, SHED, LIAR, BONES (CARDS compiles the Swift face, its stage path does not call it) |
| `c/test/` | the kernel test harness, test-only and never shipped: `check.h` (`TEST`, `CHECK` with its per-test failure cap `CHECK_NAMED_CAP`, `report`) and `twophone.h` (`STEP`, `OK` and the quiet rehearsal of a two-phone script, on the same counters); a product's own test header includes `check.h` and adds what pokes its own game; `check_test.c` tests the harness | SHED, LIAR, BONES |

## Swift (`swift/`)

| Path | What | Used by |
| --- | --- | --- |
| `swift/PackedBytes.swift` | the fixed-layout byte reader | CARDS |
| `swift/MotionRuler.swift` | the debug ruler on UIKit and Core Animation layers (DEBUG only) | UTTT; CARDS compiles it and paints its own SwiftUI ruler from the same `CMotionRuler` |
| `swift/MessagesKit/DevFlags.swift` | the one DEBUG dev-file reader: App Group lookup once, `dev.*` files read fresh (compiled out of Release) | CARDS (`MessageDevBoard`), UTTT (`UtttDev`), SHED, LIAR, BONES |
| `swift/MessagesKit/InsertStaging.swift` | the Swift face of `c/msg_stage` | UTTT, SHED, LIAR, BONES |
| `swift/MessagesKit/SendHint*.swift` | the staged-but-unsent arrow at Messages' Send (SwiftUI and UIKit views, one set of numbers) | CARDS (`SendHint`), UTTT (`SendHintView`, `SendHintMetrics`), SHED (`SendHint`, `SendHintMetrics`) |
| `swift/MessagesKit/CollapseSlide.swift` | the auto-collapse on Core Animation layers | UTTT, SHED |
| `swift/Textures/` | the wool, felt, wood and fern-back generators and their palettes; CoreGraphics only, and the generator half compiles only under `-D TEXTURE_BAKE`, so a shipping target carries the palettes and resource names but never renders | CARDS (its texture loader reads the names, its `ios/Tools/regenerate_textures.sh` bakes into its own resources), SHED, LIAR, BONES (compile the palettes and names; their baked JPEGs are committed) |

## Tools (`tools/`)

| Path | What | Used by |
| --- | --- | --- |
| `tools/ship/ship.sh` | archive, export, check and upload an iMessage app to TestFlight; the product is its `ship.env` | CARDS (`ios/Tools/ship.env`), UTTT (`uttt/ios/Tools/ship.sh`) |
| `tools/release_strings.sh` | fail a Release `.app`/`.ipa` on `dev.*` names, em dashes and forbidden frameworks (C scanner in `release_strings/`); `ship.sh` runs it | CARDS, UTTT |
| `tools/asc/testflight.py` | TestFlight status, release to the external group; the product is its `asc.env` | CARDS (`ios/Tools/asc.env`), UTTT (`uttt/ios/Tools/asc.env`) |
| `tools/devlogs.sh` | Release dev-install on a phone, the `log collect` line, and a subsystem filter | CARDS, UTTT (via `ship.env`) |
| `tools/devcap/` | film a USB iPhone for the motion tool | CARDS, UTTT (via `ship.env`) |
| `tools/motion/` | the ruler finder and ride scorer; `README.md` says how to film and score a take | CARDS, UTTT |
| `tools/structgen/`, `tools/sgcommon/` | the C-layout-to-Swift/TS/Kotlin generator and its libclang driver | CARDS, SHED, LIAR, BONES (THIRD hand-writes its bindings) |
| `tools/datagen/` | the translation-table generator | CARDS, SHED, LIAR, BONES |
| `tools/llvm.mk` | the one LLVM toolchain the wasm builds use | CARDS, SHED, LIAR, BONES (through structgen and datagen) |
| `tools/wasm_cc.mk` | which clang compiles a kernel for wasm32 (the pinned Homebrew LLVM on a Mac) and `wasm-cc-check`, the guard that refuses Apple clang, which targets wasm32 but produces different bytes | SHED, LIAR, BONES (CARDS still carries its own copy of the same guard) |
| `tools/ios_xcframework.mk` | the `ios-lib` recipe: `$(call IOS_XCFRAMEWORK,name,sources,cflags,headers,min-ios,out)` builds the device and both simulator slices and wraps them in an xcframework | CARDS, UTTT, SHED, LIAR, BONES |
| `tools/tighten/` | the showcase video cutter (`media/`) | CARDS |
| `tools/textures/` | the bake tool: `regenerate_textures.sh <out-dir>` compiles `swift/Textures` with `GenerateTextures.swift` and writes the JPEGs; a product passes its own resources folder | CARDS, SHED |
| `tools/check_ui_doc.py` | the UI design doc checker | UTTT, SHED |

## Scripts (`scripts/`)

| Path | What | Used by |
| --- | --- | --- |
| `scripts/ios_mac_tests.sh` | the Mac-side iOS gate: xcframework, xcodegen with every tracked entitlements file put back by `cp -p` (bytes and mtime), then each test scheme and each build scheme; the product is the env its own `ios/scripts/mac_tests.sh` sets before it `exec`s this one | CARDS, UTTT, SHED |

## Rig (`rig/lib/`)

The measurement half of the simulator rig: frame windows (`window.sh`), MSE and bars (`mse.py`, `msecmp.py`, `bars.py`, `avgbar.py`, `newbar.py`, `rate.py`, `traces.py`), squares (`squares.py`, `squareplot.py`), `motionplot.py` for `tools/motion` tables, and `ax.py`, the accessibility driver.
Used by CARDS (`ios/Tools/rig`), UTTT (the same rig through `uttt/ios/Tools/rig.env`, and `motionplot.py` through `devcap`) and SHED, LIAR and BONES (the same rig, each through its own `ios/Tools/rig.env`).
