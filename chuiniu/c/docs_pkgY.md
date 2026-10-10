# Package Y - every bubble is one line

The owner's rule: "bubbles must always be a single line".
A bubble's caption (MSMessageTemplateLayout.caption, which MessagesViewController sets from the kernel's staged caption, and the same text as summaryText) and the bubble picture's plate words are each one line, for every game state, seat count, name up to the cap, bid, reveal and lobby line.

## The budget

Messages sets the caption in the system font under the picture; the study's `.bub-cap` is 17px in a 300px bubble with 14px padding a side, so the text has 272 points.
The kernel cannot know a font, so it counts a line by `cn_cap_width` (`src/cn_say.c`): an upper bound on the system font's advance at 17 points, the wider of regular and semibold (either may be what Messages draws), in eighths of a point.

- ASCII 0x20 to 0x7E: a table of the 95 advances, measured with Core Text on the iOS 27 simulator and on the Mac (the two agree to the eighth), rounded up.
  W and % are the widest at 16.5 points (132 units), i and j the narrowest at 4.2.
- Every other code point by the widest glyph of its class, as the phone measures it: a Latin letter (U+00A0 to 024F, 1E00 to 1EFF) 19 points (Œ 18.9), Greek and Cyrillic 24 (Ѹ 23.9), CJK ideographs, kana and Hangul 19 (18.7), emoji 24 (23.9), cuneiform 80 (78.8), U+FDFD 73 (72.4), the Latin digraphs (Ǆ 23.5) and everything else 46 (the widest found, U+2E3B, is 45.2); combining marks and joiners 0.
- `CN_CAP_BUDGET` is 272 points (2176 units).

The Mac's own scan first gave Cyrillic 22 and U+FDFD 61; BubbleLineTests on the simulator found both wider (23.9 and 72.4) and failed, which is why the classes are the phone's numbers.

## The steps (the kernel guarantees it by construction)

`fit_line` says the study's sentence when it is within the budget, which is the usual case ("Dice rolled. Alex bids first" is 193 points).
Otherwise the same sentence, shorter, in this order, and never a word cut:

1. the template's shorter form: `CAP_START_SHORT` "{who} bids first", `CAP_INVITE_SHORT` "{who} wants a game";
2. the bid in digits, "Alex bid 12 3s" (bids and calls; the face is always a digit);
3. the shortest form with digits, with the name clipped to the room left: whole characters (a base with its marks), trailing spaces dropped, then `CAP_CLIP` "…" ("WWWWWWWW… calls 12 6s").

Who and what survive every step: the name (whole, or a non-empty start of it and the mark), and the bid's quantity and face.
A caption is one act (the start, a bid or a call); `caption()` in cn_say.c says the newest act of a move, and a call still never says what it found (K8).
The outcome line ("Bo calls. Four 3s was true, Bo loses a die") is a screen line, not a caption, and is not held to the budget.

## The sentences that changed

- `CAP_INVITE`: "{who} wants a game of {game}. Tap to join" became "{who} wants a game of {game}".
  The old line never fit: "Alex wants a game of Chui Niu. Tap to join" is 317 points, and without any name 280.
- New: `CAP_START_SHORT` "{who} bids first", `CAP_INVITE_SHORT` "{who} wants a game", `CAP_CLIP` "…".
- Every other caption is unchanged whenever it fits; the bid's digit form ("12 3s") and the clipped name appear only past the budget.

The goldens that pinned the invitation moved with it: `tests/cn_say_test.c`, `tests/cn_twophone_test.c`, `ios/cn_api_smoke.c`, `ios/cn_api_smoke.swift`, `ChuiniuKitTests/BridgeKernelTests.swift`, and the comment in `cn_api.h`.

## Measured

Every caption over names of 1 to 16 code points (and at most the wire's 48 bytes) from the widest glyph of every class, narrow glyphs, spaces and a letter with its mark, one glyph throughout or cycling, every bid 1 to 30 of every face 2 to 6, bids and calls, the start, the invitation, a join and a leave:

| | widest caption | over 272 points |
|---|---|---|
| before (Mac, Core Text, 58,067 captions) | 652.9 points, "😀 x16 wants a game of Chui Niu. Tap to join" | 15,520 |
| after (iOS 27 simulator, 293,360 captions) | 270.6 points, "%%%%%%%%%%%%% bid 1 4" | 0 |

The usual invitation was 317.2 points before (it wrapped for everyone); it is "Alex wants a game of Chui Niu" now.

The bubble's plate (`CnStageHud` plate, 160 by 56 in the bubble) carries the bid (beside its 30-point die) or the reveal's tally.
In IM Fell English the fullest is "There were eleven", at 18 points, 99.8% of the 136 points it has; "eleven 4s" fits at 26 in 96.
Nothing needed a kernel short form, so the plate's words are the kernel's as before, and `BidPlate(oneLine: true)` in the bubble shrinks on its one line past the last size instead of taking two lines (the table's plate keeps I28's two lines for the tally).

## Tests

- `tests/cn_say_test.c`, "the bubble's caption is one line": 310,080 captions over the same name set (plus the fallback "Player 6") against the budget, who and what kept, the study's sentence whenever it fits, a name clipped only when no whole-name form fits, a clip never ending in a space; the step goldens; every caption of 60 played games at 2 to 6 seats with the widest names at every seat.
- `ChuiniuKitTests/BubbleLineTests.swift`: the kernel's bound against the phone's regular and semibold system font at 17 for every ASCII glyph and every class's widest; every caption through the seam (`cn_api_caption_probe`) against the real font; the plate's words at the bubble HUD's plate; and the staged captions of a real two-phone table named 16 W and 16 M.
- `ios/cn_api_smoke.c`: the probe's entry points.

Mutation checks (each red for the assertion named, then restored by re-editing):

| Mutation | Test that went red |
|---|---|
| `fit_line` accepts twice the budget | cn_say_test "the lobby past the budget, 2304 of 2176 units: 𒐫 wants a game of Chui Niu" |
| `clip_name` keeps the trailing spaces | cn_say_test "loses who (MMmmwwii  ŒŒѸ): MMmmwwii  … wants a game" |
| no digit step for a bid | cn_say_test "a call clipped a name that fits: 𒐫… calls three 4s" |
| the start has no short form | cn_say_test "the start loses who (﷽﷽): Dice rolled. … bids first" |
| ASCII W 132 lowered to 120 | cn_say_test "ASCII" and the step goldens |
| ASCII W lowered to 120 (iOS) | BubbleLineTests `testTheKernelsBoundIsAtLeastTheFontEverywhere` ("'W' is 16.47 points at 17, the kernel says 15.0") and `testEveryCaptionIsOneLine` ("a bid: 272.0 points, past 272.0: WWWWWWWWWWWW bid ...") |
| `fit_line` accepts twice the budget (iOS) | `testEveryCaptionIsOneLine` ("the invitation: 277.3 points ... ﷽ wants a game of Chui Niu") and `testTheStagedCaptionsOfAWideTableAreOneLine` ("468.4 is greater than 272.0 - WWWWWWWWWWWWWWWW wants a game of Chui Niu") |
| `BidPlate.sizes` only 26 | `testThePlateWordsAreOneLine` ("'There were 0' fits no size on one line in 136.0 points"); the other three stayed green |
| (before the fix) the Mac's class widths | BubbleLineTests: "'Ѹ' is 23.86 points at 17, the kernel says 22.0", "the invitation: 277.3 points, past 272.0: ﷽ wants a game of Chui Niu" |
