# Keeping the bot ladder out of the iMessage bundle

## What was wrong

`FoolishKit` is a dynamic framework, so its public API is exported and every
exported symbol is a `-dead_strip` root.
`EngineC.botDrive` was public on `FoolishKit`, so it was such a root, and it
referenced `fio_bot_drive_packed` -> `bot_drive` -> `bot_roster` -> all 21
strategies, `octogen` included.
`FoolishKit.framework` ships inside `FoolishMessagesApp`, the iMessage-only
container.

So a product that plays people and never drives a seat was carrying the
strongest brain in the roster, in compiled form, in a shipped bundle.

No literal source was ever exposed: the xcframework ships object code plus
`ios_api.h` and `module.modulemap` and nothing else, and the wasm modules are
`--strip-all` with no name section.
Compiled octogen in a shipped binary is still the thing worth not shipping.

## The shape now

The split is made twice, at the C archive and at the Swift module, because
either one alone can be undone by link order or by dead-strip behaviour.

| | core | bots |
|---|---|---|
| C sources | `IOS_CORE_SRC` (`c/Makefile`) | `IOS_BOTS_SRC` |
| C bridge TU | `c/ios/ios_api.c` | `c/ios/ios_bots_api.c` |
| header + clang module | `c/ios/include/ios_api.h`, `CFoolish` | `c/ios/include-bots/CFoolishBots/ios_bots_api.h`, `CFoolishBots` |
| xcframework | `Foolish.xcframework` | `FoolishBots.xcframework` |
| Swift module | `FoolishKit` | `FoolishBots` |
| Swift sources | `sdk/swift/` except `bots/` | `sdk/swift/bots/` |
| linked by | everything | the host app and its tests only |

`FoolishMessages` and `FoolishMessagesApp` depend on `FoolishKit` and nothing
else.
There is no bot symbol for them to name, because the declarations are not in
`ios_api.h` at all.

## The thing that could have gone silently wrong

The kernel keeps ONE static `Game` and hands it to the bot half through
`fio_resident_game()` (`c/ios/ios_internal.h`).
If `libfoolishbots.a` ever pulled its own copy of `ios_api.o`, there would be
two resident games: the app would deal a board and the bots would drive a
different, empty one.
Nothing would crash.
It would just play nonsense.

`FoolishBots.xcframework` therefore contains no core object, and its undefined
`fio_resident_game` / `game_*` / `legal_*` resolve against `FoolishKit` at link
time.
`c/ios/one_game_check.c` asserts it: deal through the core, drive through the
bots, read the board back through the core.

## What is checked, and where

Run on Linux and in CI, no Mac needed:

- `make ios-archives` builds both archives with the host compiler from the
  Makefile's own source lists, then runs both checks below.
- `ios/archive_check.sh` fails if the CORE archive defines any ladder symbol,
  and also if the BOTS archive defines none (a split that quietly shipped an
  empty bots library would otherwise pass).
- `ios/one_game_check.c` fails if the two halves do not share a resident game.
- `ios/split_check.sh`, already wired into `make ios-smoke`, fails if
  `ios_api.c` so much as names a bot symbol.

Measured when this landed, on the host compiler:

```
core archive  195,820 B     0 ladder symbols
bots archive  442,070 B     3 octogen symbols
```

so the ladder is 69% of the native kernel, and the extension no longer links
any of it.

## Finished on a Mac

Done, on Xcode 26.2, iOS 26 simulator.
`make ios-lib` writes both xcframeworks; `xcodegen generate` picks up the
`FoolishBots` target; `Foolish`, `FoolishMessagesApp` and the 627 `FoolishTests`
all build and pass.

The check that counts, on the built bundle:

```
$ nm -a FoolishMessagesApp.app/Frameworks/FoolishKit.framework/FoolishKit \
    | grep -ci octogen
0
$ ls FoolishMessagesApp.app/Frameworks
FoolishKit.framework
```

and the whole ladder pattern (the one `archive_check.sh` uses, not octogen
alone), swept over every file in `FoolishMessagesApp.app` including
`PlugIns/FoolishMessages.appex`, matches nothing.
The same sweep over `Foolish.app` finds the ladder where it belongs:
`Frameworks/FoolishBots.framework`, 4 octogen symbols, and 0 in that app's
`FoolishKit`.

Five things the Mac half needed that the Linux half could not have found:

- **Two `module.modulemap` files, one `include/`.** Xcode copies EVERY linked
  static xcframework's headers into the single `$(BUILT_PRODUCTS_DIR)/include`,
  so two maps by that name are `Multiple commands produce`. The bots header and
  its map therefore sit in `c/ios/include-bots/CFoolishBots/`, and the
  `FoolishBots` target names that subdirectory in `HEADER_SEARCH_PATHS`.
  The core half is untouched: `include/module.modulemap` is where Xcode looks
  by itself.
- **`import FoolishKit`** in `LocalGame.swift` and `BotDriveWire.swift`. Their
  types are all FoolishKit's now; only `EngineC+Bots.swift` came with an import.
- **Public inits on `BotAction` and `BotDrive`.** A synthesized memberwise init
  is internal however public the stored properties are, and `BotDriveWire` -
  the only thing that builds either - is now in another module.
- **`@_spi(FoolishBots) public` held.** No fallback to plain `public` needed.
- **`ios_api_smoke.c` names four bot entries** and included only `ios_api.h`,
  which stopped declaring them. GCC on Linux made that an implicit-declaration
  warning; clang makes it an error, so `make ios-smoke` did not build here until
  the smoke included `ios_bots_api.h` too.

Played, not just built.
The host app deals and a bot answers - which is what proves the split's real
hazard, one resident game across two libraries, did not happen - the tutorial
runs, and in Messages the shipping extension opens, makes a lobby, deals a
3-player game and plays an attack on a `FoolishKit` that links no ladder.
`HARNESS_SCENARIO=board` and `myplay` render the same board in the harness.
The `Release` device build of `FoolishMessagesApp`, the configuration that
actually ships, carries zero ladder symbols in any file of the bundle; the host
app's `Release` build carries them where they belong, in
`FoolishBots.framework`.

One environment note, not a property of the split: `make ios-archives` used
`ar`, and a homebrew binutils on `PATH` shadows Apple's - a GNU-format archive
that `ld` refuses with `archive member '/' not a mach-o file`. It archives with
Apple's `libtool` on Darwin now, the same tool `ios-lib` already used.

## Two things left alone

`WatchFoolish` references `LocalGame` but declares no dependency on
`FoolishKit`, and `WatchUI` imports neither.
That target does not build today and did not before this change, so it was not
touched (`project.yml` says as much at its definition).

`tests/msg_flow_sim.c` does not compile, here or on main: it predates a
`fio_msg_encode` signature change.
