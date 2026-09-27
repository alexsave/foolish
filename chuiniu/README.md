# Chui Niu

Chui Niu (吹牛) is Liar's Dice in an iMessage thread: 2 to 6 players each hide five dice, raise bids on the whole table, and call a bid they do not believe.
It is a public-domain folk bluffing game, and the name is the game's own generic Chinese one (`LEGAL.md`).

**This is a PROOF OF CONCEPT.**
It skipped the design phase on purpose, and it has no App Store Connect record, no signing for distribution and no TestFlight build.
Every decision taken on the owner's behalf is one paragraph in [docs/DECISIONS.md](docs/DECISIONS.md), and each one can be vetoed.

Commands below run from inside `chuiniu/`.

## Layout

| Folder | What |
|---|---|
| `c/` | the C kernel (`c/src/cn_*`), its tests (`c/tests/`) and the iOS bridge (`c/ios/`, header `cn_api.h`, module `CChuiniu`) (K1) |
| `ios/` | `ChuiniuKit`, `ChuiniuMessages` (the extension) and `ChuiniuMessagesApp` (the container), from `ios/project.yml` (I1) |
| `docs/` | `DECISIONS.md`, the one decisions doc |
| `LEGAL.md` | what is safe to clone here, what is avoided, and what was not checked |

## Build and test

```
make -C c run             every C test and the bridge smoke
make -C c asan            the same under ASan + UBSan
make -C c ios-lib         ios/vendor/Chuiniu.xcframework and ios/Generated/ (Xcode)
make -C c swift-smoke     the bridge driven from Swift (a Mac)
make -C c build/cn_link_dump   decode a bubble's link (a simulator check)

cd ios && xcodegen generate
xcodebuild -project Chuiniu.xcodeproj -scheme ChuiniuMessagesApp -destination 'generic/platform=iOS Simulator' build
ios/scripts/mac_tests.sh  ios-lib, xcodegen, ChuiniuKitTests and the shipping build
```

After any xcodegen run, `git status --short -- '*.entitlements'` must be empty.
CI: `.github/workflows/chuiniu.yml` runs `run`, `asan`, `structgen` and `datagen` on Linux; not the Xcode half.

## What is verified, and how

Every count below is from a run on 2026-09-27 on this Mac, on branch `cn-tie`.

- The kernel: `make -C c run` and `make -C c asan`, every test 0 failed: `cn_test` 3,763 assertions, `cn_dice_test` 6,962, `cn_plan_test` 1,737,483, `cn_say_test` 7,168, `cn_fuzz` 15,759,233 (3,134,738 under ASan), `cn_msg_test` 89,178 (14,297 under ASan), `cn_twophone_test` 9,909, and the bridge smoke 77 checks; `make -C c swift-smoke` 40 checks from Swift through the generated readers.
- The bridge in the app: `make -C c ios-lib` writes `ios/vendor/Chuiniu.xcframework` and `ios/Generated/` (layout hash `0x584fcc1c`, the file names `.github/workflows/chuiniu.yml` checks for), and `BridgeKernel` refuses to read a stale pair (`cn_api_layout_hash` against `SG_LAYOUT_HASH`).
- `DEST='platform=iOS Simulator,name=iPhone 17e,OS=27.0' ios/scripts/mac_tests.sh`: ios-lib, xcodegen, `ChuiniuKitTests` 12 tests 0 failures, and the shipping `ChuiniuMessagesApp` build; `git status --short -- '*.entitlements'` empty after every xcodegen run.
- `BridgeKernelTests` drives the real bridge from Swift (a DM lobby, a join that starts it, a raise, a call, the reveal, the next round, and the top of the table) and checks the model against its own oracle; every test added here was seen red on a mutation (`ios/TESTS_MUTATED.md`, M12 to M20).
- Inside Messages on an iPhone 17e simulator (iOS 27.0), one simulator playing both people: a lobby, the invitation, a join, a start, two raises, a call, the reveal with its outcome line, and the next round opened by the loser, each step seen and shot; then a second whole game to a winner, eight rounds and 18 bubbles.
  Every sent link of that game was decoded with `c/tests/cn_link_dump.c`, and the dice on the 16 screens that show a player's own dice agree with the kernel's die for die.
  The record is [docs/SIM_VERIFICATION.md](docs/SIM_VERIFICATION.md), the shots are `docs/shots/tie_*.png`, and the decoded game is `docs/SIM_GAME_LOG.txt`.
- `foolish/e2e/validation/shared_is_shared_validation.test.ts` refuses "Chui Niu" and "chuiniu" under `shared/` too (3 tests, 0 failed).

## What is NOT verified

- No real phone: the insert-after-the-drawer-is-up rule, the collapse after a raise and the tap-to-open were seen on a simulator only.
- Three to six seats were played only by the C tests (`cn_twophone_test` at three, the fuzz at two to six); on screen only two seats, light appearance, English.
- The name gate: a device with no nickname creates a lobby as "Player 1" (DECISIONS I14); on the simulator every name came from `dev.seat`, never typed.
- Leave, the race between two bubbles of one game (Rule P) on screen, a cancel with Messages' X, and a link the kernel refuses (the unreadable screen) were not driven on the simulator; the kernel's side of each is in `cn_msg_test` and the bridge smoke.
- The roll of a seat's own new dice runs on `RollBeats`, not on the kernel's frame (I10); only the reveal is driven by `cn_api_beats_frame`, and that driving has no automated test of its own.
- The `chuiniu` addition to the shared-is-shared test was not mutation-checked: making it go red means putting the name in a file under `shared/`, which this package may not touch.
- `There were 0`: the kernel's count line says a digit for none (K11), seen on the last screen of the game and left for the kernel's owner.

## Legal

Read [LEGAL.md](LEGAL.md) before naming, theming or drawing anything.
