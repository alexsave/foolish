# FoolishKit code size

The iMessage app ships one big binary, `FoolishKit.framework/FoolishKit`, and at 1.2 build 75 it is 3.7 MB stripped.
This document says where those bytes come from, what was trimmed with no change in behaviour, and what is left for the owner to decide.
Every number here is measured on a Release device build of `FoolishMessagesApp` from `origin/main` at 4e5ab2fe, unsigned (`CODE_SIGNING_ALLOWED=NO`), with the framework stripped the way the archive strips a dylib (`strip -x`).
The comparison point is UtttKit, the same shape of product, at 0.58 MB.

## How it was measured

- `xcodebuild -scheme FoolishMessagesApp -configuration Release -destination generic/platform=iOS LD_GENERATE_MAP_FILE=YES LD_MAP_FILE_PATH=...` writes a linker map per binary, which is the only attribution that names object files.
- `xcrun size -m -l -x FoolishKit` gives the sections; `xcrun otool -l` gives the LINKEDIT pieces (`strsize`, export trie, chained fixups).
- `xcrun nm -m -n` plus `xcrun swift-demangle` gives per-symbol sizes as address deltas (llvm-nm prints no sizes for Mach-O).
- The C half was compiled to objects on its own with the `IOS_CFLAGS` from `c/Makefile` at `-O2`, `-Os` and `-Oz` and summed with `xcrun size -m`.
- Nothing here was measured on a simulator or a device, so frame time is not measured; the one change that could move it (`-Osize`) is a proposal, not a commit.

## The shape of 3.83 MB

Stripped FoolishKit on `origin/main` is 3,828,632 bytes.
The file is code, translation text, one C global, Swift metadata, and the symbol table a dynamic framework must export.

| Section | Bytes | What it is |
| --- | ---: | --- |
| `__TEXT,__text` | 1,687,920 | Swift 1.57 MB, C 0.12 MB (link map, after dead stripping) |
| `__LINKEDIT` | 622,592 | 5,072 exported symbols: names 306,144, nlist 100,656, export trie 136,832, chained fixups 55,872, function starts 10,472 |
| `__TEXT,__cstring` | 581,330 | 521,478 of it is the 25 language tables (`FoolishStrings*.o`); the rest is Swift metadata strings |
| `__DATA,__data` | 480,004 | 310,800 is the 25 Swift string tables (12,432 each), 136,000 is `g_session`, the rest is Swift globals |
| `__TEXT,__swift5_typeref` | 149,178 | mangled generic type names, 93% of it from the SwiftUI view files (MessageTableView 31 KB, GameSurface 28 KB, RulesView 20 KB, FBattleGrid 12 KB) |
| `__const` (both) | 109,680 | Swift metadata, protocol witness tables, C block descriptors |
| `__DATA,__bss` | 1,047,304 | zero-fill, free on disk: the kernel's scratch games and replay arenas |

The appex itself is 122 KB and imports 43 symbols from FoolishKit.

### By source file (link map, `__text` bytes, top 25)

| Object | `__text` | Note |
| --- | ---: | --- |
| GameSurface.o | 198,408 | one 2,610-line SwiftUI view; its four value witnesses alone are 17 KB, `resolvedContent` 11.6 KB, `body` 5.6 KB |
| AnimLog.o | 129,396 | 242 per-call-site specialisations of `say`, in Release, to print nothing (gated now, see below) |
| RulesView.o | 77,316 | `tableExample` inlined nine times: two closures of 17 KB and 14 KB |
| MessageTurnController.o | 75,984 | |
| MessageTableView+Sequence.o | 70,448 | `runEventStream` alone is 88 KB across 59 async partial functions |
| MessageTableView.o | 53,932 | |
| BoardFlight.o | 51,708 | |
| MessageTableView+Undo.o | 39,532 | |
| Models.o (sdk/swift) | 35,084 | |
| replay.o (C) | 31,952 | `run_replay_v6` is 18 KB |
| MessageEnvelope.o (sdk/swift) | 31,268 | |
| LobbyScreens.o | 30,924 | |
| FHandFan.o | 27,152 | |
| kernel.ios.o (generated) | 26,420 | |
| FlightRecorder.o | 26,124 | a product feature (the owner's hang/memory diagnostic), not debug tooling |
| anim_plan.o (C) | 24,412 | |
| FBattleGrid.o | 24,008 | |
| FRoleMotion.o | 23,512 | |
| MessagesRootView.o | 23,472 | |
| MessageTableView+Play.o | 22,920 | |
| CollapseLayer.o | 22,264 | |
| MessageTableView+OpenReplay.o | 22,168 | |
| CollapseSlide.o (shared) | 20,560 | |
| MessageTableView+BoutEnd.o | 18,728 | |
| game.o (C) | 18,376 | |

All C objects together are 119,108 bytes of `__text`.

### Cross-cutting Swift costs

These are sums over symbol kinds in the whole binary, and they overlap with the table above.

| Kind | Bytes | Symbols | Cause |
| --- | ---: | ---: | --- |
| async partial functions ("suspend resume partial function for") | 212,900 | 493 | every `await` in a long `async` function splits it and repeats the frame setup; `runEventStream` has 26+ |
| generic specialisations | 134,736 | 667 | SwiftUI generics, `ForEach`, `Array` of tuples |
| value witnesses (copy, take, destroy) | 116,164 | 1,184 | large structs with many stored properties; `GameSurface` is the worst |
| closure-propagated specialisations of `AnimLog.say` | 129,396 | 242 | removed |

## Ranked opportunities

Savings are against the 3,828,632-byte baseline; "measured" means a full Release build was made with the change and stripped.

| # | Opportunity | Saving | Basis | Risk | Effort | Status |
| --- | --- | ---: | --- | --- | --- | --- |
| 1 | Merge FoolishKit into the appex (Xcode mergeable library) so nothing is exported | 423,568 measured | build: appex 3,230,440 + stub 33,144 vs 3,565,008 + 122,144 | medium: `Bundle(for:)` texture lookup must change, Foolish app also links it, not run | 1 day | proposal |
| 2 | Swift `-Osize` | 258,360 measured | build: 3,570,272 | perf unmeasured: needs a rig take before and after | 1 line | proposal, needs the owner's frame check |
| 3 | Language tables as C (like `uttt/c/src/uttt_lang.c`), indexed by key, from the same `c/i18n` source | ~230-290 KB | 310,800 of `__data` becomes 25 x 387 x 8 B of pointers (77 KB) or 2 B offsets (19 KB); `__cstring` unchanged | low: `FStrings.t` keeps its API | 1 day | proposal |
| 4 | Ship only the keys iOS references (139 of 387 keys are web-only: dashboard, oracle, ranked) | ~150 KB `__cstring` + ~110 KB `__data` | grep of every Swift string literal against the generated table | low, if the generator's allow-list is a test | half a day | proposal |
| 5 | `AnimLog` compiles to nothing in Release | 132,536 measured | build: 3,696,096 | none: no Release path ever set the env var | done | committed |
| 6 | `g_session` in `__bss` via a load-time constructor | 131,088 measured | build: 3,565,008 (`__data` -136,000) | none: same fresh session, checked by `ios-smoke` | done | committed |
| 7 | Split `runEventStream` and the other long async functions so each `await` splits a small function | ~50-100 KB | 213 KB of async partials, 88 KB in one function | medium: it is the animation sequencer | days | proposal |
| 8 | `RulesView` illustration helpers as `View` structs instead of `some View` functions | ~40 KB | the two inlined closures are 31 KB, nine call sites | low: static illustrations | 1 hour | proposal |
| 9 | C at `-Os` / `-Oz` for the core archive | ~45-50 KB | objects: 157,772 -> 100,380 (`-Os`) -> 91,048 (`-Oz`) before dead strip | the same archive is the phone app's Monte Carlo inner loop (project.yml note); needs a third, size-built slice for the extension | half a day | proposal, owner's call |
| 10 | Slim `GameSurface`'s stored state (value witnesses 17 KB, `resolvedContent` 12 KB) by moving state into a class or smaller structs | ~30 KB | symbol sizes | medium | days | proposal |
| 11 | Move the rulebook and about paragraphs (238 KB + 31 KB across 25 languages) behind compression or into the C table as a compressed blob | ~150-200 KB | `c/i18n` byte counts | low | 1 day | proposal, after 3 |

Things that were checked and are already right: `-dead_strip` is on, `-O` with whole-module optimisation is on, `MessageDevBoard` and `MessageDebugFlags` are `#if DEBUG || SOLO_TESTING` and absent from Release, `Localizable.xcstrings` is excluded, no Supabase or network stack is linked, and `FlightRecorder` is a shipped diagnostic the owner asked for and stays.

## What was implemented

Each change is its own commit with the before/after in the message.
Together they take the stripped framework from 3,828,632 to 3,565,008 bytes (-263,624, -6.9%) with no change in what a Release build does.
The kernel suite (`make -C c tests`, 7,385 checks) and the bridge smoke (`make -C c ios-smoke`) pass on the result.

### AnimLog compiles to nothing in Release (-132,536 bytes)

In Release `AnimLog.on` read two environment variables that nothing in the repo sets for a Release build, and an App Store extension cannot be handed an environment anyway.
The optimiser had specialised `say` once per call site with that site's string interpolation propagated in: 242 symbols, 129 KB of `__text` in `AnimLog.o`, to print nothing.
Now `on` is the literal `false` and `say` is an empty inlinable function under `#else`, so after inlining every message closure is dead and the linker drops it, along with the message literals.
Debug builds, the harness and the rig are unchanged: they always traced, and still do.
Measured: 3,828,632 -> 3,696,096 bytes; `__text` 1,687,920 -> 1,566,304.

### `g_session` in `__bss` (-136,000 bytes)

`c/ios/ios_api.c` held the resident `FioSession` as an initialised global because three of its fields have non-zero sentinels, and an initialised global is `__data`: 136 KB of the shipped file was a `Game` full of zeros with three non-zero bytes in it.
The struct is now uninitialised (zero-fill) and a `__attribute__((constructor))` writes the three sentinels when the framework is mapped, before any Swift can call in.
The first attempt used plain stores and vanished: clang's global optimiser evaluates a constructor that only stores constants into a global and folds it back into a static initialiser, which put the struct straight back into `__data`.
The stores go through a `volatile` pointer, which the evaluator cannot fold, and the file says so.
`c/ios/ios_api_smoke.c` now checks the fresh session shows all three sentinels before the first game, and each check was mutation-checked by dropping its store (each fails alone).
Measured: `__data` 479,956 -> 343,956 (-136,000); stripped 3,696,096 -> 3,565,008 (-131,088, the rest is page rounding).

## Proposals in detail

### 1. Stop exporting FoolishKit (the biggest lever)

FoolishKit is a dynamic framework, so every `public` symbol is exported, and the appex uses 43 of the 5,072.
The 5,029 unused exports cost 306 KB of symbol names, 101 KB of nlist entries and 137 KB of export trie that `strip -x` cannot touch, and they are dead-strip roots, so nothing behind them can be removed either.
The clean fix is to link FoolishKit into the appex: Xcode 15's mergeable libraries (`MERGEABLE_LIBRARY = YES` on FoolishKit, `MERGED_BINARY_TYPE = automatic` on FoolishMessages) do this in Release and keep the framework bundle for its resources.
The one thing to fix first is `FTextures.load`, which finds the felt and wood textures with `Bundle(for: BundleToken.self)`; in a merged binary that class lives in the appex and the lookup must go by bundle identifier (`cards.foolish.kit`) with the old path as the fallback.
The phone app (`Foolish`) also links FoolishKit and FoolishNet, and merging is per consumer, so it either merges too or keeps the dylib.
Measured, with a temporary project edit (the two settings above), on top of the two committed changes: the merged appex is 3,230,440 bytes fully stripped (an extension is an executable, so the archive strips it with `strip`, not `strip -x`) and the framework stub is 33,144, against today's 3,565,008 + 122,144.
That is -423,568 bytes (-11.5%) for the pair, and it is all export overhead: the merged `__text` is 1,573,640 against 1,566,336, so the linker found nothing new to dead-strip, which says the `public` surface is not what is keeping code alive, only what is keeping names alive.
The build was not run, so the texture lookup above is still the open question, and the appex would also stop paying dyld's export lookups at launch.

### 2. `-Osize`

`SWIFT_OPTIMIZATION_LEVEL = -Osize` on FoolishKit measured 3,828,632 -> 3,570,272 (-258,360, -6.7%), `__text` -273 KB, everything else unchanged.
The old note in `ios/project.yml` says 42 KB, measured on a 2.62 MB binary; the code has since grown and so has the win.
It is not committed because the owner's acceptance criterion for the board is frame time on a device, which this session could not run (no simulator, no device).
To take it: change the one line, film one burst-capture take of a pickup on the rig before and after, and compare with the motion scorer.
The hot paths (kernel, animation plans, replay decode) are C and are not affected by the Swift optimisation level.

### 3 and 4. The language tables

The iMessage app ships 25 languages x 387 keys as generated Swift dictionaries: 521 KB of UTF-8 in `__cstring` and 311 KB of 32-byte (key, value) pairs in `__data`, and each table is hashed into a `Dictionary` on first use.
The source is already C (`c/i18n/strings_<code>.c`, keyed by `c/i18n/keys.h`); only the generated target is Swift.
UTTT already made this move (`uttt/c/src/uttt_lang.c`): compile the C tables into the xcframework as `const char *const [KEY_COUNT]` per language, expose one lookup entry, and have `FStrings.t` call it.
Keys in `keys.h` are alphabetical, so a `bsearch` over the key names keeps `t("key")` string-keyed at every call site, or datagen emits a Swift enum of key indices and the call sites move over file by file.
That removes the 311 KB of `__data` for 77 KB of pointer tables (19 KB with 16-bit offsets into one blob), and the `__cstring` bytes stay the same.
Separately, 139 of the 387 keys are never named by any Swift file under `ios/`, `sdk/swift` or `shared/swift` (they are the website's dashboard, oracle and ranked strings): 152,865 of the 562,120 translation bytes, 27%.
A per-host allow-list in the generator, checked by a test that greps the Swift sources, drops them from the app's tables for about 150 KB of `__cstring` and 110 KB of `__data`.
The rulebook (`ios.rules.*`, 238 KB) and about paragraphs (31 KB) are half of what remains and read once per user, so once the table is C they could be one compressed blob per language decoded on demand.

### 7. Async partials

Swift splits an `async` function at every `await` into partial functions, each with its own frame setup, and 493 of them are 213 KB of this binary.
`MessageTableView.runEventStream` alone is 88 KB in 59 pieces.
Restructuring it as a small driver that awaits one step function per event would cut the duplicated prologue and epilogue per suspension point.
This is the animation sequencer, so it needs the rig's before/after takes, not just a size number.

### 8. `RulesView`

`tableExample` and `coverExample` return `some View` and are called nine and four times, so each call site gets a copy of the whole nested generic body; the two biggest copies are 17 KB and 14 KB.
Wrapping each helper in a `struct ...: View` compiles the body once.
The illustrations are static, so the view identity change cannot be seen.

### 9. C at `-Os`

The core objects shrink from 157,772 to 100,380 bytes of `__text` at `-Os` and 91,048 at `-Oz` (before dead strip; 119 KB of C survives the link at `-O2`, so the linked saving is about 45-50 KB, which agrees with the earlier audit).
`ios/project.yml` records why it was not taken: the same archive is the host app's Monte Carlo inner loop, so it needs a third, size-built slice for the extension, or the phone app takes the slowdown.

## Not worth doing

- `-Xfrontend -disable-reflection-metadata` would drop `__swift5_fieldmd` and `__swift5_reflstr` (30 KB) but SwiftUI reads field metadata to find `@State` and other dynamic properties, so it is unsafe.
- Stripping harder: a dylib's exported symbols cannot be stripped, which is proposal 1's point.
- Dropping languages is a product decision, not a code one; the per-language cost is about 33 KB (21 KB text, 12 KB table).
- LTO across the C objects: `c/Makefile` keeps per-TU objects on purpose so the archive stays portable across Xcode versions, and the C is 3% of the file.
