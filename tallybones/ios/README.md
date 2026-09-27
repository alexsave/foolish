# Tallybones - the iMessage app

The Messages extension and its container, on Pick 'Em Up's shape (`pickemup/ios/README.md`): SwiftUI screens on foolish's felt and wood, one conversation controller, and every rule, word and die behind one seam.
The kernel (`tallybones/c`) is being written on another branch, so today the seam is a Swift protocol, `TallyKernel`, and the only thing behind it is `StandInKernel`: a fixed hand of dice, a second seat that plays itself, and no rules.
Decisions taken on the owner's behalf are rows T10 to T18 of `../docs/DECISIONS.md`.

## Build

```
cd tallybones/ios && xcodegen generate
xcodebuild -project Tallybones.xcodeproj -scheme TallybonesMessagesApp \
  -destination 'generic/platform=iOS Simulator' build
```

`Tallybones.xcodeproj` is a build output and git-ignored, as `vendor/` and `Generated/` will be.
`DebugAppGroup.entitlements` is set per config by hand, so xcodegen does not know it and cannot blank it; after any xcodegen run, `git status --short -- '*.entitlements'` must be empty.

## Test

```
tallybones/ios/scripts/mac_tests.sh                                          # xcodegen, TallybonesKitTests, the shipping build
DEST='platform=macOS,variant=Mac Catalyst' tallybones/ios/scripts/mac_tests.sh unit   # the tests with no simulator (T18)
DEST='platform=iOS Simulator,id=<udid>' tallybones/ios/scripts/mac_tests.sh
DEST='generic/platform=iOS Simulator' tallybones/ios/scripts/mac_tests.sh app        # the shipping build only
```

`TallybonesKitTests` pins the die faces, the keep toggling, T11's blanks until a KEEP is sent, the tumble, the scorecard's rows and the bubble picture's size.
Each test's mutation, and the assertion it went red on, is in `TESTS_MUTATED.md`.
Keep at most two simulators booted on this Mac, and shut yours down when done.

## Run and drive with the rig

```
source tallybones/ios/Tools/rig.env
eval "$(foolish/ios/Tools/rig/rig.sh newsim TallybonesRig)"
foolish/ios/Tools/rig/rig.sh doctor
```

`rig.sh build` runs `make ios-lib` in `tallybones/c` first, so it stops there until the kernel lands; until then build with the `xcodebuild` line above and install the `.app` with `xcrun simctl install`.
A Debug build reads two dev files from the App Group (`TallybonesKit/Kernel/TallybonesDev.swift`): `dev.empty` draws nothing, and `dev.nick` is the nickname a fresh simulator sits down under.

## What is where

| Path | What |
|---|---|
| `TallybonesKit/Kernel/TallyKernel.swift` | THE SEAM: every call the screens and the conversation make, each naming the pickemup entry point it mirrors, and `TallyStage` |
| `TallybonesKit/Kernel/StandInKernel.swift` | the stand-in behind the seam: two canned hands and their canned scores; T11's blanks kept faithfully |
| `TallybonesKit/Model/TrayModel.swift` | the plain values: `TrayModel`, `CardModel`, `LobbyModel`, `Category`, `TallyView`, `DraftKind` |
| `TallybonesKit/Board/TallyTable.swift` | the view model: my keep marks before Roll, the `previewScores` closure, one kernel call per touch |
| `TallybonesKit/Board/DiceFace.swift`, `DiceTray.swift`, `TumblePlayer.swift` | a drawn die, the tray, the tumble behind `DiceMotion` |
| `TallybonesKit/Board/Scorecard.swift`, `SeatBadge.swift`, `TableScreen.swift`, `Anchors.swift` | the card, the badges, the table, the named anchors |
| `TallybonesKit/Screens/` | lobby, name gate, the unreadable screen, the bubble picture, the root and host |
| `TallybonesKit/Design/` | foolish's tokens, felt, wood and buttons, by way of pickemup |
| `TallybonesMessages/MessagesViewController.swift` | the conversation: adopt, stage through the shared insert loop, send, cancel |

Every file copied from pickemup starts with `// COPIED from <path> at <commit>`, so the lift that owns it can find and delete it.

## Wiring the kernel

What the integration worker adds, in this order, and nothing else in the shell has to change:

1. **The library.** `make -C tallybones/c ios-lib` writes `ios/vendor/Tallybones.xcframework` (static, module `CTallybones`) and `ios/Generated/` (the structgen readers, e.g. `TallybonesKernel.swift`, and the datagen string tables), with `shared/tools/ios_xcframework.mk` as pickemup does.
   Both are already git-ignored.
2. **`project.yml`**, at the two `# KERNEL:` comments in the `TallybonesKit` target: add `- path: Generated` to `sources`, and
   ```
   dependencies:
     - framework: vendor/Tallybones.xcframework
       embed: false
   ```
   `HEADER_SEARCH_PATHS` already has `$(BUILT_PRODUCTS_DIR)/include`, where the xcframework copies the `CTallybones` module map.
3. **`TallybonesKit/Kernel/BridgeKernel.swift`**: `final class BridgeKernel: TallyKernel` over `import CTallybones` and the generated readers, one bridge call per method, in the shape of `pickemup/ios/PickemupKit/Kernel/Pk.swift`; each method's doc comment in `TallyKernel.swift` names the pickemup entry point it mirrors.
   It must keep the T11 contract written at the top of `TallyKernel.swift`: `view()` over a pending KEEP reports the rerolling dice as 0.
4. **The preview numbers**: the kernel's per-category preview for the dice on the tray, as the `previewScores` closure.
5. **`TallybonesRoot.swift`, `TallybonesHost.init`**: the one line that picks the kernel; pass `BridgeKernel()` and the bridge's preview, then delete `StandInKernel.swift` and point the tests' fixtures at the real kernel (pickemup's `PickemupKitTests/Phones.swift` plays two phones in one process).
6. **The `// KERNEL:` literals**: `FMotion` in `Design/Tokens.swift` (from `tb_beats.h`), `TbLayout` in `Board/TableScreen.swift` (from a `tb_lay.c`, if the kernel takes the layout as pickemup's does), and the tumble in `Board/TumblePlayer.swift` (a `BeatPlayer` conforming to `DiceMotion`, sampling the dice-settle beat of T9 at anchors `die.0` to `die.4`).
7. **`scripts/mac_tests.sh`** needs no edit: it builds the library as soon as `tallybones/c` exists.

`grep -rn "KERNEL:" tallybones/ios` lists every place.

## What Swift does not do

It scores no dice, decides no bonus, derives no roll and composes no sentence: those are the kernel's, and today the stand-in's canned data (plus a plain sum for the totals, marked STAND-IN).
The one thing the shell owns is my keep marks between taps and the Roll that sends them, because until the KEEP move carries the mask it is nobody else's.
