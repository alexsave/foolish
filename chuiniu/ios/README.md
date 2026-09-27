# Chui Niu - the iMessage app

The Messages extension and its container, on pickemup's shape (`pickemup/ios/README.md`).
The screens run on one seam, `ChuiniuKit/Kernel/KernelSeam.swift`; until the kernel in `chuiniu/c` is wired in, its `FakeKernel` plays scripted data.
Decisions are in `chuiniu/docs/DECISIONS.md`, section "iOS and rendering".

## Build

```
cd chuiniu/ios && xcodegen generate
xcodebuild -project Chuiniu.xcodeproj -scheme ChuiniuMessagesApp \
  -destination 'generic/platform=iOS Simulator' build
```

`Chuiniu.xcodeproj`, and later `vendor/` and `Generated/`, are build outputs and git-ignored.
`DebugAppGroup.entitlements` is set per config by hand, so xcodegen does not know it and cannot blank it; after any xcodegen run, `git status --short -- '*.entitlements'` must be empty.

## Test

```
chuiniu/ios/scripts/mac_tests.sh              # xcodegen, ChuiniuKitTests, the shipping build
chuiniu/ios/scripts/mac_tests.sh app          # the shipping build only
DEST='platform=iOS Simulator,id=<udid>' chuiniu/ios/scripts/mac_tests.sh unit
```

Each test's mutation, and the assertion it went red on, is in `TESTS_MUTATED.md`.
Keep at most two simulators booted on this Mac, and shut yours down when done.

To open the drawer inside Messages: build and install by hand, then `source chuiniu/ios/Tools/rig.env`, set `RIG_SIM`, and `foolish/ios/Tools/rig/rig.sh open` (the rig's `build` wants the kernel, so it waits for the tie-together).
A Debug build reads `dev.scene` from the App Group (`ChuiniuKit/Kernel/ChuiniuDev.swift`): `lobby`, `invited`, `bidding`, `waiting`, `revealed` or `over` opens the fake on that scene.

## Wiring the kernel (the tie-together)

1. `make -C chuiniu/c ios-lib`, then uncomment the four `KERNEL` lines in `project.yml` (the `Generated/ChuiniuKernel.swift` source, the `vendor/Chuiniu.xcframework` dependency and the two `HEADER_SEARCH_PATHS`).
2. Add a `BridgeKernel: Kernel` beside `KernelSeam.swift` that fills `TableModel` from `cn_api.h`, and return it from `KernelSeam.make()`.
3. Delete `FakeKernel` and the `dev.scene` flag.

## What is where

| Path | What |
|---|---|
| `ChuiniuKit/Kernel/KernelSeam.swift` | the model the screens draw, the `Kernel` protocol, the fake |
| `ChuiniuKit/Design/Die.swift`, `Cup.swift` | a die face 1 to 6 at any size, a plain cup (product-neutral) |
| `ChuiniuKit/Board/DiceRoll.swift` | one seat's roll: shake, lift, tumble, settle; timing in `RollBeats` |
| `ChuiniuKit/Board/BidPicker.swift` | quantity stepper, face chips, Raise and Call, lit from the kernel's menu |
| `ChuiniuKit/Board/DiceTable.swift` | the felt table, the seats round it, my dice, the bid; `DiceTableLayout` |
| `ChuiniuKit/Screens/` | lobby, table, reveal, the root and its host, the bubble picture |
| `ChuiniuKit/Design/Tokens.swift`, `Materials.swift`, `Buttons.swift` | copied from pickemup |
| `ChuiniuMessages/MessagesViewController.swift` | the conversation: adopt, stage through the shared insert loop, send, cancel |

Swift decides no rule, composes no sentence and derives no game number: each is a field of `TableModel` or a `Kernel` word.
