# Chui Niu - the iMessage app

The Messages extension and its container, on pickemup's shape (`pickemup/ios/README.md`).
The screens run on one seam, `ChuiniuKit/Kernel/KernelSeam.swift`, whose one kernel is `ChuiniuKit/Kernel/BridgeKernel.swift`, the `CChuiniu` bridge (`chuiniu/c/ios/include/cn_api.h`) read through the generated readers.
Decisions are in `chuiniu/docs/DECISIONS.md`, section "iOS and rendering".

## Build

```
make -C chuiniu/c ios-lib          # vendor/Chuiniu.xcframework and Generated/
cd chuiniu/ios && xcodegen generate
xcodebuild -project Chuiniu.xcodeproj -scheme ChuiniuMessagesApp \
  -destination 'generic/platform=iOS Simulator' build
```

`Chuiniu.xcodeproj`, `vendor/` and `Generated/` are build outputs and git-ignored.
`DebugAppGroup.entitlements` is set per config by hand, so xcodegen does not know it and cannot blank it; after any xcodegen run, `git status --short -- '*.entitlements'` must be empty.

## Test

```
chuiniu/ios/scripts/mac_tests.sh              # ios-lib, xcodegen, ChuiniuKitTests, the shipping build
chuiniu/ios/scripts/mac_tests.sh app          # the shipping build only
DEST='platform=iOS Simulator,id=<udid>' chuiniu/ios/scripts/mac_tests.sh unit
```

Each test's mutation, and the assertion it went red on, is in `TESTS_MUTATED.md`.
Keep at most two simulators booted on this Mac, and shut yours down when done.

## Run and drive with the rig

```
source chuiniu/ios/Tools/rig.env
export RIG_SIM=<udid>
foolish/ios/Tools/rig/rig.sh build          # ios-lib, xcodegen, the Debug build, install
foolish/ios/Tools/rig/rig.sh stage light
foolish/ios/Tools/rig/rig.sh seat Alex      # who this simulator is (DECISIONS I15)
foolish/ios/Tools/rig/rig.sh open           # the + menu: a new lobby and its invitation
```

A Debug build reads three dev files from the App Group (`ChuiniuKit/Kernel/ChuiniuDev.swift`): `dev.empty` draws nothing, `dev.nick` is the nickname a fresh simulator sits down under, and `dev.seat` makes the extension that person, so one simulator plays both sides of a thread (`rig.sh killappex`, `rig.sh seat Bo`, then tap the newest bubble).
It writes `dev.staged` and `dev.sent`, the newest links it staged and sent, for `chuiniu/c/tests/cn_link_dump.c` to decode.
How a whole game was played this way is `chuiniu/docs/SIM_VERIFICATION.md`.

## What is where

| Path | What |
|---|---|
| `ChuiniuKit/Kernel/KernelSeam.swift` | the model the screens draw and the `Kernel` protocol |
| `ChuiniuKit/Kernel/BridgeKernel.swift` | the kernel: `cn_api_*` read into the model, the seat records and the nickname |
| `ChuiniuKit/Design/Die.swift`, `Cup.swift` | a die face 1 to 6 at any size, a plain cup (product-neutral) |
| `ChuiniuKit/Board/DiceRoll.swift` | one seat's roll: shake, lift, tumble, settle; timing in `RollBeats` |
| `ChuiniuKit/Board/BidPicker.swift` | quantity stepper, face chips, Raise and Call, lit from the kernel's menu |
| `ChuiniuKit/Board/DiceTable.swift` | the felt table, the seats round it, my dice, the bid; `DiceTableLayout` |
| `ChuiniuKit/Screens/` | lobby, table, reveal, the root and its host, the bubble picture |
| `ChuiniuKit/Design/Tokens.swift`, `Materials.swift`, `Buttons.swift` | copied from pickemup |
| `ChuiniuMessages/MessagesViewController.swift` | the conversation: adopt, keep a staged move, the sender fact, stage after the move rests, send, cancel |

Swift decides no rule, composes no sentence and derives no game number: each is a field of `TableModel` or a `Kernel` word.
