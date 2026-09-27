# Pick 'Em Up - the iMessage app

The Messages extension and its container, on uttt's shape (`uttt/ios/README.md`): the kernel is C in `pickemup/c`, reached through one bridge (`pickemup/c/ios/include/pk_api.h`), and every screen is SwiftUI copied from foolish's views (ORCHESTRATION O1).
Decisions taken on the owner's behalf are in `pickemup/docs/IOS_DECISIONS.md`.

## Build

```
make -C pickemup/c ios-lib          # vendor/Pickemup.xcframework + Generated/ (readers, strings)
cd pickemup/ios && xcodegen generate
xcodebuild -project Pickemup.xcodeproj -scheme PickemupMessagesApp \
  -destination 'generic/platform=iOS Simulator' build
```

`vendor/`, `Generated/` and `Pickemup.xcodeproj` are build outputs and git-ignored.
Run `ios-lib` again after any change under `pickemup/c`: the library is stamped with the layout hash of the readers written beside it, and a pair that does not match shows the unreadable screen instead of reading at a wrong offset (D46, I22).
`DebugAppGroup.entitlements` is set per config by hand, so xcodegen does not know it and cannot blank it; after any xcodegen run, `git status --short -- '*.entitlements'` must be empty.

## Test

```
pickemup/ios/scripts/mac_tests.sh                    # ios-lib, xcodegen, PickemupKitTests, the shipping build
pickemup/ios/scripts/mac_tests.sh --no-lib unit      # the tests only
DEST='platform=iOS Simulator,id=<udid>' pickemup/ios/scripts/mac_tests.sh
```

`PickemupKitTests` drives the view model against the real kernel (two phones in one process, `PickemupKitTests/Phones.swift`), the layout numbers through the bridge, the card faces and the bubble picture's size.
The portable half is `make -C pickemup/c run asan`, which includes the bridge smoke that pins every layout threshold.
Each test's mutation, and the assertion it went red on, is in `TESTS_MUTATED.md`.
Keep at most two simulators booted on this Mac, and shut yours down when done.

## Run and drive with the rig

```
source pickemup/ios/Tools/rig.env
eval "$(foolish/ios/Tools/rig/rig.sh newsim PickemupRig)"
foolish/ios/Tools/rig/rig.sh doctor
foolish/ios/Tools/rig/rig.sh build
```

`rig.env` derives every path from its own location, so a worktree drives its own tree.
A Debug build reads two dev files from the App Group (`PickemupKit/Kernel/PickemupDev.swift`): `dev.empty` draws nothing, and `dev.nick` is the nickname a fresh simulator sits down under.

## What is where

| Path | What |
|---|---|
| `PickemupKit/Kernel/Pk.swift` | the bridge from Swift: identity, the resident message, the lobby, staging, the reads, the words |
| `PickemupKit/Kernel/PkLayout.swift` | `pk_lay.c`'s numbers as CGFloat: hand row, ring, fan, deck, pile, pill slots |
| `PickemupKit/Kernel/PickemupSeats.swift` | the seat records and the nickname, kept for the kernel |
| `PickemupKit/Board/TableModel.swift` | what the table shows and what a touch does, each touch one kernel call |
| `PickemupKit/Board/TableScreen.swift` | the table: ring, status corner, direction box, pile, deck, pills, hand, picker, end reveal |
| `PickemupKit/Board/Anchors.swift` | every element's frame under UI.html's anchor name, for the flight layer |
| `PickemupKit/Screens/` | lobby and name gate, the unreadable screen, the rules, the bubble picture, the root |
| `PickemupKit/Design/` | foolish's tokens, felt, wood, buttons and card frame, with this game's faces |
| `PickemupMessages/MessagesViewController.swift` | the conversation: adopt, stage through the shared insert loop, send, cancel |

Every file copied from foolish or uttt starts with `// COPIED from <path> at <commit> - replaced by lift step Sn` (REUSE_AUDIT.md), so the lift that owns it can find and delete it.

## What Swift does not do

It decides no rule, derives no layout number and composes no sentence.
Legality, the masked view, captions, screen lines, the pill slots, the hand overflow, the ring and the results order are all one call into C.
The board is static: a new state snaps in, and the motion grid's flights are the next layer, flown between the named anchors.
