# Package P - the lobby's controls

## The cause

The creator's drawer showed the title, the roster and "Waiting for players", and nothing to press.
`LobbyScreen` drew exactly one control per kernel offer: Join for `CN_LOBBY_JOIN`, Start for `CN_LOBBY_START`, and `EmptyView()` for WAITING, INVITE and FULL.
A creator alone is offered WAITING (seated alone, and the newest bubble is theirs), so nothing showed.
Leave was never wired at all: the seam had no `leave`, the bridge never called `cn_api_leave`, and the string table had no word for it.
The SwiftUI scaffold (56ffed7d) had the same one-control switch, so this is not a regression of the theme work; the study's lobby (Start sunk until two are seated, then lit, beside Leave) was never built.
The bridge's `offered: .waiting` at BridgeKernel.swift's started-game branch is correct: a started game has no lobby.
The lobby branch already mapped the kernel's offer faithfully.

A second hole on the same screen: `cn_api_new` seats the creator under the nickname and refuses without an accepted one.
`MessagesViewController.create` returned on that refusal, so a phone that had never typed a name got a drawer with no seats and no way to give one.

The "knob" on each seat band was the top-down cup drawn with no count (the lobby's seats carried `dice: 0`).

## The fix

- Kernel strings: `BTN_LEAVE` ("Leave") and `LOBBY_YOU` ("(you)"), in the lobby section of `i18n/keys.h`.
- Seam: `Kernel.leave() -> String?` (the kernel's `CN_API_W_LEFT` caption, worded before the row goes), `TableModel.mayLeave` (the kernel's `can_exit`; named in the model's own words because the architecture lint refuses a struct that declares two kernel field names), `Word.leave` and `.lobbyYou`.
- `SeatModel.lobbyRow` is gone: the row is the seat's name in the small caps with the kernel's "(you)" dim after mine, as the study draws it, so the "1." prefix is gone. `CN_API_W_LOBBY_ROW` stays in the kernel (its C tests read it) but no host reads it now.
- The lobby's seats carry `CN_START_DICE`, so each band's cup is stamped with the dice the seat sits down with, as the study's is.
- `LobbyControls.of(table, nameAccepted:)` reads the kernel's verdicts into what the screen shows:

| Kernel state | Shown |
|---|---|
| not seated, room (JOIN) | name field, Join (lit once the kernel accepts the name, sunk before) |
| not seated, no room (FULL) | "This table is full" |
| seated, offered START | Start lit, Leave (quiet if `can_exit`, else sunk) |
| seated, WAITING or INVITE | Start sunk, Leave (quiet if `can_exit`, else sunk), "Waiting for players" |
| an unnamed creator (`cn_api_new` refused) | name field, Join; Join names me and makes the lobby with the seed already drawn |
| started | no lobby |

- Layout: the roster scrolls between the title and the controls, so six bands in a 328-point drawer never push Start or Leave out; every control is 44 points tall.

## The title

The study's lockup is 吹牛 over "Chui Niu".
No CJK face is bundled, and no OFL CJK font was on this Mac to subset (pyftsubset is), so the title stays the kernel's GAME_NAME in IM Fell English roman.
A two-glyph subset of an OFL face (Noto Serif SC, LXGW WenKai) would be a few kilobytes if the owner wants the lockup.

## Tests

`ChuiniuKitTests/LobbyTests.swift`, through the real bridge, phones switched as BridgeKernelTests does; mutation records in `chuiniu/ios/TESTS_MUTATED.md`.
