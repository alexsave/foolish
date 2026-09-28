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
A Debug build also reads `dev.seed` (64 hex digits: the next game deals from that seed) and `dev.anchors.on` (every anchor's frame written to `dev.anchors` as `name x y w h` lines, for a driver's taps), IOS_DECISIONS I45.
It reads five more dev files from the App Group (`PickemupKit/Kernel/PickemupDev.swift`): `dev.empty` draws nothing, `dev.nick` is the nickname a fresh simulator sits down under, `dev.slide` turns on the auto-collapse slide (A14), `dev.sendhint` the Send reminder (A15), and `dev.persona` ("1 Bo") makes the next appex process another person with its own seat records, so one simulator's two stub threads can seat two players (IOS_DECISIONS I43; leave the thread before flipping it).

## What is where

| Path | What |
|---|---|
| `PickemupKit/Kernel/Pk.swift` | the bridge from Swift: identity, the resident message, the lobby, staging, the reads, the words |
| `PickemupKit/Kernel/PkLayout.swift` | `pk_lay.c`'s numbers as CGFloat: hand row, ring, fan, deck, pile, pill slots |
| `PickemupKit/Kernel/PickemupSeats.swift` | the seat records and the nickname, kept for the kernel |
| `PickemupKit/Board/TableModel.swift` | what the table shows and what a touch does, each touch one kernel call |
| `PickemupKit/Board/TableScreen.swift` | the table: ring, status corner, direction box, pile, deck, pills, hand, picker, end reveal |
| `PickemupKit/Board/Anchors.swift` | every element's frame under UI.html's anchor name, for the flight layer |
| `PickemupKit/Board/BeatPlayer.swift` | the kernel's timeline played: the board as of now, each beat's transform, the cards in the air |
| `PickemupKit/Kernel/PkCollapse.swift` | the shared CollapseSlide on the kernel's push (A14, `dev.slide`) |
| `PickemupKit/Screens/` | lobby and name gate, the unreadable screen, the rules, the bubble picture, the root |
| `PickemupKit/Design/` | foolish's tokens, felt, wood, buttons and card frame, with this game's faces |
| `PickemupMessages/MessagesViewController.swift` | the conversation: adopt, stage through the shared insert loop, send, cancel |

Every file copied from foolish or uttt starts with `// COPIED from <path> at <commit> - replaced by lift step Sn` (REUSE_AUDIT.md), so the lift that owns it can find and delete it.

## What Swift does not do

It decides no rule, derives no layout number and composes no sentence.
Legality, the masked view, captions, screen lines, the pill slots, the hand overflow, the ring and the results order are all one call into C.
It holds no duration, curve or order either: every motion is the kernel's timeline (`pk_beats.h`), which `Board/BeatPlayer.swift` samples each frame and flies between the named anchors (ANIMATION_DECISIONS A1).
Two pieces are compiled in but off until Messages has judged them, each switched on in a Debug build by a dev file in the App Group: `dev.slide`, the auto-collapse on the render server (A14), and `dev.sendhint`, the Send reminder (A15).
