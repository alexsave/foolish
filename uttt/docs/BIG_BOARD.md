# The 243 x 243 board

A hidden game mode for TESTFLIGHT BUILDS ONLY.
It exists to test `shared/swift/BubbleDataKit` on a second real device: the whole 243 x 243 board travels as a picture in the message bubble, and the URL carries only what a picture cannot.
Nothing about the shipped 9 x 9 game changes: its wire bytes, its screens, its kernel files.

## What it is

Recursive Ultimate Tic-Tac-Toe: a 9-ary tree of depth 5 where the shipped game is depth 2.
9^5 = 59,049 leaf cells.
A node is won by three of its nine children in a line held by one mark, drawn when all nine children are decided without a line; decided is forever and closes every leaf below it.
The root decides the game.
A move is a leaf index whose five base-9 digits run from the top block to the cell; the next move must go into the leaf-level block named by the last four digits, and when that block or anything above it is decided the target relaxes to the parent of the first decided node on its path from the root (the root means anywhere).
A first move is anywhere.
X moves first and X is the joiner, as in the 9 x 9 game.
The rules are stated once, in `uttt/c/src/uttt_big.h`, and `tests/uttt_big_test.c` holds depth 2 of the new module to the shipped kernel move for move.

## How to use it (TestFlight build)

1. Open any Ultimate bubble whose screen has the rules door (the rulebook button, bottom right): a game, somebody's invitation, or a game you are watching.
   The waiting screen of your own unsent or unanswered invitation has no rules door, 9 x 9 or big.
2. HOLD THE RULES DOOR FOR 4 SECONDS. A haptic fires (success) and a "243" badge appears in the bottom left corner of the drawer: the mode is on.
3. Open Ultimate from the + menu. The invitation it stages is a 243 x 243 game; every bubble of that chain is a 243 x 243 game, and a rematch (Again) of a big game is a big game.
4. On the board: pinch to zoom, pan when zoomed, double-tap to zoom in by three (and back out to the whole board from the deepest zoom), tap a cell to stage a move, tap another cell to change your mind. The yellow wash is where you must play; the last move is outlined; your staged move is outlined as a draft. Messages' X on the staged bubble takes the move back, as in the 9 x 9 game.
5. Hold the rules door for 4 seconds again to leave the mode (a warning haptic, and the badge goes from a 9 x 9 screen). The mode changes only what CREATING a game does; it does not touch games already in the thread. A big game's screen keeps the badge whatever the mode, because it says what board this is.

The mode persists in the extension's own defaults (`UserDefaults.standard`, key `uttt.big.mode`, as UtttSeats keeps its records) until it is toggled off, and survives an update of the app.
It is never on where the big game is unavailable (`UtttBig.available`).

### The hold, exactly

The rulebook has ONE long-press recogniser, at the shorter of the holds it has (`UtttRulebookButton.setLongHold`, UtttKit/UtttInk.swift):

- TestFlight (Release with the feature): only the 4-second hold. It fires the moment 4 s is reached.
- Debug: the diagnostics hold (1.5 s) is there too. The recogniser stays at 1.5 s and the 4 s clock starts when it recognises. Reaching 4 s toggles the mode and the diagnostics never open. Letting go between 1.5 s and 4 s opens the diagnostics, on the release instead of at 1.5 s.
- A tap opens the rules as always, and the release that ends any hold is swallowed (the `holdFired` latch), so a hold never also opens the rules.
- Without the feature compiled in, or where it is unavailable, no long hold is added and the door is exactly the 9 x 9 one.

### The badge

"243" in the small label type (9.5 pt semibold, tracked, capitals) in the ink, on a paper-coloured pill with a hairline ink border (`UtttBigBadge`, UtttKit/UtttBigMode.swift).
It sits in the extension's root view, above every screen and below the send overlay, 12 points in from the bottom left corner of the safe area.
It is not in the top right corner because the send hint points at Messages' Send button from there whenever a bubble is staged, and the two were drawn on top of each other.

## What the bubble looks like

The picture IS the data: 243 x 243 cells and a header row of three greys (white empty, grey X, black O; the top row is the kit's header and CRC), at THREE PIXELS A CELL (`BubbleDataGeometry.robust243`, 729 x 732 px), scaled by Messages into its frame.
One pixel a cell (243 x 244) does not carry a real game's sparse board through Messages' two JPEG passes (BubbleData.swift's comment on `board243`), so the bubble uses the robust geometry.
It cannot be decorated, because any change to the pixels breaks the reading. In the transcript a new game is a white square, and every move is one grey or black dot on it: the board as it stands, too small to play from.
The caption under it is exactly the 9 x 9 game's ("New game?", "X to play", "X won in N moves"); `summaryText` is the same line.
That is the one honest design: the bubble is a board that only the app can read, and it says so by looking like one.

## The wire

Format byte 3 under the 9 x 9 game's magic (`uttt/c/src/uttt_big_msg.h`): seed, flags, look, the seat tags as format 2 has them, then n_plies, the last move, a CRC-32 of the 59,049 cells, and the 2-byte wire check.
Every shipped reader (formats 1 and 2) answers `UTM_EFORMAT` to it and shows "a bubble this build cannot read"; nothing misreads.
The board comes from the picture through `BubbleData.symbols(from: message, cells: 243)` and is held against the URL's CRC before it is adopted; the ply count, the turn (from the mark counts), the last move's cell and every node status are checked, so a damaged picture is refused, never guessed.

## The flag

ONE compile-time switch: the Swift compilation condition `UTTT_BIG_BOARD`, set in `uttt/ios/project.yml` as

    UTTT_BIG_BOARD_CONDITION: UTTT_BIG_BOARD

Delete the word and every build compiles the feature out: every Swift file of the feature is inside `#if UTTT_BIG_BOARD`, and every kernel entry (`uti_big_*`) is `UTI_UNEXPORTED`, so a build whose Swift never names them dead-strips them from UtttKit.

- Debug builds (the rig, the preview) have the condition.
- A TestFlight archive has it because `uttt/ios/Tools/ship.env` passes `UTTT_TESTFLIGHT_CONDITIONS=$(UTTT_BIG_BOARD_CONDITION)` on the archive command line (`SHIP_ARCHIVE_SETTINGS`, `shared/tools/ship/ship.sh`).
- An App Store archive is `ship.sh --store`, which passes nothing, and carries none of the feature.
- THE PICTURE CODEC GOES WITH IT. UtttKit compiles BubbleDataKit's two sources (`BubbleData.swift`, `bubble_data.c`, with `uttt/ios/CBubbleData/module.modulemap`) itself, and `EXCLUDED_SOURCE_FILE_NAMES` is looked up by the configuration's feature word (`UTTT_BIG_WORD`), so a build without the feature does not compile them.
  Linking the SwiftPM package instead put the whole kit into every Release build: Xcode links a local package's targets as prelinked objects, and the linker could not drop any of it (measured: 12 `bd_` and 187 BubbleDataKit symbols in a Release build without the feature).
- The extension never names the codec: `UtttBigBubble` (UtttKit) is its one caller.

PLUS a runtime check, so a leaked condition still cannot expose the feature: `UtttBig.available` is true only in a debug build or when `Bundle.main.appStoreReceiptURL?.lastPathComponent == "sandboxReceipt"` (a TestFlight install). The hold does nothing otherwise, and a big bubble opened by such a build shows as unreadable.

## Where it lives in the app

- `uttt/ios/UtttKit/UtttBigScreens.swift`: `UtttBigModel` (the UtttModel analogue), `UtttBigGameScreen` (a 64-point header with "you are" and the kernel's headline, the board below, the rules door and Again at the bottom; it opens focused on the region when that is a 27 x 27 block or smaller, and keeps the zoom when the position changes), `UtttBigLobby` (the waiting words, no board). Every number is in `UtttBigLayout`.
- `uttt/ios/UtttKit/UtttBigBubble.swift`: the picture on and off a bubble; every read logs its risky count and smallest margin (`big-read`).
- `uttt/ios/UtttKit/UtttBigMode.swift`: the mode and the badge.
- `uttt/ios/UtttMessages/MessagesViewController.swift`, its "the 243 board" section and the `#if UTTT_BIG_BOARD` lines that reach it: a big link is routed to the big path where `UtttBig.available`; the board of a bubble this device staged or sent comes from memory (the newest four), any other from the message's picture; `current`, `newest`, `markSent` and `sessionFor` ask the big kernel when either link is big (a big and a 9 x 9 are different games, the tapped one wins).
- DEBUG only: `dev.bigzoom` (a number, or `max`) opens the next big board at that zoom centred on the region, so the rig can tap a cell without a pinch.

## The numbers behind it (2026-10-01)

- Kernel (`tests/uttt_big_test.c`, `make run`): 120 checks; 2,000 random depth-2 games held to the shipped 9 x 9 kernel ply for ply (legal list, turn, over, region, cells, blocks, play and undo); 10 random depth-5 games to the end at 39,561 to 40,936 plies, about 0.1 s each; 36 mutations each caught by a named assertion. `sizeof(UtbGame)` 66,448 bytes.
- Codec (`tests/uttt_big_msg_test.c`): 105 checks; every ply of 20 games through bytes, text, header and board; every single-bit flip and every truncation of a link refused; a format-3 link refused by the shipped `utm_decode` with `UTM_EFORMAT`; the 9 x 9 golden `b70266f0d580016a5cf6f0753e83dbf81c12f8053463485962a7778ea8beda1002` pins the shipped format; 23 codec and 9 bridge mutations.
- The picture chain (`make big-chain`, ImageIO q0.50 then q0.89, both 4:2:0): one random game of 40,711 plies, EVERY ply, at 3 px a cell: 40,712 positions, 0 wrong, 0 refused, worst margin 25 of 64, 0 risky reads, JPEGs 255 KB then 460 KB on average, 991 s. At 1 px a cell (the kit's board243) 21,330 of the positions were refused (none misread), so the bubble is robust243 and the kit's own tests now hold a sparse board (`testSparseBoardNeedsThreePixelsACell`: 29 of 72 sparse boards refused at 1 px, all read at 3 px with a worst margin of 37).
- `make run` 30.7 s, `make asan` 82 s, `make ios-smoke` "bridge ok", `cd foolish && npm run -s test:validate` 150 of 150.
- The archives (`uttt/ios/Tools/ship.sh --build 14 --no-upload`, and the same with `--store`): both export and pass `release_strings.sh`. The TestFlight ipa is 2,227,149 bytes and its UtttKit carries 35 `uti_big` and 12 `bd_` symbols, 63 `UtttBig` and 15 `BubbleData` strings; the App Store ipa is 2,109,638 bytes and its UtttKit carries 0 of each (and 102 `uti_` symbols against 137).
- The 9 x 9 game, pixel for pixel: the same seeded game (`devgame 6`) on the branch base build and on this build, same simulator, frames 8 s after opening: below Messages' own compose chrome 217 of the sheet's pixels differ by at most 1 of 255 in one channel (two frames of the base build alone differ in 83 there); the board, the words and the doors are identical. The 9 x 9 kernel files, UtttBubble.swift, UtttWire.swift, UtttBoardView.swift, UtttModel.swift and UtttKernel.swift are untouched (`git diff 1bccc75e`).
- The board at three zooms (preview app and the extension): fully out, levels 1 to 3 solid and level 4 faint, won blocks tinted with a big mark at the level they were won, the target outlined in the highlighter; mid (27 x 27 cells across), levels 2 to 4 solid and the cells faint; fully in, 36 pt cells with all five levels.
- Through idb, a hold of 5 s toggles the mode; 4.3 s through idb did not (the recogniser's 4 s is measured from the simulated touch, which idb delivers late). A finger needs 4 s.

## How it was verified

On the rig (`foolish/ios/Tools/rig/rig.sh` with `uttt/ios/Tools/rig.env`, an iPhone 17 Pro Max class simulator on iOS 27, 440 x 956 pt), 2026-10-01:

1. A seeded 9 x 9 game (`devgame 6`), a 4.2 s hold on the rules door: `big-mode on` in the log and the badge on the 9 x 9 screen. A 2.5 s hold opened the diagnostics and left the mode on.
2. The + menu with the mode on: `start big`, the big lobby, and the staged bubble a white picture captioned "New game?". Sent.
3. With `dev.picker` on, ONE thread for both players: the bubble tapped, "who are you" over it (the badge under it), "took it up": the reader took the picture back as `59049 cells, risky 0, min margin 46`, seat open, the board at the deepest zoom (cells 36 pt), "Your move", you are X. A tapped cell staged at once: the bubble a dot on white captioned "O to play", the board "Waiting on O" with the draft outlined, the earlier bubble collapsed to its caption (one session). Sent.
4. Tapped again, "put it down": seat O by record, plies 1, X's move outlined as the last move and the 3 x 3 region washed. O's reply staged ("X to play", two dots), sent; tapped again as X: seat X by record, plies 2, O's move there. Every read: risky 0, min margin 46.
5. Pan (a swipe), a double tap at the deepest zoom (out to the whole board), a double tap at the whole board (in by three): all as expected.
6. A 4.2 s hold on the big game's rules door: `big-mode off`; the + menu then staged the 9 x 9 invitation with no badge.
7. Messages' X on a staged big invitation: `cancel the draft`, `dismiss the big invitation draft was cancelled`.

Memory (`mem`, physical footprint / peak, MB): staging the invitation 21.7 / 22.2 before the picture and 23.8 / 25.8 after it, 25.9 / 26.6 a second after the insert; staging a move from the expanded drawer 35.6 / 35.9 before and 37.7 / 39.7 after, 48.6 peak at the insert, 31.7 a second later; reading a picture 23.4 -> 24.5, peak 29.2.
The first read of a sent picture took 2.2 s on the main thread (84.2 s -> 86.4 s in the log); every later read took 40 to 110 ms. Not profiled yet.

Mutation checks on the rig, each watched go red and then restored:
- `UtttBigMode.on`'s setter made a no-op: a hold from off logged `big-mode off` again and no badge came.
- The router's `isBigText` made false: the + menu's big invitation showed "Can't read that".

Builds: Debug UtttMessagesApp and UtttPreview with no warnings. Release for the simulator, `xcrun nm` / `strings` of UtttKit: without the feature `uti_big` 0, `bd_` 0, BubbleData 0, UtttBig 0; with `UTTT_TESTFLIGHT_CONDITIONS='$(UTTT_BIG_BOARD_CONDITION)'` `uti_big` 35, `bd_` 12, BubbleData 171, UtttBig 146. `shared/tools/release_strings.sh` is clean on both.

NOT proven: a real pinch (idb has none; the preview app's UI test pinches the board view), a second device, a real send between two phones (the simulator's thread is one participant), Again on a finished big game, and the re-staging of an unsent invitation when the mode is toggled (no screen of an unsent invitation has a rules door).
