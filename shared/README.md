# shared/

Code that more than one product in this repo builds, kept once.
A fix made here is a fix everywhere, which lasts only as long as nothing here names a product: `e2e/validation/shared_is_shared_validation.test.ts` refuses a product name anywhere under `shared/`.
So products are named below by what they are: CARDS is the card game (the sibling folder with `c/`, `ios/`, `sdk/` and `server/` in it, and the largest), UTTT is `uttt/`, THIRD is the paused third product, and SHED is the shedding card game still being built.
A path given for CARDS is inside that folder.

A product reaches a shared C header by a relative `#include` from its own file, never by an include path (`shared_headers_reachable_validation.test.ts`).
Swift reaches a header-only C module through its `module.modulemap` on `SWIFT_INCLUDE_PATHS`.

## C (`c/`)

| Path | What | Used by |
| --- | --- | --- |
| `c/sha256.{c,h}` | SHA-256 | CARDS, UTTT, THIRD |
| `c/deal_rng.{c,h}` | the deal's RNG | CARDS, THIRD |
| `c/b32.{c,h}` | base32 codes | UTTT |
| `c/i18n/languages.h` | the language registry: every language's code, endonym and direction, one row each | CARDS (the string generator reads it), UTTT (compiled into its kernel, which indexes its tables by it) |
| `c/motion_ruler/` | the debug ruler's palette and geometry (`CMotionRuler`), painted by both products and read by `tools/motion` | CARDS, UTTT |
| `c/msg_stage/` | when a Messages insert may go, what a silent one means, and whether a received bubble is my own echo (`CMsgStage`); `INSERT_GATING.md` is the evidence, `msg_stage_test.c` the test | UTTT (CARDS compiles the Swift face, its stage path does not call it) |

## Swift (`swift/`)

| Path | What | Used by |
| --- | --- | --- |
| `swift/PackedBytes.swift` | the fixed-layout byte reader | CARDS |
| `swift/MotionRuler.swift` | the debug ruler on UIKit and Core Animation layers (DEBUG only) | UTTT; CARDS compiles it and paints its own SwiftUI ruler from the same `CMotionRuler` |
| `swift/MessagesKit/DevFlags.swift` | the one DEBUG dev-file reader: App Group lookup once, `dev.*` files read fresh (compiled out of Release) | CARDS (`MessageDevBoard`), UTTT (`UtttDev`) |
| `swift/MessagesKit/InsertStaging.swift` | the Swift face of `c/msg_stage` | UTTT |
| `swift/MessagesKit/SendHint*.swift` | the staged-but-unsent arrow at Messages' Send (SwiftUI and UIKit views, one set of numbers) | CARDS (`SendHint`), UTTT (`SendHintView`, `SendHintMetrics`) |
| `swift/MessagesKit/CollapseSlide.swift` | the auto-collapse on Core Animation layers | UTTT |

## Tools (`tools/`)

| Path | What | Used by |
| --- | --- | --- |
| `tools/ship/ship.sh` | archive, export, check and upload an iMessage app to TestFlight; the product is its `ship.env` | CARDS (`ios/Tools/ship.env`), UTTT (`uttt/ios/Tools/ship.sh`) |
| `tools/release_strings.sh` | fail a Release `.app`/`.ipa` on `dev.*` names, em dashes and forbidden frameworks (C scanner in `release_strings/`); `ship.sh` runs it | CARDS, UTTT |
| `tools/asc/testflight.py` | TestFlight status, release to the external group; the product is its `asc.env` | CARDS (`ios/Tools/asc.env`), UTTT (`uttt/ios/Tools/asc.env`) |
| `tools/devlogs.sh` | Release dev-install on a phone, the `log collect` line, and a subsystem filter | CARDS, UTTT (via `ship.env`) |
| `tools/devcap/` | film a USB iPhone for the motion tool | CARDS, UTTT (via `ship.env`) |
| `tools/motion/` | the ruler finder and ride scorer; `README.md` says how to film and score a take | CARDS, UTTT |
| `tools/structgen/`, `tools/sgcommon/` | the C-layout-to-Swift/TS/Kotlin generator and its libclang driver | CARDS, THIRD |
| `tools/datagen/` | the translation-table generator | CARDS, THIRD |
| `tools/llvm.mk` | the one LLVM toolchain the wasm builds use | CARDS, THIRD |
| `tools/tighten/` | the showcase video cutter (`media/`) | CARDS |
| `tools/check_ui_doc.py` | the UI design doc checker | UTTT, SHED |

## Rig (`rig/lib/`)

The measurement half of the simulator rig: frame windows (`window.sh`), MSE and bars (`mse.py`, `msecmp.py`, `bars.py`, `avgbar.py`, `newbar.py`, `rate.py`, `traces.py`), squares (`squares.py`, `squareplot.py`), `motionplot.py` for `tools/motion` tables, and `ax.py`, the accessibility driver.
Used by CARDS (`ios/Tools/rig`) and UTTT (the same rig through `uttt/ios/Tools/rig.env`, and `motionplot.py` through `devcap`).
