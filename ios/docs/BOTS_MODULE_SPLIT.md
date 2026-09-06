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
| header + clang module | `c/ios/include/ios_api.h`, `CFoolish` | `c/ios/include-bots/ios_bots_api.h`, `CFoolishBots` |
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

## Finishing on a Mac

Everything above is verified.
What could not be, because it needs Xcode:

1. `cd c && make ios-lib` - builds both xcframeworks. New: it emits
   `FoolishBots.xcframework` alongside `Foolish.xcframework` and runs
   `archive_check.sh` on the device slice at the end.
2. `cd ios && xcodegen generate` - picks up the new `FoolishBots` target.
3. Build `Foolish` and `FoolishMessages`. The Swift move is mechanical but
   unverified by a compiler:
   - `EngineC`'s `json` and `check` (and the `e*` code constants) are now
     `@_spi(FoolishBots) public` instead of `private`, because an extension in
     another module cannot reach a private member. If `@_spi` gives trouble,
     plain `public` works and is the fallback.
   - `sdk/swift/bots/EngineC+Bots.swift` declares
     `@_spi(FoolishBots) import FoolishKit` and `import CFoolishBots`.
   - `LocalGame.swift` and `BotDriveWire.swift` moved unchanged; they may need
     `import FoolishKit` added if they were relying on being in that module.
   - `BotDrive` (the struct) deliberately stayed in `FoolishKit`'s
     `Models.swift`: it names no C symbol, so it costs nothing there.
4. Confirm on the built products, which is the only place it can be confirmed
   for real:
   ```
   nm -a <FoolishMessagesApp.app>/Frameworks/FoolishKit.framework/FoolishKit \
     | grep -ci octogen        # expect 0
   ```
   and the same over the whole `.appex`.

## Two things left alone

`WatchFoolish` references `LocalGame` but declares no dependency on
`FoolishKit`, and `WatchUI` imports neither.
That target does not build today and did not before this change, so it was not
touched (`project.yml` says as much at its definition).

`tests/msg_flow_sim.c` does not compile, here or on main: it predates a
`fio_msg_encode` signature change.
