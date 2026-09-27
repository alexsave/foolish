# Pick 'Em Up - reuse audit of foolish and uttt, and the lift plan into shared/

Audited on 2026-09-26 at `a3dbd05c` (the commit that finished moving foolish into `foolish/`).
Every path is from the repository root unless it says otherwise.
Line numbers are as of that commit.

The short version:
foolish's card surface is all Swift and SwiftUI, and none of it is in `shared/` yet.
The C kernel draws no card and exports no layout (`foolish/ios/FoolishKit/DesignSystem/FCard.swift`, `foolish/ios/FoolishKit/Boards/MessageTableView.swift`).
What `shared/` already holds is the Messages plumbing uttt needed, the build and ship tools, and the rig's measurement half.
So pickemup can start today by copying.
The lifts below then take the copies away one small verified step at a time.

---

## 1. The rules every lift has to obey

**shared/ may not name a product.**
`foolish/e2e/validation/shared_is_shared_validation.test.ts` greps every file under `shared/` for `durak`, `foolish`, `werewolf`, `wolf`, `cards.foolish`, `group.cards`, `CFoolish*`, `Foolish*`, `CWerewolf*`, `.xcodeproj` and `MessagesExtension` (the `PRODUCT` list at its line 52).
Its third test also refuses a product copy of a file that already lives in `shared/` (the `shadowed` list).
`shared/README.md` gets around naming by using code names: CARDS, UTTT and THIRD.
Pickemup needs a code name of its own before it appears in that README (step S0).
The regex does not match `uttt` or `pickemup` today, so right now nothing stops either name from creeping into `shared/`.

**A shared C header is reached by a relative `#include`, never by `-I`.**
`foolish/e2e/validation/shared_headers_reachable_validation.test.ts` enforces this for `sha256.h` and `deal_rng.h`.
Its header explains why: five build systems compile the kernel, and a flag added to one of them is a flag missing from the other four.
Every new shared C file follows the same rule, and adding it to `SHARED_HEADERS` in that test is part of its lift.

**A Swift file moved into `shared/swift/` compiles into foolish with no project change.**
`foolish/ios/project.yml:346` gives FoolishKit the whole directory `../../shared/swift` as a source path.
So moving a FoolishKit file there is a `git mv` plus rewording its comments.
The type stays in the same module and no call site changes.
uttt instead names its shared files one at a time (`uttt/ios/project.yml:167-183`), so pickemup should do the same.
Two consequences:

- Every file under `shared/swift` is compiled into FoolishKit, so only move a file there once foolish itself uses it.
- **Never put a resource (`.jpg`, `.png`) under `shared/swift`.** xcodegen would add it to FoolishKit as a resource, and it would collide with the file of the same name in `foolish/ios/FoolishKit/Resources/`.
Resources stay inside each product, and each product bakes its own from the shared generator.

**Swift that computes geometry is the pattern's known debt, not its goal.**
`uttt/ios/README.md` ("What Swift does not do") and `docs/ARCHITECTURE_AS_A_PATTERN.md` put coordinates in the kernel.
foolish's board is the exception, measured in section 4.
Where a lift has to parameterise a layout anyway, the plan moves the numbers into C (`shared/c/`) instead of into shared Swift.
That is the owner's standing rule to prefer C over a higher-level language.

**uttt's extension never loads SwiftUI, and foolish's is SwiftUI throughout.**
`uttt/ios/Tools/ship.env:21-22` sets `SHIP_FORBID_FRAMEWORKS=SwiftUI` ("The extension's process never loads SwiftUI", TESTFLIGHT_PLAN 14).
Every foolish view candidate below is SwiftUI.
Pickemup has to pick one side before any view lift happens; see Fork F1 in section 8.

---

## 2. The proof that foolish is unchanged

Run these from `foolish/` in the lift's worktree.
They are listed from cheapest to dearest.

| Id | Command | What it proves | Output that must not change |
| --- | --- | --- | --- |
| P1 | `make -C c tests && make -C c tests-asan` | the 200-test kernel binary (`build/cnitro_tests`), plain and under ASan+UBSan | - |
| P2 | `make -C c difftests` | the differential suites, `tests/anim_plan_test.c` (1440 lines) and `msg-flow-sim` (the Swift lobby decisions, Monte-Carlo over the real kernel, `foolish/c/Makefile:454`) | - |
| P3 | `make -C c ios-smoke` | every `fio_*` bridge entry point with the host compiler, plus `ios/split_check.sh` | - |
| P4 | `make -C c ios-goldens && git status --short -- ios/Fixtures` | the bridge rewrites `ios/Fixtures/goldens.json`, `action_goldens.bin` and `table_goldens.bin` (`foolish/c/Makefile:479-500`) | **the status must print nothing.** This is the same check `.github/workflows/ios.yml` job `bridge-linux` runs. |
| P5 | `bash ios/scripts/lint_architecture.sh` | the module import rules | - |
| P6 | `bash tools/structgen/gen.sh --check` | the generated Swift/TS still matches the C, generated twice and compared | - |
| P7 | `npm run test:validate` | the derived gates in `e2e/validation/`, including `shared_is_shared` and `shared_headers_reachable` | `db_validation` needs the Postgres `validate.yml` starts (stress/stress/foolish on :5432) |
| P8 | `DEST='platform=iOS Simulator,name=iPhone 17e,OS=27.0' bash ios/scripts/mac_tests.sh --no-lib` | see below | the snapshot PNGs under `ios/FoolishTests/__Snapshots__` |
| P9 | `git status --short -- ios/FoolishKit/Resources` | the baked felt, wool, wood and fern JPEGs | must print nothing |

When a lift touches `shared/c`, also run `make -C uttt/c run asan ios-smoke` (P10) and `make -C werewolf/c tests tests-asan` (P11).
Those are the other products that compile it.

### What `mac_tests.sh --no-lib` covers (P8)

The script is `foolish/ios/scripts/mac_tests.sh`.
It `cd`s to `foolish/` (`dirname $0/../..`) and does five things:

1. `bash tools/structgen/gen.sh` regenerates `sdk/swift/gen`.
2. `--no-lib` skips `make -C c ios-lib`.
It refuses to run if `ios/vendor/Foolish.xcframework` does not exist.
Drop `--no-lib` for any lift that changes C.
3. It runs `xcodegen generate` only if `ios/project.yml` is newer than the `.pbxproj`, or if `--regen` is passed.
Around that run it backs up every tracked `*.entitlements` with `cp -p` and restores it the same way, keeping bytes and mtime.
4. It runs `xcodebuild test` on scheme `Foolish` (FoolishTests, which the header counts as 513 cases) and on scheme `FoolishHarness` (HarnessTests, 24 cases).
It builds the shipping scheme `FoolishMessagesApp`.
5. If the build hits Xcode's "was modified during the build" error, it deletes only `XCBuildData` and retries once.

**It writes no committed goldens.**
The only reference files it touches are the `ComponentSnapshotTests` PNGs, which are git-ignored (`foolish/.gitignore:68`).
On the first run in a fresh checkout it records them, so that run fails, and it compares from the second run on (`foolish/ios/README.md:52-54`).
`EngineGoldenTests` and the packed-wire tests read `ios/Fixtures/*`, but P4 is what writes those.

**The visual zero-diff procedure for any Swift view lift:**

1. In the lift's worktree, before touching anything, run P8 twice: once to record, once to confirm green.
2. Make the lift.
3. Run P8 again.
Any snapshot diff fails the step.

The references are per-machine, so they have to be recorded from the baseline in that same worktree.

The test counts disagree.
The comment at the bottom of `.github/workflows/ios.yml` says 454 (430 + 24), and the script header says 513 + 24.
Take the count from the run, and make sure the count after the lift equals the count before.

---

## 3. The inventory

Classes:

- **(a)** already in `shared/`.
- **(b)** product-neutral and liftable as-is.
- **(c)** liftable once parameterised.
- **(d)** product-specific; copy it and adapt it.

"Risk" means the risk that the lift changes foolish's behaviour.
"Proof" names the rows in section 2.

### 3.1 Already in shared/ (a)

| Item | Where | Who uses it | Pickemup use |
| --- | --- | --- | --- |
| Deal RNG, SHA-256 | `shared/c/deal_rng.{c,h}`, `shared/c/sha256.{c,h}` | foolish, werewolf, uttt (sha only) | seeded deck and reshuffle; the chain digest |
| base32 | `shared/c/b32.{c,h}` | uttt | the bubble URL payload |
| Language registry | `shared/c/i18n/languages.h` | foolish (datagen), uttt (compiled in, `uttt/c/src/uttt_lang.c:2`) | the kernel's strings |
| Insert gating, including the "an insert before the drawer is up is silently dropped" fix | `shared/c/msg_stage/msg_stage.h` (`ms_drawer_up` :38, the `ms_stage_*` loop, `ms_receive` for own echo); Swift face `shared/swift/MessagesKit/InsertStaging.swift` | uttt only: `uttt/ios/UtttMessages/MessagesViewController.swift` (`InsertStaging.Loop` :49, `drawerUp` :240, `receive` :323). foolish compiles it and calls none of it; its `stage` does a bare `conversation.insert` (`foolish/ios/FoolishMessages/MessagesViewController.swift:968`) | use as uttt does |
| Drawer auto-collapse on Core Animation layers | `shared/swift/MessagesKit/CollapseSlide.swift` | uttt (`uttt/ios/UtttKit/UtttSheetView.swift:290`) | UIKit path only |
| Staged-but-unsent Send arrow | `shared/swift/MessagesKit/SendHint*.swift` | both | as-is |
| DEBUG dev-file flags (`flag(key, shipping:)`) | `shared/swift/MessagesKit/DevFlags.swift` | both (`MessageDevBoard`, `uttt/ios/UtttKit/UtttDev.swift:33`) | as-is |
| Fixed-layout byte reader | `shared/swift/PackedBytes.swift` | foolish | as-is |
| Motion ruler | `shared/c/motion_ruler/`, `shared/swift/MotionRuler.swift` (UIKit), `shared/tools/motion/` | uttt paints the Swift one; foolish paints its own SwiftUI ruler (`foolish/ios/FoolishKit/Messages/CollapseRuler.swift`) from the same C constants | as-is |
| Generators and toolchain | `shared/tools/structgen/`, `datagen/`, `sgcommon/`, `llvm.mk`, `shared/scripts/ci_llvm.sh` | foolish, werewolf | the bridge types |
| Ship, TestFlight, release scan, device logs, device capture | `shared/tools/ship/ship.sh`, `shared/tools/asc/testflight.py`, `shared/tools/release_strings.sh`, `shared/tools/devlogs.sh`, `shared/tools/devcap/` | both, each through its `ship.env` / `asc.env` | copy uttt's 3-line `uttt/ios/Tools/ship.sh` wrapper plus the two env files |
| Rig measurement half | `shared/rig/lib/*` (ax, mse, squares, window, ...) | both, through `foolish/ios/Tools/rig/rig.sh:100` (`SHLIB`) | as-is |
| UI doc checker, store frames | `shared/tools/check_ui_doc.py`, `shared/tools/store/market.py` | uttt, pickemup | as-is |

That is 13 items.

### 3.2 Card visuals, hand, table, felt

| Item | foolish source | uttt | Class | Parameters | Risk | Proof |
| --- | --- | --- | --- | --- | --- | --- |
| Texture bakers: felt, wool, wood, fern back | `foolish/ios/FoolishKit/DesignSystem/{FeltTexture,WoolTexture,WoodTexture,FernCardBack}.swift`, `foolish/ios/Tools/GenerateTextures.swift`, `foolish/ios/Tools/regenerate_textures.sh`; felt palette `FeltTexture.swift:97,109` (classic 0x286646, dark 0x143827) | none; werewolf has a third copy in `werewolf/ios/Tools/` | **(b)** once the macro `FOOLISH_TEXTURE_BAKE` is renamed and the comments are reworded | output directory (already `$1` in the script) | low; the shipped JPEGs are committed and nothing re-bakes them | P8, P9 |
| Design tokens: `FSpace`, `FRadius`, `FMotion`, `Color(hex:)` | `foolish/ios/FoolishKit/DesignSystem/Tokens.swift` (169 lines, no product name) | none | **(b)** | - | low | P8 |
| Square buttons | `foolish/ios/FoolishKit/DesignSystem/FSquareButton.swift` (side 40, :38) | none | **(b)**; two comments need rewording | - | low | P8 |
| Texture loader and table background | `foolish/ios/FoolishKit/DesignSystem/FTextures.swift` (`Bundle(for: BundleToken.self)` :248), `Materials.swift` (`TableBackground`, `TableWeave`) | none | **(c)** | the preference source (`FPrefs.shared`, Materials.swift:63) becomes an argument; the variant list | low | P8 |
| Card frame: rounded rect, ink, edge, back, thin-face rule | `FCard.swift`: radius `min(5, w*0.1)` :158, `thin` below 40pt :159, `deepRed` 0x8B1A1A :133, `edge(...)` :271, back :292-308 | none | **(c)** | the content view (the Durak overlays at :183-187 become an injected view), thin threshold, edge colours, back image name, card size | medium; the snapshot tests draw it | P8 |
| Card face content (rank and suit) | `FCard.swift` `centerGlyph` :209, `corner` :221, `thinCenter` :238; `foolish/sdk/swift/Models.swift` `Suit.glyph`, `CardRank.label` :49 | none | **(d)** | - | - | - |
| Hand row geometry and reorder | `foolish/ios/FoolishKit/DesignSystem/FHandFan.swift` (968 lines): `cardH 72` :232, `maxCardW 52` :240, `gap 4` :241, `rowH 80` :242, `rowGap 6` :244, `twoRowThreshold 34` :252, `rowCount` :288, `slotFrames` :378, `reorder` :614; drag `DragGesture` :819 | none | **(c)** | card identity (it uses `Card` 18 times), the trump flag (:27, :768), the order store (`MessageGameStore.setHandOrder`), the numbers above. Recommended: move the pure geometry to C | medium | P2, P8 (HandOrderFlightTests) |
| Which cards are laid out (kernel side) | `anim_fan_cards` / `anim_hand_laid_out`, `foolish/c/src/anim_plan.c:1314-1382`; bridge `fio_fan_cards` etc., `foolish/c/ios/ios_api_anim.c:496-521` | none | **(c)** | the card id range (dense 0..51) | medium | P1-P4 |
| Drop hit-testing | `foolish/ios/FoolishKit/Boards/BoardDrag.swift` (`BoardDrop.target` :84, slack 8) | none | **(c)** | the target enum (`.battle(i)` becomes a generic zone id) | low | P8 |
| Drag-to-play verbs | `foolish/c/src/legal.c:630-724` (`play_resolve`, `play_human_menu`...), bridge `foolish/c/ios/ios_api_play.c:171,212` | none | **(d)**; the pattern (a C menu export that the gesture asks) is what carries over | - | - | - |
| Seat ring | `foolish/ios/FoolishKit/Boards/MessageTableView+Seats.swift:85` (`ringPoint`, rx 0.42, ry 0.35 + 0.03 x collapse) | none | **(c)** | seat count, radii, local-seat rotation. Recommended: move to C | medium | P8 |
| Seat badge | `foolish/ios/FoolishKit/Boards/FSeatBadge.swift` (name max 96 :224, mini fan 28x40, role row `FRoleMark.rowHeight` 40 :414) | none | **(c)** | the role-row content becomes a slot (pickemup's LAST stamp) | medium | P8 |
| Deck well | `foolish/ios/FoolishKit/Boards/FDeckWell.swift` (92 x 108 :355, 46x66 backs, lean 1/2 :74-75, `maxLayers 8` :97, trump slot :186, bare trump glyph :322-347) | none | **(c)** | `hasTrump`, which turns the flipped card and the glyph off. The ink footprint then drops 94 -> 54, as `pickemup/README.md` measures | medium | P8 (DeckWellTests) |
| Discard pile | `foolish/ios/FoolishKit/Boards/FDiscardPile.swift` (78x68, tilt 20 degrees) | none | **(d)**; pickemup's centre pile is face-up and is the pile you match against | - | - | - |
| Battle grid and throw-in slide | `foolish/ios/FoolishKit/Boards/FBattleGrid.swift` (62x84, 3 columns, `slideByDefault` ~:102) | none | **(d)**, replaced by one pile | - | - | - |
| Pills and action bar | `foolish/ios/FoolishKit/Boards/FActionBar.swift` (`pillWidth 96` :18, `pillTrailing 16` :157), `DesignSystem/FButton.swift:44` | none | **(c)**; the slot arithmetic is generic, the verbs are Durak | verb list | low | P8 |
| Board placement, including the inset 8/8/14/4 | `MessageTableView.swift:705,755,848` (inset), :1165-1220 (seats, deck, discard) | none | **(d)** | - | - | - |
| Felt as the default surface | `foolish/ios/FoolishKit/DesignSystem/FPrefs.swift:99` | none | **(d)** | - | - | - |

That is 3 (b), 9 (c) and 6 (d).

### 3.3 Animation

| Item | foolish source | uttt | Class | Parameters | Risk | Proof |
| --- | --- | --- | --- | --- | --- | --- |
| Beat clock: start/gap/hold layout, `anim_plan_at` sampling, count-freeze, veil bitsets, surface plan | `foolish/c/src/anim_plan.{c,h}` (`ANIM_TIME_MS 500`, `ANIM_GAP_MS 25`, `anim_plan_at` c:451, `anim_veil_*` c:1225-1282, `anim_surface_plan` c:1574) | its own model, `uttt/c/src/uttt_anim.{c,h}` (`uttt_motion_at`) | **(c)**: split the generic half into `shared/c/` | event vocabulary, card-id range, seat cap (`FIO_PLAN_SEATS 8`) | **high**; 1642 lines, and the web wasm reads the same plan | P1-P4, P6, P7, P8 |
| Durak event planning: `beat_shape` COVER grouping, `apply_forward`/`apply_undo`, roles, optimistic cover | `anim_plan.c:82-215`, `:564` | - | **(d)**; copy the shape and write shedding events | - | - | - |
| Swift flight engine: `Flight`, `BoardAnimator.play`, `FlyingCardsLayer`, frame preference keys | `foolish/ios/FoolishKit/Boards/BoardFlight.swift` (530 lines; `flightTime` :20 reads `MessageDevBoard.slowmo` :26; curve `timingCurve(0.25,0.46,0.45,0.94)`) | none (uttt has no card flights) | **(c)** | the anchor set (deck, discard, battle, hand, seats), the slowmo source (through `DevFlags`), the duration (it should come from C, see D3) | medium-high | P8 (FlightRecorderTests, FlightNamespaceInvariantTests) |
| Per-event flight recipes (deal, refill, pickup, discard) | `foolish/ios/FoolishKit/Boards/MessageTableView+OpenReplay.swift:22`, `+Sequence.swift` (`runEventStream` :23, beat loop :277), `+BoutEnd.swift` | - | **(d)**; the deal recipe (deck -> hand slot, backs to badge offset k x 3pt) is the template for pickemup's deal | - | - | - |
| Drawer collapse numbers and tween | `foolish/ios/FoolishKit/Messages/CollapseTween.swift` (`hostResponse` 0.338 :72, `slideDuration` 0.6 :171), `CollapseDriver.swift`, `CollapseLayer.swift` (key `cards.foolish.collapse.layer` :155) | a C port in `uttt/c/src/uttt_anim.c` (`UTTT_COLLAPSE_MS 600`, `UTTT_DRAWER_RESPONSE_MS 338`) plus shared `CollapseSlide.swift` | **(c)**; three copies of one set of numbers (see D2) | none; the numbers are the host's | medium | P8 (CollapseTween/Driver/Curve/Layer/RulerTests, CompactRestHeightTests) |

That is 3 (c) and 2 (d).

### 3.4 Messages, lobby, identity

| Item | foolish source | uttt | Class | Parameters | Risk | Proof |
| --- | --- | --- | --- | --- | --- | --- |
| Extension lifecycle (adopt on select, Rule P on receive, stage, commit on `didStartSending`, cancel, `awaitTransitionSettled`) | `foolish/ios/FoolishMessages/MessagesViewController.swift` (1197 lines: `didSelect` :178, `didReceive` :271, `didStartSending` :414, `didCancelSending` :587, `awaitTransitionSettled` :682) | re-implemented in `uttt/ios/UtttMessages/MessagesViewController.swift`, on the shared `InsertStaging`; `awaitTransitionSettled` :1106 is a copy | **(d)** for now; copy **uttt's**, which uses the shared gating. The long-term base class is `werewolf/COMMON.md` item 6 | - | - | - |
| MSMessage composing | `foolish/ios/FoolishMessages/MessageComposer.swift:20-34` (`MSMessage(session: session ?? MSSession())`, template layout, `summaryText`) | `uttt/ios/UtttKit/UtttBubble.swift:202` | **(b)**; 15 lines with no product in them. Low value on its own | - | low | P8 |
| One MSSession per game | MVC :928-951, `freshSession` :36 | `sessionFor` :817 | **(d)**, a pattern | - | - | - |
| App Group nickname | `foolish/ios/FoolishKit/Messages/MessageGameStore.swift` (suite from Info.plist key `FoolishAppGroup` :121, default `group.cards.foolish.msg`; key `fmsg.nickname` :149) | none (uttt has no names) | **(c)**: the nickname slice only | suite name, key prefix | low | P8 |
| Nickname verdict and name clash | `msg_nickname_verdict` (`foolish/c/src/msg_wire.c:989`, 16 chars), `msg_name_taken` :1003 | none | **(c)**: lift together with seat identity | max chars | medium | P1-P4, P8 |
| Seat identity | `msg_seat_resolve` / `_claimed_by_name` / `_cache_disowned` / `_on_board` / `_in_lobby`, `foolish/c/src/msg_wire.c:1009-1104`; Swift `foolish/ios/FoolishKit/Messages/SeatIdentity.swift` | own tag model (`uttt/c/src/uttt_msg.h` `utm_resolve` :202); werewolf copied it "unchanged in behaviour" into `werewolf/c/src/ww_seat.{c,h}` | **(c)** over a `{seat, name}` row, as `werewolf/COMMON.md` item 1 describes | the row type | medium | P1-P4, P8, P11 |
| Lobby rules | `msg_lobby_offered` / `_can_exit` / `_can_set_rules` / `_rules_changed` / `_controls`, `msg_wire.c:1124-1163`; min 2 is hardcoded at `:131` and `:272`; `MAX_PLAYERS 8` in `foolish/c/src/game.h:12` | none; werewolf has `ww_lobby.h` with `TOO_FEW` and min 5 | **(c)** | `(min_players, max_players)`, whether a rules toggle exists (the passing checkbox is Durak), a `TOO_FEW` verdict | medium-high | P1-P4 (msg-flow-sim), P8 |
| Lobby and name-gate screens | `foolish/ios/FoolishKit/Messages/LobbyScreens.swift` (`LobbyView` :144 carries the passing toggle, `NameGateView` :612, `SeatPicker` :679), `NicknameGate.swift` | `UtttLobbyScreen.swift` | **(d)**; copy it and delete the passing row | - | - | - |
| Name-entry expand retry | `foolish/c/src/msg_expand.h` (`MSG_EXPAND_MAX_RETRIES 2`) | none | **(b)**; no product state in it | - | low | P1, P8 |
| Envelope header and Rule P | `foolish/c/src/msg_wire.{c,h}` (`msg_rule_p` c:707, `msg_chain_key` c:681; the header has Durak's variant byte at offset 16) | uttt "shaped like foolish's msg_wire... NOT sharing a byte" (`uttt/c/src/uttt_msg.h:1-6`); werewolf copied it (`ww_wire.h`, `ww_rule_p` :216) | **(c)**, with a 3-function body vtable (`werewolf/COMMON.md` item 3) | body codec, the meaning of `round` | **high** | P1-P4, P7, P8, P11 |

That is 2 (b), 5 (c) and 3 (d).

### 3.5 Build, test, ship, CI

| Item | Source | uttt | Class | Parameters | Risk | Proof |
| --- | --- | --- | --- | --- | --- | --- |
| xcframework slice recipe (device arm64, sim arm64 + x86_64 lipo'd, `-create-xcframework`) | `foolish/c/Makefile:256-386` (two libraries, `SG_LAYOUT_HASH`, `archive_check.sh`) | `uttt/c/Makefile:196-245`, the same skeleton with one library | **(c)** as `shared/tools/ios_xcframework.mk` | library name, sources, CFLAGS, headers dir, minimum iOS, output dir | medium for foolish; low if only uttt and pickemup adopt it first | `nm -g` of every slice before and after; P3, P8 without `--no-lib` |
| Mac test driver (gen, lib, xcodegen with the `cp -p` entitlements restore, the XCBuildData heal, test schemes, ship-scheme build) | `foolish/ios/scripts/mac_tests.sh` | **none; uttt has no mac_tests.sh and no Swift test target**; werewolf has its own copy (`werewolf/ios/scripts/mac_tests.sh`) | **(c)** as a shared library script | project path, test schemes, build scheme, lib command, pre-step | low | P8 before and after: same counts, and `git status` shows no entitlements change |
| Simulator rig driver | `foolish/ios/Tools/rig/rig.sh` (2258 lines; product block :106-143 with `RIG_*` overrides) | driven through `uttt/ios/Tools/rig.env` (11 exports) | **(c)** | `FOOLISH_SIM`/`_IDB`/`_OUT`/`_DD`/`_WORK` (:101-103,151,179) become `RIG_*`; `lib/seed.py:35` hardcodes `GROUP_ID`; `cmd_probe` hardcodes the label `Foolish` (:2195). The Durak commands (throwin, cover, deal, chain...) stay with foolish | medium; a device take is the only full proof | `python3 foolish/ios/Tools/rig/lib/test_rig.py` (its :440 pins identity strings to the product block), `rig.sh doctor` for both products |
| `project.yml` shape | `foolish/ios/project.yml` (606 lines) | `uttt/ios/project.yml` (210; Debug-only App Group through a committed `DebugAppGroup.entitlements` that xcodegen does not know about, :100-110) | **(d)**; copy **uttt's** | - | - | - |
| Kernel freshness preBuildScript | `foolish/ios/project.yml:369-398` | none | **(b)**; copy it into pickemup's project.yml | - | - | - |
| CI workflow shape | `.github/workflows/ios.yml` (bridge smoke, goldens freshness, lint), `werewolf.yml` (`make tests`, `tests-asan`, release gate), `validate.yml` | **uttt has no C lane**: only `uttt-web.yml`, which triggers on `uttt/c/**` but runs the web build | **(d)**; write `pickemup.yml` on werewolf's shape (see step S3) | - | - | - |
| Release gate (source half and binary half) | `werewolf/ios/scripts/release_gate.sh` | none | **(b)** as `werewolf/COMMON.md` item 7 describes | the symbol list | low | - |

That is 2 (b), 3 (c) and 2 (d).

### Totals

| Class | Count |
| --- | --- |
| (a) already in shared/ | 13 |
| (b) liftable as-is | 7 |
| (c) liftable after parameterising | 20 |
| (d) copy and adapt | 13 |

---

## 4. How foolish draws a card, and whether the face can be neutral

Everything visual is SwiftUI in FoolishKit.
C draws nothing on this surface.
uttt is the opposite: it computes every polygon in `uttt/c/src/uttt_draw.c` and `uttt_pen.c`.

- **One view draws both sides.** It is `FCard` (`foolish/ios/FoolishKit/DesignSystem/FCard.swift`).
A `nil` or hidden card draws the back (:163).
- **The face** is a `RoundedRectangle` with radius `min(5, w*0.1)` filled with `ink.face`.
Dark mode inverts the face (`Ink.light` / `Ink.dark`, :144-154).
- **The resting edge** is `deepRed` 0x8B1A1A on both sides in both schemes (:133).
It goes through the one ring builder `edge(selected:radius:resting:)` (:271): 1pt at rest, 4pt `selRed` 0xFF2A22 when selected.
- **There is no card shadow.**
- **The back** is a black fill with `FTextures.fernBack` clipped inside the same edge (:292-308).
`backSeed` is accepted but ignored, because there is one image: `foolish/ios/FoolishKit/Resources/fern-back.jpg`, baked by `FernCardBack.swift`.
- **The content** is three overlays at :183-187:
  - `centerGlyph`, the suit in Georgia bold at 0.62w (:209);
  - `corner`, rank 0.40w over suit 0.28w, inset 0.08w, with the bottom-right copy rotated 180 degrees (:221);
  - `thinCenter` below 40pt wide, rank and suit stacked at 0.56w with no corners (:238).
  `fullFace` (:39) turns the thin rule off.
  - The content hooks are `Suit.glyph`, `Suit.isRed` and `CardRank.label` in `foolish/sdk/swift/Models.swift` (:34, :49).
  - Trump reaches `FCard` only through the VoiceOver label (:312).
- **Sizes in use:**

| Where | Size |
| --- | --- |
| hand | 72 tall, width clamped 22-52 |
| battle | 50x70 |
| deck backs | 46x66 |
| discard backs | 44x62 |
| badge backs | 28x40 |

**So the frame is already neutral and the face is not.**
Split `FCard` into two parts:

- a `CardFrame`, which is shared: rounded rect, ink, edge, back, selection ring and the thin threshold;
- a content view that each product supplies.

Pickemup's content does not fit the corner-and-centre scheme:

- shape+colour suits (circle/teal, triangle/amber, square/violet, diamond/slate, `pickemup/README.md`), where the shape is the suit and has to survive the thin face;
- numbers 0-9;
- action glyphs.

That is a new content view, not a reskin of `corner` / `centerGlyph`.
The owner's rule points the glyph geometry at C: uttt's pen already emits filled polygons in a unit square, so a shape suit is four polygon lists.
Pickemup can draw its suits and action glyphs as C polygon lists that a thin SwiftUI (or CALayer) view fills, the same way `UtttBoardView` does.
That also makes the bubble snapshot and the live card the same drawing.

---

## 5. The ordered lift plan

Each step is one worker task in its own worktree with its own PR.
Each is small enough to verify completely, and none of them needs another's research.
The order puts the lowest risk and the highest value first.

**The rule for every step:**

- Baseline P1-P9 before editing.
- Record the P8 snapshots twice.
- Lift.
- Run the same proofs again.
- P4 and P9 must print nothing, and the P8 counts must match.
- Mutation-check any new test.
- foolish's own diff should be only paths: a `project.yml` source path, a Makefile `include`, a relative `#include`, or a script that now `exec`s a shared one.
If foolish's diff has to be anything else, stop and split the step.

### Step list

**S0 - guard first.**
- Add a code name for pickemup to `shared/README.md` (suggested: SHED).
- Add `/pickemup/i` and `/pick ?'?em ?up/i` to `PRODUCT` in `foolish/e2e/validation/shared_is_shared_validation.test.ts`.
- Reword `shared/README.md:47` (`pickemup/` in the check_ui_doc row) to the code name.
- Consider `\buttt\b` too.
That means rewording `shared/README.md:5` and every `uttt/` path in the README into UTTT.
- Mutation check: put `pickemup` in a comment under `shared/` and watch the test go red.
- foolish change: none.
- Proof: P7.
- Risk: none.

DONE (S0).
The code name is SHED.
`PRODUCT` gained `/pickemup/i` and `/pick ?'?em ?up/i`.
A second hit the guard found: `shared/tools/check_ui_doc.py` defaulted to `pickemup/docs/UI.html` when run with no argument, so it now prints its usage line instead (both READMEs that call it pass a path).
Mutation check: a file under `shared/` holding `pickemup`, then one holding `Pick 'Em Up`, each turned the guard red on the first test naming that line; removing it turned it green.
P7: 150 tests, 128 pass before and after with the same set of names; the 22 that fail or cancel are the Postgres suites (ECONNREFUSED on :5432, Docker was not running), identical before and after.
`\buttt\b` was NOT added: the code name UTTT is the product name, so the guard would first need a new code name for uttt and a reword of 32 lines, including comments in `shared/swift/MessagesKit` and `shared/c/i18n/languages.h`, and the path `uttt/ios/Tools/store_frames.py` in `shared/tools/store/market.py`.
That is its own step.

**S1 - texture bakers to `shared/swift/Textures/` and `shared/tools/textures/`.**
- `git mv` `FeltTexture.swift`, `WoolTexture.swift`, `WoodTexture.swift` and `FernCardBack.swift` from `foolish/ios/FoolishKit/DesignSystem/` into `shared/swift/Textures/`.
FoolishKit still compiles them through `project.yml:346`.
- Move `GenerateTextures.swift` and `regenerate_textures.sh` to `shared/tools/textures/`.
- Rename `FOOLISH_TEXTURE_BAKE` to `TEXTURE_BAKE` (FeltTexture.swift:140,245, FernCardBack.swift:42,213, the script's `-D`).
- Make the output directory a required argument.
- Reword the comments that name FoolishKit.
- foolish change: `foolish/ios/Tools/regenerate_textures.sh` becomes `exec ../../../shared/tools/textures/regenerate_textures.sh "$IOS/FoolishKit/Resources"`.
- Leave the JPEGs where they are.
- Proof: P8, P9, P7.
Optionally re-bake into a temp directory and `cmp` it against the committed JPEGs.
If the bake is not byte-identical on this machine, record that; do not commit a re-bake.
- Risk: low.
- Follow-up in the same PR: delete werewolf's third copy in `werewolf/ios/Tools/` only if werewolf builds from shared (paused; otherwise leave it and note it).

DONE (S1), except the after-run of P8, which is BLOCKED (see `ORCHESTRATION.md`).
The four generators are in `shared/swift/Textures/`, the bake tool and its script in `shared/tools/textures/`, and the flag is `TEXTURE_BAKE`.
The output directory is required by both the script and `GenerateTextures.swift`.
foolish's diff: `ios/Tools/regenerate_textures.sh` now `exec`s the shared script with `${1:-$IOS/FoolishKit/Resources}`, `ios/Tools/felt_variations.sh` points at the new paths and flag, and two comments (`IconGen/.../main.swift`, `fern_ifs.html`) name the new path.
Re-bake: the JPEGs baked before the move and after it through foolish's wrapper are all seven `cmp`-identical to the committed ones, so no re-bake was committed.
P9 prints nothing, and no entitlements changed.
P7: 150 tests, 128 pass, the same set as before S0 (the 22 others are the Postgres suites).
P8 baseline in this worktree: run 1 recorded the snapshots (853 executed, 7 snapshot records failing, as expected); run 2 executed 853 with 1 skipped and one failing test, `MemoryProfileTests.testMemoryProfileOfEverythingTheExtensionHolds` (bubble snapshots grew 8.6 MB over 20 renders against a 2 MB budget), which passed in run 1, so it is flaky and not caused by this lift.
HarnessTests did not run in either baseline, because the script stops at the first failing scheme.
P8 after the lift never reached a test: from 23:06 on, every simulator on this Mac hung (a test launch died with `Mach error -308 (ipc/mig) server died`, `simctl install` hung, and a freshly created device and the iOS 26.3 device both stuck at boot in `com.apple.addressbook.migrator`), and restarting CoreSimulatorService did not clear it.
In its place: `xcodebuild -scheme Foolish -destination 'generic/platform=iOS Simulator' build-for-testing` succeeds, and the built `FoolishKit` exports 426 symbols of the four texture types and none named `renderCGImage`, so the generator half stays out of the shipping framework.
Werewolf keeps its own copy in `werewolf/ios/Tools/` (werewolf is paused); its scripts still compile its own sources under `-D FOOLISH_TEXTURE_BAKE`.
Found on the way: `mac_tests.sh` regenerates the project only when `project.yml` is newer than the `.pbxproj`, so a checkout that already has a generated project needs `--regen` after this move, or it builds against the old paths.

**S2 - design tokens and the square button to `shared/swift/DesignKit/`.**
- `git mv` `Tokens.swift` and `FSquareButton.swift`.
- Reword the two comments in FSquareButton (:9, :89).
- foolish change: none.
- Proof: P8, P7.
- Risk: low.

NOT DONE (S2): neither file is product-neutral, so a `git mv` would put code into `shared/` that only compiles inside FoolishKit.
`Tokens.swift` takes foolish's `Suit` in `FColor.suitColor` (it reads `suit.isRed`), and its header and colours are the card game's identity ("Gosizdat Card Table", "Soviet red").
`FSquareButton.swift` calls `Haptics`, `WoodFill`, `FPressStyle` and `onWoodText`, and its `SettingsHelpSquares` reads `FPrefs`, `FStrings` keys and `FActionBar.innerInset`; its `:89` comment names the Durak rules redesign.
Making either neutral means splitting declarations out of a foolish file, which is more than the path-only diff foolish is allowed in a lift.
The split this step needs: a neutral `shared/swift/DesignKit/Tokens.swift` holding `FSpace`, `FRadius`, `FMotion`, `FType` and `Color(hex:)`, with `FColor` and the text modifiers staying in foolish; and the square button taking its surface, press style and haptic as arguments.
That is a code change in foolish, so it goes in its own reviewed step with P8 run before and after on a working simulator.
Until then pickemup copies the numbers it needs.

Commits: S0 `24372df8`, S1 `c3d99192`.

**S3 - pickemup's own CI lane and a uttt C lane (no lift, but it protects every lift after it).**
- Write `.github/workflows/pickemup.yml` on the shape of `.github/workflows/werewolf.yml`.
It triggers on `pickemup/**`, `shared/c/**` and itself, and runs `make -C c tests tests-asan ios-smoke` from `pickemup/`.
- Add a `uttt/c` lane: `make -C c run asan ios-smoke`.
Today no workflow runs uttt's C tests (`uttt-web.yml` only builds the site), so a shared/c lift can break uttt and stay green.
- foolish change: none.
- Proof: the lanes go red on a deliberately broken assertion, then green.
- Risk: none.

**S4 - the shared xcframework recipe, `shared/tools/ios_xcframework.mk`.**
- One `define` taking `(name, sources, cflags, headers dir, min iOS, out dir)`.
It emits the device slice, the two simulator slices, the lipo and `-create-xcframework`, lifted from `uttt/c/Makefile:204-236`.
- Adopt it in uttt and pickemup.
- Adopt it in foolish in a **separate** step S4b.
It calls the macro twice (core and bots) and keeps `SG_LAYOUT_HASH` and `archive_check.sh` around it.
- foolish change (S4b): an `include ../../shared/tools/ios_xcframework.mk` plus two calls.
- Proof:
  - `nm -g` sorted, and `lipo -info`, for every slice before and after, which must be identical;
  - `make -C c ios-smoke ios-archives`;
  - P8 without `--no-lib`.
- Risk: low for S4, medium for S4b.

**S5 - the shared Mac test driver, `shared/scripts/ios_mac_tests.sh`.**
- Move the generic body of `foolish/ios/scripts/mac_tests.sh` into a script driven by a product env:
  - the entitlements backup and `cp -p` restore;
  - the xcodegen trigger on `project.yml` mtime;
  - `run_scheme` and `unpoison_derived_data`.
- The env names `PROJECT`, `TEST_SCHEMES`, `BUILD_SCHEMES`, `LIB_CMD`, `PRE_CMD` and `XCFRAMEWORK`.
- foolish change: `mac_tests.sh` sets those and `exec`s the shared one.
Keep the long comment next to the code it explains, which means in the shared file.
- Give uttt and pickemup a `mac_tests.sh` the same way.
- Proof: P8 before and after, with identical counts and `git status --short -- '*.entitlements'` empty; run `--regen` once to exercise the restore.
- Risk: low.

**S6 - rig parameterisation, in place.**
- In `foolish/ios/Tools/rig/rig.sh`, rename `FOOLISH_SIM` / `_IDB` / `_OUT` / `_DD` / `_WORK` to `RIG_*`, keeping the old names as fallbacks.
- In `lib/seed.py`, read `GROUP_ID` from `RIG_APP_GROUP`.
- Take the probe label from `RIG_MENU_NAME`.
- Replace `cmd_build`'s entitlements restore (`git checkout -- ...` at rig.sh:483) with the `cp -p` backup from S5.
The current line both poisons Xcode's build description and throws away uncommitted entitlements edits (defect D1).
- Make `uttt/ios/Tools/rig.env` derive its paths from its own location, not `/Users/alex/Dev/foolish/...` (:16, :24), so it works in a worktree.
- Moving `rig.sh` into `shared/rig/` is a later step, and needs the device verification that the header of `shared_is_shared_validation.test.ts` calls for.
- foolish change: none outside the rig.
- Proof: `python3 foolish/ios/Tools/rig/lib/test_rig.py`, then `rig.sh doctor` for foolish and for uttt on ONE simulator, shut down afterwards.
- Risk: medium.

**S7 - one source for the drawer-collapse numbers (defect D2).**
- Move uttt's C port (`uttt_collapse_push`, `uttt_spring_left` / `_past`, `UTTT_COLLAPSE_MS`, `UTTT_DRAWER_RESPONSE_MS` in `uttt/c/src/uttt_anim.{c,h}`) into `shared/c/collapse/` with a module map, the way `motion_ruler` is done.
- `shared/swift/MessagesKit/CollapseSlide.swift` and foolish's `CollapseTween.swift` both read their numbers from it.
- foolish change: `CollapseTween.swift` reads its constants from the C module (the include path is already on `SWIFT_INCLUDE_PATHS`, `foolish/ios/project.yml:36`, once the new directory is added there).
- Proof: P8 (the CollapseTween/Driver/Curve/Layer/RulerTests and CompactRestHeightTests must stay green unchanged), P10.
- Risk: medium.

**S8 - seat identity to `shared/c/msg_seat.{c,h}`.**
- Lift `msg_name_taken`, `msg_nickname_verdict` and `msg_seat_*` (`foolish/c/src/msg_wire.c:989-1104`) over a `{seat, name}` row.
- foolish change: `msg_wire.c` keeps thin forwarders, or calls the shared functions through `#include "../../../shared/c/msg_seat.h"`.
Add `msg_seat.h` to `SHARED_HEADERS` in `shared_headers_reachable_validation.test.ts`.
Add `shared/c/msg_seat.c` to `IOS_CORE_SRC` and to every build tree the header of that test lists.
- Werewolf's `ww_seat.c` switching over is a separate step.
- Proof: P1-P4 (msg-flow-sim included), P7, P8 without `--no-lib`, P11 if werewolf switches.
- Risk: medium.

**S9 - lobby rules to `shared/c/msg_lobby.{c,h}`.**
- Parameterise by `(min_players, max_players, has_rules_toggle)` and add the `TOO_FEW` verdict werewolf needed.
- Remove the hardcoded `n_players < 2` at `msg_wire.c:131` and `:272` in favour of the parameter.
- Proof: as S8.
- Risk: medium-high.

**S10 - card frame split.**
- Split `FCard` into a shared `shared/swift/CardKit/CardFrame.swift` (frame, edge, back, selection ring, thin threshold as a parameter) and foolish's Durak content view, which stays in FoolishKit.
- Blocked on Fork F1.
- Proof: P8 snapshots.
- Risk: medium.

**S11 - hand row geometry to C, `shared/c/hand_row.{c,h}`.**
- Lift `rowCount`, `rowSizes`, `singleRowCardWidth`, `slotFrames`, `slotIndex` and the reorder splice out of `FHandFan.swift`.
The numbers (72, 52, 22, 4, 80, 6, 34) become the call's arguments.
- `FHandFan` calls the C and keeps the gesture.
- Proof: P8 (HandOrderFlightTests and the snapshots), a new C test mutation-checked against the Swift values, P3.
- Risk: medium.

**S12 - seat ring and deck-well layout to C, `shared/c/table_layout.{c,h}`.**
- `ringPoint` (`MessageTableView+Seats.swift:85`), the deck-well layers and lean (`FDeckWell.swift`), and the pill slot arithmetic, with seat count and `has_trump` as arguments.
- Proof: P8.
- Risk: medium.

**S13 - flight engine.**
- `Flight`, `BoardAnimator`, `FlyingCardsLayer` from `BoardFlight.swift` to `shared/swift/CardKit/`.
- Slowmo moves through `DevFlags`, and the anchor set becomes generic.
- The duration comes from C (defect D3).
- Blocked on F1.
- Proof: P8.
- Risk: medium-high.

**S14 - beat clock to `shared/c/anim_clock.{c,h}`.**
- The generic half of `anim_plan.c`: the start / gap / hold layout, `anim_plan_at`, the veil bitsets and the surface plan.
- `anim_plan.c` keeps the Durak events.
- Proof: P1-P4, P6, P7 (`wasm_exports_validation`, `wasm_outputs_validation`), P8 without `--no-lib`, and the web lanes.
- Risk: high.

**S15 - envelope and Rule P to `shared/c/msg_env.{c,h}`.**
- The header parse, bounds, digest and Rule P, with a body vtable (`werewolf/COMMON.md` item 3).
- Proof: everything, P11 included.
- Risk: high.

**S16 - the extension lifecycle as a shared base.**
- `werewolf/COMMON.md` item 6.
- Only after three products run the same copy.
- Risk: high.

### What pickemup does NOT wait for (copy first, lift later)

Pickemup needs nothing lifted to start.
Mark every copied file with a first line `// COPIED from <path> at a3dbd05c - replaced by lift step Sn`, so the lift that owns it can `grep -r "COPIED from" pickemup/` and delete the copy in the same PR.

**Start immediately by copying:**

- **Kernel.**
  - Link `shared/c/deal_rng.c`, `sha256.c` and `b32.c` by relative include; they are shared already.
  - Copy the shape of `uttt/c/Makefile` (targets `run`, `asan`, `ios-lib`, `ios-smoke`).
  - Copy uttt's `uttt_msg.{c,h}` shape for the envelope.
  - Copy the lobby and seat functions from `foolish/c/src/msg_wire.c:989-1163`; S8 and S9 replace them.
  - Model the animation plan on `anim_plan.h`, with shedding events, and use it as S14's second customer.
- **App.**
  - Copy `uttt/ios/project.yml` (the Debug-only App Group trick) and `uttt/ios/UtttMessages/MessagesViewController.swift` (it already runs on the shared `InsertStaging` and `CollapseSlide`).
  - Copy `uttt/ios/Tools/{ship.sh,ship.env,asc.env,rig.env}`.
  - Copy foolish's `FCard.swift`, `FHandFan.swift`, `FSeatBadge.swift`, `FDeckWell.swift`, `FActionBar.swift`, `BoardFlight.swift`, `BoardDrag.swift`, `Tokens.swift`, the texture bakers and `LobbyScreens.swift`.
  Do this only if F1 settles on SwiftUI.
  - Bake pickemup's own felt JPEG from its copy of the baker.
- **Tests.** Copy `foolish/ios/scripts/mac_tests.sh`; S5 replaces it.

**Must land before pickemup ships, not before it starts:**

- S0, so that no pickemup name leaks into `shared/`.
- S3, so that pickemup has CI.
- S1 and S2, so that the copies do not drift.

---

## 6. Gaps: what neither foolish nor uttt has

| Need | What exists nearest | What is missing |
| --- | --- | --- |
| **Manual multi-draw turn** (draw one at a time, until playable or by choice, then play or pass) | foolish refills from the deck automatically at bout end (`ANIM_EVT_REFILL`, `anim_plan.h:104-117`). The deck well is not a tap target. | A kernel action DRAW that can repeat within one turn, and whether a drawn card is playable. A tap-to-draw gesture on the deck well. A per-draw flight (the deal recipe in `MessageTableView+OpenReplay.swift:22` is the template). A settlement rule for a staged turn with N draws, which is more than foolish ever stages. |
| **Reshuffle animation** (the discard, minus its top card, becomes the new stock) | Flights exist table -> discard and deck -> hand. `FDeckWell.layers(for:)` only counts down. | A discard -> deck flight, the deck well counting up, a RESHUFFLE event in the plan, and a reshuffle seed derived from `deal_rng` so the replay stays deterministic. foolish never reuses a card. |
| **Colour picker** (a wild's choice) | Nothing. The nearest thing is the lobby's rules toggle. | A chooser showing the four shape+colour suits (shape first, for the colourblind reason in `pickemup/README.md`). Chosen-suit state in the kernel and in the bubble. The staged wild "drops its suit" on undo (`pickemup/docs/UI.html` motion grid). |
| **Call-out tap on a fan** ("Caught you" on an opponent's badge fan, open until the next bubble seals) | `FSeatBadge` draws a mini fan but is not a hit target. `BoardDrop.target` knows only battle, hand and table. | A seat-targeted action in the kernel, valid in a window of one bubble. A hit zone per badge. The only simultaneous-action rule in any product. It also needs a Rule P answer for two catchers. |
| **Round-robin deal** (one card to each seat in turn, 7 rounds) | foolish's deal and refill flights go per seat (`openReplayFlights`). The deal order rule is in the kernel ("defender draws last"). `FMotion.dealStagger` (0.04, `Tokens.swift:83-94`) exists and nothing uses it. | A deal plan that interleaves seats card by card with a stagger. A beat layout for 7 x N small flights inside one animation budget. |
| **Direction indicator** (clockwise or counter-clockwise; flips on reverse) | Nothing. The top-right corner is freed by deleting the discard pile (`pickemup/README.md`). | A glyph and its rotate animation, driven by kernel state. |
| Also surfaced by the study | | The one centre pile replacing the battle grid. The LAST stamp in the 40pt role row. Skip dim, +2 hand-off and turn-order consequences all settling at Send (the channel grid in `pickemup/docs/UI.html`). A hand that grows past 13 cards (thin face at 23.2pt, a 166pt box in a 340pt drawer, `pickemup/README.md`). A card face for 0-9 and action glyphs (section 4). |

---

## 7. Mac working rules found on the way

- **At most 2 booted simulators.**
Each one is hundreds of processes.
Around the eighth, the per-user process limit is hit and every shell returns a bare `Exit code 1`.
Reuse one named sim, `xcrun simctl shutdown` it when done, and check `xcrun simctl list devices booted` first.
Source: `~/.claude/projects/-Users-alex-Dev-foolish/memory/feedback_sim_process_limit.md`.
It is not written down anywhere in the repo, and `foolish/ios/Tools/rig/README.md` should say it.
- **xcodegen blanks the tracked entitlements files every run.**
It reports only "Created project".
  - The restore that works is `cp -p` of a backup, bytes and mtime, in `foolish/ios/scripts/mac_tests.sh` (`backup_entitlements` / `restore_entitlements`).
  - A `git checkout` restore leaves a new mtime.
  Xcode then fails every later build with "Entitlements file ... was modified during the build" until `XCBuildData` is deleted, which `run_scheme` heals once.
  `rig.sh:483` still restores with `git checkout` (D1).
  - uttt avoids the problem: its `DebugAppGroup.entitlements` is set through `CODE_SIGN_ENTITLEMENTS` in the Debug config, so xcodegen does not know the file exists (`uttt/ios/project.yml:104-110`).
  Pickemup should do the same.
  - The foolish memory note is `project_xcodegen_wipes_entitlements.md`.
- **A simulator build with no `DEVELOPMENT_TEAM` signs ad-hoc with an empty entitlements dict**, so the App Group, and with it the rig's flag files, does not exist (`werewolf/COMMON.md` item 10).
And `uttt/ios/project.yml:87`: an app with no entitlements does not register on a simulator.
- **uttt's test target is `make run`, not `make tests`.**
`uttt/c/Makefile` has no `tests:` target (`:57` is `run`; `asan` is :173).
The full uttt gate is `make -C uttt/c run asan ios-smoke`.
foolish and werewolf use `make -C c tests` and `tests-asan`.
Pickemup should use `tests` / `tests-asan`, matching the majority and `werewolf.yml`.
- **WASM_CC.**
`foolish/c/Makefile:564-569` defaults to `/opt/homebrew/opt/llvm/bin/clang` on a Mac when it exists, and `wasm-cc-check` refuses Apple clang.
uttt takes `$(WASM_CC)` from `shared/scripts/ci_llvm.sh` and falls back to `clang` (`uttt/c/Makefile:259-261`).
That fallback can silently use Apple clang, which is the drift foolish's check was written for.
- **`--regen` for a new Swift file.**
xcodegen only re-reads `project.yml` when it is newer, so a new `.swift` file is silently left out of the target until `mac_tests.sh --regen` runs (`werewolf/COMMON.md` item 10).
- **Stale xcframework symptoms.**
"cannot find ... in scope" means the xcframework is stale: delete `DerivedData/<Product>-*` and rebuild `ios-lib`.
"found architecture arm64, required architecture x86_64" means a one-arch simulator slice (`uttt/ios/README.md`).
- **Git-ignore `ios/vendor/*.xcframework`** (`werewolf/COMMON.md` item 10).

---

## 8. Forks and defects found

**F1 - SwiftUI or Core Animation for pickemup's extension.**
foolish's whole card surface is SwiftUI.
uttt's extension deliberately never loads SwiftUI (`uttt/ios/Tools/ship.env:21-22`).
Steps S10 and S13 lift SwiftUI views, and they only pay off if pickemup is SwiftUI.
Recommendation: settle it before any view copy.
If memory is the reason uttt dropped SwiftUI, measure foolish's extension footprint first (the iMessage memory investigation is in the owner's notes, `project_imessage_memory_hang.md`).
Either way, S11 and S12 put the geometry in C, so the geometry does not depend on the answer.

**D1 - `rig.sh` restores entitlements with `git checkout`** (`foolish/ios/Tools/rig/rig.sh:483`).
This is the mtime poisoning `mac_tests.sh` exists to avoid, and it also discards uncommitted entitlements edits.
S6 fixes it.

**D2 - the drawer-collapse numbers exist three times:**

- `foolish/ios/FoolishKit/Messages/CollapseTween.swift`;
- `shared/swift/MessagesKit/CollapseSlide.swift` (a port);
- `uttt/c/src/uttt_anim.c` (a C port).

S7 gives them one owner.

**D3 - flight timing is typed twice by hand.**
`ANIM_TIME_MS 500` and `ANIM_GAP_MS 25` are in `foolish/c/src/anim_plan.h:74-101`, and `flightTime` 0.5 and `flightGap` 0.025 are in `foolish/ios/FoolishKit/Boards/BoardFlight.swift`.
Nothing ties them together.
The fix is for Swift to read the C constants through the bridge, with a test that fails when they drift, as part of S13 or as a small step before it.

**D4 - foolish compiles the shared insert gating and does not use it.**
`foolish/ios/FoolishMessages/MessagesViewController.swift:968` inserts with no drawer-up gate, retry or door, while uttt runs the full `ms_stage_*` loop.
That is a behaviour change, so it is out of scope for a lift.
It is worth its own task, verified with the rig.

**D5 - no CI runs uttt's C tests.**
S3 fixes it.

**D6 - `uttt/ios/README.md` "Not done yet" is stale.**
It says there is no bubble, no sealing and no lobby, but uttt is in App Store review.

**D7 - the XCTest counts disagree** between `.github/workflows/ios.yml` (454) and `foolish/ios/scripts/mac_tests.sh` (513 + 24).
