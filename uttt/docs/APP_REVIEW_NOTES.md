# UTTT - notes for App Review

The text between the two rules below is what goes into App Store Connect, both the Beta App Review notes (TestFlight) and the App Review notes for the store.
It is under 4000 characters and has no em dashes.
The live Beta App Review copy is `betaAppReviewDetails/6815039449`, patched with `~/.appstoreconnect/asc.py`.

---

Ultimate is an iMessage app for playing Ultimate Tic-Tac-Toe with a friend in a Messages conversation.
It has no home screen icon: it lives only in Messages, in the apps row next to the text field.

WHAT THE GAME IS
The board is nine small tic-tac-toe boards in a 3x3 grid.
Win a small board by getting three in a row, and win the game by winning three small boards in a row.
The twist: the square you play in sends your friend to the matching small board.
Play the bottom-left square of any board, and your friend must play next in the bottom-left board.
If that board is already won or full, they may play anywhere.
The rules are also in the app, behind the book button on the board.

IT NEEDS TWO PEOPLE
Every move is a message, so the game only moves forward when the other person answers.
On a single device you can start a game and send the invitation, but when you tap your own invitation you will see "Waiting / Nobody has taken it yet".
That is correct: you are waiting for your friend to take it, and there is nothing else to do on that phone until they do.
There is no computer opponent.

HOW TO TEST (two devices, two Apple IDs)
1. On device A (Apple ID 1), open Messages and start a conversation with Apple ID 2.
2. Tap the apps button next to the text field and choose Ultimate.
An invitation with an empty board is placed in the text field; send it.
3. On device B (Apple ID 2), open the same conversation and tap the invitation.
The board opens and B plays first, as X: tap any square, then send the move that is placed in the text field.
4. On device A, tap the new message.
A is O and must play in the small board that matches the square B chose (it is highlighted); tap a square there and send.
5. Keep alternating.
The newest message always carries the whole game, so tapping it opens the current position.
6. When someone wins three boards in a row, both devices show the result, and the expanded view offers "Again" to start a new game.
Before sending, you can change your mind by tapping a different square, or cancel the move with the X on the message in the text field.
A third person in a group conversation who taps the game can watch it, but cannot move.

WITH ONE DEVICE
The rules are behind the book button on the board.
A whole game, played to the end, can be watched in any browser, no install needed:
https://uttt.live/NK3JVACJ2ZRHDRWDEOL3WSCDAWHKKBSD
That page draws the game with the app's own code, the same board, marks and highlighter, and steps through all 39 moves.
Every finished game offers the same kind of link through "Copy code" on its end screen.

NO ACCOUNTS, NO NETWORK, NO PURCHASES
There is no sign-in, no account, no server and no network code.
The whole game travels inside the Messages message itself, so nothing is collected or stored outside Messages.
There are no in-app purchases, ads or tracking.

---

## One-device review, considered and not built

A reviewer with one device cannot see a move played, and the notes above say so plainly.
These were considered:

- **A computer opponent.** The bots exist in `uttt/c/src/uttt_bots.c`, but the product decision is that nobody plays a bot here (`UtttModel.swift`); a "solo" mode just for review would be a feature the store build does not otherwise have.
- **The DEBUG seat picker** (`UtttSeatChoice.swift`, `dev.seat`) plays both chairs on one phone. It is whole-file `#if DEBUG` and must not ship; a review-only door into it would be cheating.
- **The diagnostics panel** (hold the rulebook, `UtttDiagnostics.swift`) is DEBUG only: in the build under review the rulebook is a plain tap with no hold, so there is no hidden gesture to mention.
- **Two devices, one Apple ID.** Not reliable: the seat is a hash of the per-device Messages participant, but both devices would be the same person in the same conversation, and it is not a test we have run.
- **The replay link (added 2026-09-25).** `https://uttt.live/NK3JVACJ2ZRHDRWDEOL3WSCDAWHKKBSD` is a whole game, X winning down the right in 39 moves, played by two `quill` bots (`uttt_bot_move(BOT_QUILL, g, 40, &rs)` from `rs = 5`, seed 1790352000) and checked to round-trip through `uttt_replay_read` and to play to its end on uttt.live. It is the closest thing to watching a game on one device, and it is the shipping replay path, not a review-only mode.
- **The honest one-device path that exists today** is the invitation itself, the Waiting screen and, on the board screens, the rulebook.
  UI.html draws nothing else for the creator before somebody joins.
  A rulebook door on the Waiting screen would give a one-device reviewer the rules to read without changing what the game is; it is not in the spec and was not built.
- **A screen recording of a two-phone game** is the foolish pattern (`docs/APP_REVIEW_NOTES.md` at the repo root) and should be attached in App Store Connect before external TestFlight or the store; it needs two real phones and has not been recorded.
