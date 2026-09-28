# Tallybones - the iMessage app

The Messages extension and its container, on Pick 'Em Up's shape (`pickemup/ios/README.md`): SwiftUI screens on foolish's felt and wood, one conversation controller, and every rule, word and die behind one seam.
The seam is a Swift protocol, `TallyKernel`, and the one thing behind it is `BridgeKernel`, which calls the C kernel (`tallybones/c`, `tb_api.h`) through `Tb` and reads every struct through the generated readers.
Decisions taken on the owner's behalf are rows T10, T50 to T56 and T60 to T66 of `../docs/DECISIONS.md`.

## Build

```
make -C tallybones/c ios-lib
cd tallybones/ios && xcodegen generate
xcodebuild -project Tallybones.xcodeproj -scheme TallybonesMessagesApp \
  -destination 'generic/platform=iOS Simulator' build
```

`Tallybones.xcodeproj` is a build output and git-ignored, as `vendor/` and `Generated/` are.
`DebugAppGroup.entitlements` is set per config by hand, so xcodegen does not know it and cannot blank it; after any xcodegen run, `git status --short -- '*.entitlements'` must be empty.

## Test

```
tallybones/ios/scripts/mac_tests.sh                                          # xcodegen, TallybonesKitTests, the shipping build
DEST='platform=macOS,variant=Mac Catalyst' tallybones/ios/scripts/mac_tests.sh unit   # the tests with no simulator (T56, T63)
DEST='platform=iOS Simulator,id=<udid>' tallybones/ios/scripts/mac_tests.sh
DEST='generic/platform=iOS Simulator' tallybones/ios/scripts/mac_tests.sh app        # the shipping build only
```

`TallybonesKitTests` runs over the real kernel: the die faces, the keep toggling, T11's blanks until a KEEP is sent, the kernel's settle on the tray, the scorecard's rows, the bubble picture, and a receiver seeing the sender's dice and card (two phones in one process, `Phones.swift`).
Each test's mutation, and the assertion it went red on, is in `TESTS_MUTATED.md`.
Keep at most two simulators booted on this Mac, and shut yours down when done.

## Run and drive with the rig

```
source tallybones/ios/Tools/rig.env
eval "$(foolish/ios/Tools/rig/rig.sh newsim TallybonesRig)"
foolish/ios/Tools/rig/rig.sh doctor
```

`rig.sh build` runs `make ios-lib` in `tallybones/c`, xcodegen (keeping the entitlements) and the Debug build, and installs it.
A Debug build reads three dev files from the App Group (`TallybonesKit/Kernel/TallybonesDev.swift`): `dev.empty` draws nothing, `dev.nick` is the nickname a fresh simulator sits down under, and `dev.who` names the PERSON the simulator plays, which is how one simulator plays both sides of a game (T65): write the file, `rig.sh killappex`, open the other stub thread. `docs/SIM_VERIFICATION.md` has the whole run.

## What is where

| Path | What |
|---|---|
| `TallybonesKit/Kernel/TallyKernel.swift` | THE SEAM: every call the screens and the conversation make, each naming the pickemup entry point it mirrors, and `TallyStage` |
| `TallybonesKit/Kernel/Tb.swift` | the Swift face of `tb_api.h`: one call per entry point, every read through the generated readers |
| `TallybonesKit/Kernel/BridgeKernel.swift` | the seam over `Tb`: the kernel's view, verdicts and words copied into the plain models |
| `TallybonesKit/Model/TrayModel.swift` | the plain values: `TrayModel`, `CardModel`, `LobbyModel`, `Category`, `TallyView`, `DraftKind` |
| `TallybonesKit/Board/TallyTable.swift` | the view model: my keep marks before Roll, the kernel's preview, the newest plan to the player, one kernel call per touch |
| `TallybonesKit/Board/DiceFace.swift`, `DiceTray.swift`, `BeatPlayer.swift` | a drawn die, the tray, the kernel's timeline behind `DiceMotion` |
| `TallybonesKit/Board/Scorecard.swift`, `SeatBadge.swift`, `TableScreen.swift`, `Anchors.swift` | the card, the badges, the table, the named anchors |
| `TallybonesKit/Screens/` | lobby, name gate, the unreadable screen, the bubble picture, the root and host |
| `TallybonesKit/Design/` | foolish's tokens, felt, wood and buttons, by way of pickemup |
| `TallybonesMessages/MessagesViewController.swift` | the conversation: adopt, stage through the shared insert loop, send, cancel |

Every file copied from pickemup starts with `// COPIED from <path> at <commit>`, so the lift that owns it can find and delete it.

## How the kernel is wired

1. **The library.** `make -C tallybones/c ios-lib` writes `ios/vendor/Tallybones.xcframework` (static, module `CTallybones`) and `ios/Generated/` (the structgen readers `TallybonesKernel.swift`, stamped with the layout hash the library carries, and the datagen string tables); `ios-lib-catalyst` adds a Mac Catalyst slice for the tests (T63). Both are git-ignored.
2. **`project.yml`**: `TallybonesKit` compiles `Generated/` and links the xcframework statically; the module map lands in `$(BUILT_PRODUCTS_DIR)/include`.
3. **`Kernel/Tb.swift` and `Kernel/BridgeKernel.swift`**: the bridge calls and the seam (T60). A stale pair (library and readers of different layouts) reads nothing and shows the unreadable screen (`TallybonesHost.readable`).
4. **`Board/BeatPlayer.swift`**: the kernel's plan on the tray (T62).
5. **`TallybonesMessages/MessagesViewController.swift`**: `didStartSending` of my staged bubble calls `tb_api_mark_sent` (T11: the reroll exists from here), then the view is re-read and the send's plan plays; a KEEP keeps the drawer up for the next choice; a tapped or arrived bubble goes through `tb_api_prefer` and `tb_api_adopt`.

## What Swift does not do

It scores no dice, decides no bonus, derives no roll, times no motion and composes no sentence: those are the kernel's.
The one thing the shell owns is my keep marks between taps and the Roll that sends them, because until the KEEP move carries the mask it is nobody else's.
