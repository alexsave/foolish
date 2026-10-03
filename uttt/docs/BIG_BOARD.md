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

1. Open any Ultimate screen that shows a grid: the "New game?" screen of your own invitation (the 9 x 9 "Waiting" lobby from the + menu, or a sent one nobody has answered), a game in progress, a finished game, somebody else's invitation, a game you are watching, or any 243 screen.
2. HOLD THE GRID STILL FOR 4 SECONDS, anywhere on the board, on a playable cell or not. A haptic fires (success) and a "243" badge appears in the bottom left corner of the drawer: the mode is on.
3. If that screen was your own UNSENT invitation (it is in Messages' compose field), the invitation in the field is swapped at once for a 243 x 243 one (a white picture captioned "New game?"), and the screen becomes the big lobby: the same words over the empty big board at the fit. On any other screen nothing else changes; open Ultimate from the + menu and the invitation it stages is a 243 x 243 game. Every bubble of that chain is a 243 x 243 game, and a rematch (Again) of a big game is a big game.
4. On the board: pinch to zoom, pan when zoomed (past the board's edges, so a corner cell can be brought to the middle of the view), double-tap to zoom in by three (and back out to the whole board from the deepest zoom), tap a cell to stage a move, tap another cell to change your mind. The yellow wash is where you must play, outlined in gold; the last move is ringed in its mark's colour; your staged move is ringed dashed. Every outline is the size of what it outlines and thins as you zoom out. Messages' X on the staged bubble takes the move back, as in the 9 x 9 game.
5. Hold any grid still for 4 seconds again to leave the mode (a warning haptic, and the badge goes from a 9 x 9 screen). On your own unsent invitation the field is swapped back to a 9 x 9 invitation and the 9 x 9 lobby comes back. Otherwise the mode changes only what CREATING a game does; it does not touch games already in the thread. A big game's screen keeps the badge whatever the mode, because it says what board this is.

The rulebook is just the rules again: a tap opens them, and in a debug build a 1.5 s hold opens the diagnostics, exactly as on the branch base (`UtttRulebookButton` is byte for byte the base's).
WHILE THE 243 MODE IS ON, a 1.5 s hold on the rulebook opens the picture diagnostics instead (next section).

The mode persists in the extension's own defaults (`UserDefaults.standard`, key `uttt.big.mode`, as UtttSeats keeps its records) until it is toggled off, and survives an update of the app.
It is never on where the big game is unavailable (`UtttBig.available`).

### The hold, exactly

ONE recogniser class, `UtttModeHold` (UtttKit/UtttBigMode.swift), a `UILongPressGestureRecognizer` whose numbers are the kernel's: `minimumPressDuration` is `UTB_HOLD_MS` (4000 ms) and `allowableMovement` is `UTB_HOLD_SLOP_PT` (10 points), both in `uttt/c/src/uttt_big_msg.h`, read through `uti_big_hold_ms` / `uti_big_hold_slop` as `UtttBig.holdSeconds` / `UtttBig.holdSlop`.

- It fires once, the moment 4 s is reached.
- It takes nothing from the board's own gestures: `cancelsTouchesInView`, `delaysTouchesBegan` and `delaysTouchesEnded` are all false, and nothing `require(toFail:)`s it. A tap stages a move exactly as before, with no added delay.
- A tap and a double tap lift long before 4 s; a pan or a drag moves past the 10 points; a pinch is two fingers. Each makes the recogniser fail on its own, so none of them ever toggles. A hold that drifts past the slop or lets go early does nothing.
- When it does fire, a tap still waiting on the same touch fails, so the hold never also plays the cell under it.
- On the 9 x 9 board (`UtttBoardView.setModeHold`) it sits beside the board's one tap recogniser. The waiting lobby's board takes no touches; `setModeHold` turns them on, which is safe because that board's `onTap` is nil.
- On the 243 board (`UtttBigBoardView.setModeHold`) it sits on the scroll view's zooming content beside the taps, so it sees the touch before the scroll view's pan takes it, and it recognises alongside the pan and the pinch, which it never holds up.
- Each screen with a grid forwards `setGridHold` to its board (UtttLobbyScreen only in its `.waiting` stance; UtttGameScreen; UtttWatchScreen; UtttBigGameScreen; UtttBigLobby). The extension calls it in one place, `show(_:)`, and only where `UtttBig.available`.
- Without the feature compiled in, no board has any gesture beyond the 9 x 9's tap: `setModeHold`, `setGridHold` and `UtttModeHold` are all under `UTTT_BIG_BOARD`.

### The invitation in the field

The toggle swaps the draft only when the screen up is the waiting lobby (9 x 9 or big) AND an invitation of no plies is in the field (`staged` set and `draftURL` set): turning the mode on calls `start`, which reads the mode, opens a big invitation (`UtttBig.openInvitation`) and stages it with its picture (`stageBig`); turning it off opens and stages a 9 x 9 one (`Uttt.openInvitation`, `stage`). The new bubble replaces the old one in the field, and the screen re-presents to match.
A SENT invitation has nothing in the field (`draftURL` is cleared on the send), so the hold only toggles the mode.

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

## The send rule is an open question

Rule (A), the full shift of the address every move, is what ships.
The owner expected the send to climb only when a move completes a block.
`BIG_BOARD_SEND_RULE.md` holds both rules, the precedent found, the arguments each way, and what to measure before deciding.

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

- `uttt/ios/UtttKit/UtttBigScreens.swift`: `UtttBigModel` (the UtttModel analogue), `UtttBigGameScreen` (a 64-point header with "you are" and the kernel's headline, the board below, the rules door and Again at the bottom; it opens focused on the region when that is a 27 x 27 block or smaller, and keeps the zoom when the position changes), `UtttBigLobby` (the waiting words over the empty big board at the fit, the grid the mode's hold is on). Every number is in `UtttBigLayout`.
- `uttt/ios/UtttKit/UtttBigBubble.swift`: the picture on and off a bubble; every read logs its risky count and smallest margin (`big-read`).
- `uttt/ios/UtttKit/UtttBigMode.swift`: the mode, its door (`UtttModeHold`) and the badge.
- `uttt/ios/UtttKit/UtttBigDiagnostics.swift`: the picture diagnostics' facts, its sheet, its door on the rulebook (`UtttBigDiagHold`) and the history's bytes; every rule of the report is `uttt/c/src/uttt_big_diag.c`.
- `uttt/ios/UtttMessages/MessagesViewController.swift`, its "the 243 board" section and the `#if UTTT_BIG_BOARD` lines that reach it: a big link is routed to the big path where `UtttBig.available`; the board of a bubble this device staged or sent comes from memory (the newest four), any other from the message's picture; `current`, `newest`, `markSent` and `sessionFor` ask the big kernel when either link is big (a big and a 9 x 9 are different games, the tapped one wins).
- DEBUG only: `dev.bigzoom` (a number, or `max`) opens the next big board at that zoom centred on the region, so the rig can tap a cell without a pinch.

## The picture diagnostics (TestFlight build 1.1(16))

What a real send did to a big bubble's picture, told by the app itself, because the second phone has no cable.

### How to use it

1. Turn the 243 mode on (a still 4 s hold on any grid; the "243" badge shows).
2. Tap the bubble you want to know about in the thread (the received one, on the phone that received it), so the drawer opens on it.
3. HOLD THE RULEBOOK (the book door, bottom right) for 1.5 s. A sheet titled "243 picture" opens over the drawer with the report.
4. Tap Copy: the whole report goes on the pasteboard as plain text. Paste it into a message to us. Close (or a swipe down) puts the drawer back.

The report is about the message the drawer was opened from: the selection when the drawer opened, a bubble tapped while it was up, or one that arrived while it was up.
Opened from the + menu there is no message, and the report is the header and the history only.
Outside the mode the hold does nothing new: the rulebook is the shipped door (a tap opens the rules; in a debug build its own 1.5 s hold opens the debug diagnostics, unchanged).
The grid's 4 s hold is on the board and this one on the rulebook, so neither sees the other's touches.

### What each line means

Every line is at most 46 characters.
A line that compares a measurement with what was expected starts with a space when they agree, `!` when they do not, and `~` when the value is shown but not judged.

- **Header**: the app version and build, `debug`, `TestFlight` or `release`, the iOS version, the device model (`iPhone16,2`; a simulator says `sim`), and the time.
- **The message**: how the drawer came to it (`selectedMessage`, `didSelect`, `didReceive`); `sent by this device` or `the other person` (`senderParticipantIdentifier` against `localParticipantIdentifier`); `pending` (`isPending`); `session`, the CRC-32 of `MSSession.hash` printed in hex, so two bubbles of one session show the same number.
- **The link**: its length in characters and in bytes, the format byte (3 is the 243 board), `header and check` (the `UTM_E*` of reading the header and its 2-byte wire check), the seed, the flags and whether X is taken, the ply count and the last move, `seat here`, and the board CRC-32 the link carries.
- **`seat here`** is this device's seat, which only the big kernel can say, and it holds ONE big game: the last one the drawer read, staged or played, which need not be the bubble the report is on. So (`seat_lines`, `uttt_big_diag.c`): when the kernel holds this link's position (the same text, or the same game at the same ply and board CRC), `seat here X, by record`; when it holds the same game at another ply, the same line and `~ as at ply N, which the app holds` (most often my own reply staged on top of the bubble I opened, or a newer bubble of that game that the drawer showed instead); otherwise `seat here ? - the app holds another game` or `... no 243 game`.
  Build 1.1(16) said `seat here not the game on screen` for all of the last three. It was printed whenever the message's link was not byte for byte the kernel's re-encoding of its resident big game, and it read as if the drawer showed a different game, which in the usual case (a received bubble, then a reply staged, or the drawer preferring my own staged draft) it did not.
- **The layout**: its class, the lengths (never the text) of caption, subcaption and summary (`nil` for none), the picture's pixels (`image W x H`, expected 729 x 732) and scale, and the file at `mediaFileURL`: its extension and its bytes. The bytes are not judged, because they depend on the board: a mid-game board is about 255 KB at q0.50 and 460 KB at q0.89, a new game's about 10 KB.
- **The jpeg, from the file's bytes** (parsed in C, `uttt/c/src/uttt_big_diag.c`): its size (expected 729 x 732; a side at 1200 is the transport's cut), components (3), chroma (`4:2:0`, from the luma component's 2x2 sampling), the frame (`baseline SOF0`), the markers, and the quality.
- **The quality's method**: the luma quantisation table is held to ImageIO's own luma tables at every quality from 0.00 to 1.00 in steps of 0.01 (`src/uttt_jpeg_imageio.inc`, generated with `make jpeg-imageio`, held to ImageIO on every `make big-diag-imageio`). An equal table is `exact`, and its range is every step that writes that table: ImageIO writes one table for 0.89, 0.90 and 0.91, so the transport's file reads `0.89-0.91`. A table no step writes (ImageIO takes the quality as a continuous number) is interpolated on the tables' sums and marked `~`. The libjpeg scaling's quality is printed as well, for an encoder that is not ImageIO. A copy from the other person is expected at 0.89 (after the transport); this device's own at 0.50 or 0.89.
- **The reading, at the size it came**: the kit's result (`BD_EOK` or the `BD_E*` it refused with, or `no picture`), cells sampled, risky cells (within 16 of a threshold) and the smallest margin, whether the CRC-32 of the decoded board equals the link's (`= link`: the board read is the board that was sent), the mark counts, and the read's time in ms.
- **The greys**: the luminance, (r + g + b) / 3 at the pixel the kit samples. The picture is 243 x 244 cells: row 0 is the kit's header (64 cells of greys and blacks that are its magic, length and CRC, then 179 painted empty) and rows 1 to 243 are the board.
  THE BOARD'S CELLS ONLY (`the board, rows 1-243: 59049 cells`): for the cells read as empty (painted 255), X (128) and O (0), each with its count, mean, standard deviation, minimum and maximum (rounded outward), so `X (painted 128): 1 cell` means one X on the board; `board worst deviation`; and a 16-bucket histogram of the board's luminances, 16 levels a bucket, the darkest first, as two lines of eight counts.
  ROW 0 APART: `header 64 cells, worst deviation D` with how they read (`read empty E, X X, O O`), and the same for `rest of row 0 179 cells`.
  Last, `worst deviation from nominal` over every cell, row 0 and the board: the largest distance of any cell from its class's painted level, which is the room a 4-level picture would have to fit in.
  Build 1.1(16) counted row 0 into the classes and the histogram, so a real phone's report said `X (painted 128): 21 cells, O (painted 0): 19 cells` for a board holding one X: 20 and 19 of those were header cells. The sampling is the kit's `bd_sample` reproduced, and `tests/uttt_big_diag_test.c` holds the two to the same classes, risky count and margin on six picture sizes and four noise levels.
- **History, newest first**: the last 20 reads and sends this extension knows about, two lines each: the day of the month and time, `sent` (what this device's own extension saw at `didStartSending`: the picture's size, the file's bytes and quality; not read) or `open` (a read of a bubble's picture, by the app or by this screen), the pixels, the bytes, the quality (`=` exact, `~` estimated), then the risky count, the smallest margin and the result (`crc!` when the board did not match its link). Kept in the extension's `UserDefaults.standard` under `uttt.big.diag.ring` in the kernel's fixed 404-byte little-endian layout (`uttt_big_diag.h`), so it outlives the extension and a send on one phone can be set beside the opening on the other.

### What the simulator showed (2026-10-02, iOS 27.0, iPhone 17 Pro Max class, single-thread picker)

The mode on by a 5 s grid hold on the 9 x 9 lobby, the big invitation sent, the bubble tapped and taken as X, one move played and sent, that bubble tapped and taken as O, a 2 s hold on the rulebook: the report said format 3, `UTM_EOK`, plies 1, the image and the file 729 x 732, 4:2:0, baseline, `! quality 0.50 (expect 0.89)` (the simulator's loopback has no transcoder, so its received copy is the sender's own 0.50 file), 10,163 bytes, `BD_EOK`, risky 0, min margin 46, `board crc 0fb05b6c = link`, X 1, read 5.1 ms, greys empty mean 255.0 (min 239), X mean 127.4 (115 to 140), O mean 3.7 (0 to 18), worst deviation 18.0, and eight history records across two extension processes and a reinstall.
Copy put the same text on the simulator's pasteboard (`xcrun simctl pbpaste`).
With the mode off, the same hold opened the debug diagnostics (the base's) and a tap opened the rules.

On a Mac, ImageIO's two passes of a real send (`make big-diag-imageio`): a mid-game board (30% marks) at 280,680 then 507,158 bytes reads 0.50 then 0.89-0.91 exact, risky 0, min margin 30, worst deviation 34 (empty 221 to 255, X 101 to 161, O 0 to 33); a sparse one at 16,381 then 22,771 bytes, min margin 46, worst deviation 18.

NOT proven: anything on a real phone. The quality the transport writes on a real send, the 1200 px cut and the history across two phones are what this screen is for, and none of them has been seen through it yet. The JPEG parser was held to Messages' own simulator file and to ImageIO and cjpeg files; the real transport's received files on this Mac (`~/Library/Messages/Attachments`) could not be read by the agent that built this (access refused), so no transcoder output is among the test files.

## Build 1.1(17): the owner's notes on 1.1(16) (2026-10-03)

### 1. The board pans past its edges

"The very bottom left corner is hard to pan to": the board could not be scrolled beyond its own edges, so a corner cell sat against the drawer's edge.
The scroll view's insets are now worked out on each axis from how far the board overflows the view there (`UtttBigBoardView.insets(zoom:)`).
While the board is smaller than the view (the fit, and only the fit) they centre it as before, and it cannot move, so the fit is one position and a pinch out or a double tap at the deepest zoom lands exactly on it.
Once it overflows, each edge may come in as far as the middle of the view's safe area less half a cell, so every cell, the four corners too, can be brought to the middle of the view; the room grows with the overflow (never more than the overflow itself), so the insets change continuously and nothing jumps as a pinch leaves or returns to the fit.
A non-animated zoom (the screen's opening focus, `dev.bigzoom`) is centred with the new zoom's room afterwards, so a target at the board's edge opens in the middle of the view rather than against its edge.
An animated double tap from the fit still lands against the edge on its first step (the fit has no room past the edges), and the next double tap centres.
A clamp of the offset at the end of a zoom was tried and taken out: the corner test pinching out from past an edge was green without it, so UIScrollView already brings the offset back.
The painting is unchanged: the canvas is the board's visible part clipped to the view, so past an edge it is smaller, and the sheet's paper shows around the board.

### 2. The outlines scale with the zoom

"The outlines (the red O, the blue X, the yellow highlight) should scale down accordingly as you zoom out."
Every outline is now the size of what it outlines, with a stroke in proportion to it, clamped to 0.75..3 screen points (`stroke`): the last move's and the draft's ring 0.08 of the cell (never smaller than 4 points), the forced target's outline 0.03 of the block (never smaller than 12 points), the anywhere outline 0.0075 of the board (always at the 3-point ceiling, since the board is never under 400 points); the draft's dashes are 2 and 1.5 strokes long.
The old fixed sizes were a 10-point ring 2 points wide, a 12-point target outline 3 points wide and a 4-point anywhere outline.
At three zooms on the rig and in the preview: at the deepest zoom (36-point cells) the rings and the target outline are about 3 points; at a 27 x 27 block across (16-point cells) about 1.3 to 1.4 points; at the fit a 4-point ring and a 12-point gold box, both hairlines.
The target at the fit was checked twice. With a 6-point floor it was a speck: findable only knowing where to look, in a corner (rig) and in the middle of a game's marks (preview). The 12-point floor, a one-line change, makes it a hairline box round the 5-point wash, which is findable; the ring stays at 4 points.
The draft never read as a draft in the app: the game screen hands a staged move as both the last move and the draft, and the solid ring painted under the dashed one filled its gaps (the preview's synthetic board has two different cells, which is why it hid there). The solid ring is now skipped when it is the draft.

### 3. The diagnostics count the board apart from the header row

See "The greys" and "`seat here`" above.
On the simulator, after this fix, the same situation as the phone's report (a bubble of one X, opened as O with O's reply staged) read `X (painted 128): 1 cell`, the header `read empty 25, X 20, O 19`, and `seat here O, by record` with `~ as at ply 2, which the app holds`.

### The numbers (2026-10-03)

- `make -C uttt/c run`: exit 0 in 25 s, every suite 0 failed (`uttt_big_diag_test` 7920 assertions); `make asan` exit 0 in 75 s, no sanitizer error; `make ios-smoke` "bridge ok"; `cd foolish && npm run -s test:validate` 150 of 150.
- The diagnostics' mutations, each red at its own assertions and restored from a copy: row 0 counted into the board (20 failures, the first "board classes 56945 1165 1182 against 56740 1141 1168"), one board cell dropped (12, "histogram holds 59048 of the board's 59049"), the same-game branch removed, the same-position rule forced either way, the re-spelled-link rule removed.
- The preview's UI tests, iOS 27 simulator, all five green, the new `testEveryCornerCellPansToTheMiddleAndTaps` among them: at zoom 19.88 every corner cell's centre stopped at (220.0, 397.3), the middle of the 440 x 794.7 view; its taps named 0, 14762, 44286 and 59048; a double tap (top corners) and a pinch out from past the edge (bottom corners) gave back the first fit exactly. Mutated to no room past the edges it went red at "top left: the corner cell stopped at (18.0, 18.0)".
- Memory: the preview's peak footprint 65.2 MB over the four corners and 65.5 MB over the deepest-zoom pans (an app process, with SwiftUI); the extension on the rig, staging the corner move from the deepest zoom, 32.9 MB before the picture and 42.4 MB at the insert, 36.0 MB a second later, peak 55.5 MB; reading a picture 28.5 MB, peak 33.7.
- The rig: zoomed in on the bottom-left corner with double taps, the corner cell in the middle of the board view at 36-point cells, a tap: `big played 44286`, the bubble one dot at its bottom left captioned "O to play".
- The 9 x 9, pixel for pixel against the branch base's build (`devgame 6`, frames 8 s after opening, the 243 mode off): below the compose chrome 147 and 65 pixels differ by at most 1 of 255, and two base frames differ by 148 there. No 9 x 9 file changed (`git diff 8c62e49e --stat`).
- The archives (`ship.sh --no-upload`, 1.1(17)): both "release strings clean". TestFlight ipa 2,276,778 bytes, its UtttKit 39 `uti_big`, 32 `utb_`, 8 `ubd_`, 12 `bd_` and 125 `UtttBig` symbols. App Store ipa (`--store`) 2,102,075 bytes, its UtttKit 0 of each of those as symbols and as strings (102 `uti_` against 141), the extension 0 of each.
- The big kernel's headers (`uttt_big.h`, `uttt_big_msg.h`) declare their functions under `#pragma GCC visibility push(hidden)`, so a framework that links the kernel no longer exports them and a build without the feature dead-strips them; the previous report counted 43 `utb_` in the App Store build, and this one 0.

NOT proven: anything on a real phone. The corner, the outlines and the diagnostics were seen on an iOS 27 simulator only, with idb taps and XCUITest gestures, not a finger; the 243 badge and the rulebook overlap the zoomed board in the compact drawer (the board view spans the drawer's width there), which this round did not change.

## The arena: uttt.live/243

Two bots play the 243 x 243 game in the visitor's browser, spectator only (`web/app/243`, `web/components/Arena.tsx`, the kernel `c/wasm/uttt_243_web.c` built by `make wasm-243`, the bot `c/src/uttt_big_bot.{c,h}`).
A game is its seed (`?seed=`) and the settings each move was played at.

### The bot

Negamax with alpha-beta and iterative deepening over a score kept incrementally: a play re-reads only the grids on its leaf's path.
Wins are weighted in ninths, nine times per size: 3 x 3 = 9, 9 x 9 = 81, 27 x 27 = 729, 81 x 81 = 6,561, the game 2^22.
A THREAT, two of a side's marks in an open line of a live grid with the third child still open, is worth two ninths of that grid's win (2, 18, 162, 1,458, and 13,122 in the game's grid): less than the win it threatens, more than a win one size down, and taking a win (seven ninths, after the threat it spends) beats making three threats.
Deciding a grid takes every threat inside it off the board, both sides'.
A move on the last ply that decides a grid is answered by the opponent's best grid-deciding reply, or nothing if it declines (a one-ply quiescence); the first ply, which always finishes, is never extended.
The candidate cap below the root follows the plies and the budget (`utb_bot_cap_for`: the widest whose alpha-beta tree fits, 3 to 32), and the root's is four times that, at least 48.

### The control

In the header: "Look ahead" (a stepper, 1 to 8 plies) and "Work a move" (Small 4,000, Medium 30,000, Large 150,000, Huge 1,000,000 work units), with the caps they give shown beside them.
A change goes to the kernel (`ua_set_bots`) and the next move plays by it; the game goes on.
The defaults are 6 plies on Large.

### The numbers (2026-10-03, this Mac)

- Chrome 154 (CDP, 1100 x 1300 at 2x, `next start`), seed 4803c49362b7f7bf: at the defaults a whole game of 27,047 plies, X won, in 51.6 s, 524 moves a second over the game (about 1,000 early, 580 late). At 1 ply, 18,779 plies drawn in 0.237 s, 79,000 moves a second.
- Natively (`-O3 -flto`), whole depth-5 games: the old setting (4 plies, 4,000) went 30,000 -> 18,500 moves/s with threats and -> 14,700 with the extension too; at the defaults about 790 moves/s, 30 to 35 s a game, 93% of moves searching all 6 plies. 7 plies on Large about 390 moves/s (85% full), 8 on Large 190 (88%); a small budget starves deep plies (8 on Small: 12% of moves finish).
- Strength: threats alone beat the material-only bot 91-3 at depth 3; the new defaults beat the old page's bot 40-0 at depth 4. 6 plies beat 4 at depth 2 (74-18 of 120, z 5.8, in the suite) and at depth 3 (447-239 of 800, 114 drawn, z 7.9). The extension is worth about a ply at a fixed depth (3 plies with it against 4 without: 121-130 at depth 3) and nothing measurable on the tight 4,000 budget.
- The module is built for the MVP feature set plus bulk memory and sign extension (`-mcpu=mvp -mbulk-memory -msign-ext`): `llvm-objdump -d` finds no v128 or reference-type opcodes, one memory.copy, two memory.fill, six i32.extend8_s, and every function type has at most one result. It is no slower: in node at 4 plies / 4,000, 10,344 moves/s with the default features and 12,003 with these.

### Fixed with it

- "MOVES / S 32,502,000" and "TIME 0:00.0" after a game: a loop started again over a finished game (React runs an effect again when a hidden page is shown) divided the whole game's moves by its own first millisecond. The clock (`GameClock`, `lib/arena.ts`) now belongs to the game, ignores frames once it is over, counts a gap over 250 ms as 250 ms, states no rate under 20 ms of play, and the time reads to the millisecond once the game is over ("51.649 s").
- "This browser could not start the game (it needs WebAssembly)" for every failure: it now says the error's own words under the sentence (`startError`), and keeps the sentence alone only when there is no WebAssembly. One real cause found while measuring: a browser holding an older `uttt243.wasm` in its HTTP cache (served with max-age 3600) beside a newer build's readers is refused by the layout check; the module is now fetched with `cache: 'no-cache'` (a 304 when unchanged).

NOT proven: the page on Safari, iOS or an in-app browser; the timings on any machine but this Mac; whether the extension pays for itself on the page's budgets.

## The numbers behind it (2026-10-01)

- Kernel (`tests/uttt_big_test.c`, `make run`): 120 checks; 2,000 random depth-2 games held to the shipped 9 x 9 kernel ply for ply (legal list, turn, over, region, cells, blocks, play and undo); 10 random depth-5 games to the end at 39,561 to 40,936 plies, about 0.1 s each; 36 mutations each caught by a named assertion. `sizeof(UtbGame)` 66,448 bytes.
- Codec (`tests/uttt_big_msg_test.c`): 105 checks; every ply of 20 games through bytes, text, header and board; every single-bit flip and every truncation of a link refused; a format-3 link refused by the shipped `utm_decode` with `UTM_EFORMAT`; the 9 x 9 golden `b70266f0d580016a5cf6f0753e83dbf81c12f8053463485962a7778ea8beda1002` pins the shipped format; 23 codec and 9 bridge mutations.
- The picture chain (`make big-chain`, ImageIO q0.50 then q0.89, both 4:2:0): one random game of 40,711 plies, EVERY ply, at 3 px a cell: 40,712 positions, 0 wrong, 0 refused, worst margin 25 of 64, 0 risky reads, JPEGs 255 KB then 460 KB on average, 991 s. At 1 px a cell (the kit's board243) 21,330 of the positions were refused (none misread), so the bubble is robust243 and the kit's own tests now hold a sparse board (`testSparseBoardNeedsThreePixelsACell`: 29 of 72 sparse boards refused at 1 px, all read at 3 px with a worst margin of 37).
- `make run` 30.7 s, `make asan` 82 s, `make ios-smoke` "bridge ok", `cd foolish && npm run -s test:validate` 150 of 150.
- The archives (`uttt/ios/Tools/ship.sh --build 14 --no-upload`, and the same with `--store`): both export and pass `release_strings.sh`. The TestFlight ipa is 2,227,149 bytes and its UtttKit carries 35 `uti_big` and 12 `bd_` symbols, 63 `UtttBig` and 15 `BubbleData` strings; the App Store ipa is 2,109,638 bytes and its UtttKit carries 0 of each (and 102 `uti_` symbols against 137).
- The 9 x 9 game, pixel for pixel: the same seeded game (`devgame 6`) on the branch base build and on this build, same simulator, frames 8 s after opening: below Messages' own compose chrome 217 of the sheet's pixels differ by at most 1 of 255 in one channel (two frames of the base build alone differ in 83 there); the board, the words and the doors are identical. The 9 x 9 kernel files, UtttBubble.swift, UtttWire.swift, UtttModel.swift and UtttKernel.swift are untouched, and UtttInk.swift (the rulebook) is byte for byte the base's; UtttBoardView.swift, UtttGameScreen.swift and UtttLobbyScreen.swift differ from the base by one `#if UTTT_BIG_BOARD` block each, the grid hold (`git diff 1bccc75e`).
- The board at three zooms (preview app and the extension): fully out, levels 1 to 3 solid and level 4 faint, won blocks tinted with a big mark at the level they were won, the target outlined in the highlighter; mid (27 x 27 cells across), levels 2 to 4 solid and the cells faint; fully in, 36 pt cells with all five levels.
- Through idb, a hold of 5 s toggles the mode; 4.3 s through idb did not (the recogniser's 4 s is measured from the simulated touch, which idb delivers late). A finger needs 4 s; XCUITest's `press(forDuration: 4.6)` fires it.

## How it was verified

On the rig (`foolish/ios/Tools/rig/rig.sh` with `uttt/ios/Tools/rig.env`, an iPhone 17 Pro Max class simulator on iOS 27, 440 x 956 pt), 2026-10-01:

1. A seeded 9 x 9 game (`devgame 6`), a 4.2 s hold on the rules door: `big-mode on` in the log and the badge on the 9 x 9 screen. A 2.5 s hold opened the diagnostics and left the mode on.
2. The + menu with the mode on: `start big`, the big lobby, and the staged bubble a white picture captioned "New game?". Sent.
3. With `dev.picker` on, ONE thread for both players: the bubble tapped, "who are you" over it (the badge under it), "took it up": the reader took the picture back as `59049 cells, risky 0, min margin 46`, seat open, the board at the deepest zoom (cells 36 pt), "Your move", you are X. A tapped cell staged at once: the bubble a dot on white captioned "O to play", the board "Waiting on O" with the draft outlined, the earlier bubble collapsed to its caption (one session). Sent.
4. Tapped again, "put it down": seat O by record, plies 1, X's move outlined as the last move and the 3 x 3 region washed. O's reply staged ("X to play", two dots), sent; tapped again as X: seat X by record, plies 2, O's move there. Every read: risky 0, min margin 46.
5. Pan (a swipe), a double tap at the deepest zoom (out to the whole board), a double tap at the whole board (in by three): all as expected.
6. A 4.2 s hold on the big game's rules door: `big-mode off`; the + menu then staged the 9 x 9 invitation with no badge.
7. Messages' X on a staged big invitation: `cancel the draft`, `dismiss the big invitation draft was cancelled`.

Steps 1 and 6 were run with the first door, a 4 s hold on the rulebook, which the grid hold replaced the same day.

THE GRID HOLD, on the same rig, a fresh iOS 27 simulator, every hold `idb ui tap --duration 5.0` on the grid unless it says otherwise (screenshots in ~/Downloads/foolish-shots, g01 to g19):

1. The + menu with the mode off: the 9 x 9 "Waiting" lobby, its empty board, the 9 x 9 invitation ("New game?") in the field.
2. Hold on that board: `big-mode on`, `my unsent invitation, staged again as 243`, `start big`, `show UtttBigLobby`. The field held the 243 invitation (a white picture, "New game?"), the drawer the big lobby ("Waiting / Nobody has taken it yet" over the empty big board at the fit) and the badge.
3. Hold on the big board: `big-mode off`, `staged again as 9 x 9`, the 9 x 9 invitation back in the field and the 9 x 9 lobby back, no badge. Held again three times, 15 to 6 s apart: on, off, on, the field and the lobby following every time.
4. Sent, `dev.picker` on, the bubble tapped, "took it up": `big-read 59049 cells, risky 0, min margin 46`, the big game screen, "Your move", you are X.
5. A 3 s hold on that grid: nothing - no log line, the same screen, no move.
6. A quick tap on a cell: `big played 29527`, staged at once: the bubble a dot on white captioned "O to play", the board "Waiting on O" with the draft outlined.
7. A hold on that game's grid, on the drafted cell: `big-mode off` and nothing else; the big screen's own badge stayed, the staged move and the field untouched.
8. A seeded 9 x 9 game (`seat a`, `devgame 6`, `open`): a hold on its grid, `big-mode on` and the badge on the 9 x 9 game. Reopened as X (`seat b`), a quick tap on a legal square of the washed block staged the 9 x 9 move at once ("O to play", the X in the bubble and on the board).

The big lobby first showed its board view's flat paper as a band across the sheet's textured paper (g02): the board view and its scroll view are now clear around the board, and the lobby's board is just its square (g18, g19: one paper, edge to edge).

The preview's UI tests (UtttPreviewUITests, iOS 27 simulator), all four green: `testGridHoldTogglesOnlyAFourSecondStill` on the 243 board - a still 4.6 s press fires the door once and taps nothing, a 3.0 s press does not fire, a press that drags 40 points and stays down to 4.6 s does not fire, a tap names its cell, a double tap zooms, a pinch zooms, and none of the last five fires; `testGridHoldOnTheNineByNine` on the 9 x 9 game screen - a 4.6 s press on a legal square fires and plays nothing, a tap on it plays it. Mutations, each red at its own assertion and restored from a copy: `minimumPressDuration` 1 s (red at "a 3 s hold fired the door"), `allowableMovement` 10,000 (red at "a hold that drifted 40 points fired the door"), and `UtttBoardView.setModeHold` installing nothing (red at "did not fire: hold fired 0").

The kernel's numbers: `make ios-smoke` asserts 4000 ms and 10 points; `UTB_HOLD_MS` 1000 and `UTB_HOLD_SLOP_PT` 40 each turned it red (after `build/ios_smoke` was made to depend on the headers: before that the first mutation ran the stale binary, green).

Memory (`mem`, physical footprint / peak, MB): staging the invitation 21.7 / 22.2 before the picture and 23.8 / 25.8 after it, 25.9 / 26.6 a second after the insert; staging a move from the expanded drawer 35.6 / 35.9 before and 37.7 / 39.7 after, 48.6 peak at the insert, 31.7 a second later; reading a picture 23.4 -> 24.5, peak 29.2.
The first read of a sent picture took 2.2 s on the main thread (84.2 s -> 86.4 s in the log); every later read took 40 to 110 ms. Not profiled yet.

Mutation checks on the rig, each watched go red and then restored:
- `UtttBigMode.on`'s setter made a no-op: a hold from off logged `big-mode off` again and no badge came.
- The router's `isBigText` made false: the + menu's big invitation showed "Can't read that".

Builds: Debug UtttMessagesApp and UtttPreview with no warnings. Release for the simulator, `xcrun nm` / `strings` of UtttKit: without the feature `uti_big` 0, `bd_` 0, BubbleData 0, UtttBig 0; with `UTTT_TESTFLIGHT_CONDITIONS='$(UTTT_BIG_BOARD_CONDITION)'` `uti_big` 35, `bd_` 12, BubbleData 171, UtttBig 146. `shared/tools/release_strings.sh` is clean on both.
With the grid hold: without the feature UtttKit has 0 `uti_big`, `UtttBig`, `UtttModeHold` and `setModeHold` symbols and 0 `uti_big`, `UtttBig`, `setModeHold` and `setGridHold` strings, and the extension 0 `setGridHold`; with it `uti_big` 37 symbols (the two hold numbers added), `UtttModeHold` 11, `setModeHold` 2, and 146 `UtttBig` strings. `release_strings.sh` clean on both.

NOT proven: a real pinch (idb has none; the preview app's UI test pinches the board view), a second device, a real send between two phones (the simulator's thread is one participant), Again on a finished big game, the grid hold under a real finger on a phone (only idb's simulated 5 s touch and XCUITest's press), a hold on a watched game's grid and on a SENT, unanswered invitation's lobby (each is the same `setGridHold` call; the sent case only toggles because `draftURL` is cleared on the send), and the haptic (a simulator has none).
