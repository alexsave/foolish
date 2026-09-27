# Pick 'Em Up - every decision taken on the owner's behalf

## How to read this

Every decision the team took without the owner has one row here: its id, the choice, the alternative it rejected, and the file that holds the full entry.
The owner vetoes by id, each one on its own, and never has to accept or reject a whole file.
Each full entry names the alternative it rejected, the reason for the choice and a confidence, so a veto can be weighed without reading anything else.
A SUPERSEDED row names the decision that overrode it, and the ones the workers themselves flagged as ones the owner may want to veto are marked **VETO?** in the last column and listed just below.
If this index and a full entry ever disagree, the full entry is the truth; fix this file.

Counts: 60 rules decisions (D1 to D57, plus D5b, D5c and D5d, of which D24 is superseded), 25 visual (U1 to U25), 39 iOS (I1 to I39, of which I10 and I16 are superseded), 17 animation (A1 to A17) and 9 orchestration (O1 to O9), 150 in all.

## Flagged for a possible veto

- D2, the call-out word "Last card!".
- D5 and D5b, who may catch and what a right and a wrong catch cost.
- D6 and D10, drawing when you could play, and the explicit pass.
- I4 (affirmed by O7), a draw stages nothing until the turn plays or passes.
- I11 and I12, a scrolling hand plays by tap only, and how the drawer's hand overflows.
- A3, A4 and A9, where the motion took the grid's prose or the demo script's numbers.
- O1, SwiftUI copied from foolish rather than Core Animation.
- O4, hands past thirteen cards overlap and then scroll instead of a hand cap.
- O8, two fixes made in foolish's own code on this branch.

## Rules (D), full entries in `RULES_AND_KERNEL.md` section 2

| Id | Choice | Rejected | File | Flag |
|---|---|---|---|---|
| D1 | 104 cards: per suit two each of 1-9, two Skip, two Reverse, two +2, plus four Wild and four Wild +4 | the familiar 108 with a 0 | RULES_AND_KERNEL.md | |
| D2 | the call-out word is "Last card!" | "Down to one!" or "Final card!" | RULES_AND_KERNEL.md | VETO? |
| D3 | "Last card!" may be said only in a message after the one that left you on one card | the same message, or only on your next turn | RULES_AND_KERNEL.md | |
| D4 | the catch window closes at the end of the next message that completes a turn | "one bubble is the window" | RULES_AND_KERNEL.md | |
| D5 | any other seated player may catch, in or out of turn, and the exposed player may say it out of turn | only the next player, inside their turn | RULES_AND_KERNEL.md | VETO? |
| D5b | a right catch costs the caught player 2 cards, a wrong one costs the catcher 1 | 2 and 2, or 4 for a right catch | RULES_AND_KERNEL.md | VETO? |
| D5c | at most one catch per message, never on a seat showing LAST | several catches, or a stamped seat as a miss | RULES_AND_KERNEL.md | |
| D5d | a catch is judged against the table at the message's start and dealt at its end | judged and dealt where the fan was tapped | RULES_AND_KERNEL.md | |
| D6 | a player may draw on their turn while holding a playable card, as often as there are cards | draw only when nothing plays, or exactly one | RULES_AND_KERNEL.md | VETO? |
| D7 | at 2 players one message may carry several turns when a play hands the turn back; a play to one card ends it | exactly one turn per message | RULES_AND_KERNEL.md | |
| D8 | a draw is committed at once, and plays, "Last card!" and "Caught you!" can be un-staged | everything undoable until Send | RULES_AND_KERNEL.md | |
| D9 | Messages' x rebuilds the draft to its floor and the drawn cards stay | the x throws the whole draft away | RULES_AND_KERNEL.md | |
| D10 | an explicit PASS, legal after a draw this turn or when nothing can be drawn or played | no pass, or a free pass | RULES_AND_KERNEL.md | VETO? |
| D11 | a Wild +4 may be played at any time, with no challenge | legal only without a card of the live suit | RULES_AND_KERNEL.md | |
| D12 | no stacking of +2 or +4 | stacking as a house rule | RULES_AND_KERNEL.md | |
| D13 | at 2 players a Reverse is a Skip, and no direction is drawn | flip the direction anyway | RULES_AND_KERNEL.md | |
| D14 | the start card is always a number; a non-number turned at the start is buried face up | the classic start-card effects | RULES_AND_KERNEL.md | |
| D15 | penalty draws are automatic, and a shortfall is forgiven | the victim taps the deck N times | RULES_AND_KERNEL.md | |
| D16 | with nothing drawable, n bare passes in a row end the game and the fewest cards wins | a stuck table is a draw | RULES_AND_KERNEL.md | |
| D17 | a wild has no default suit, and dismissing the picker cancels the play | the suit held most, or the live suit | RULES_AND_KERNEL.md | |
| D18 | the direction is a word in the top-right corner, hidden at 2 players | an arrow | RULES_AND_KERNEL.md | |
| D19 | seat 0 deals, seat 1 plays first, and the deal starts at seat 1 | a first player drawn from the seed | RULES_AND_KERNEL.md | |
| D20 | one hand is one game and the first player out wins | play to 500 points | RULES_AND_KERNEL.md | |
| D21 | a 32-byte seed, the game id from its SHA-256, and a ChaCha block range per reshuffle | uttt's 4-byte seed, or a separate 8-byte id | RULES_AND_KERNEL.md | |
| D22 | no hand count anywhere during play; the deck count is shown; every hand is shown at the end | hide the deck count too | RULES_AND_KERNEL.md | |
| D23 | caps of 1,500 turn actions and 750 messages, names of 16 characters and 48 bytes, no hand cap | a 30-card hand cap, or no stop | RULES_AND_KERNEL.md | |
| D24 | SUPERSEDED by O9: hand order is acquisition order, owned by the kernel | the player sorts or drags the hand | RULES_AND_KERNEL.md | |
| D25 | every bubble carries the whole game as seed, roster and one mixed-radix code | per-turn deltas | RULES_AND_KERNEL.md | |
| D26 | races between sibling bubbles are settled by one total order in C, with no merge | foolish's Rule P unchanged, or a merge | RULES_AND_KERNEL.md | |
| D27 | foolish's lobby minus the rules checkbox | uttt's "the joiner moves first" | RULES_AND_KERNEL.md | |
| D28 | a starter who is seat 1 may go straight into their first turn in the start bubble | a start bubble that carries the deal only | RULES_AND_KERNEL.md | |
| D29 | the lobby's verdicts live in `pk_lobby`, not the envelope | inside `pk_msg` | RULES_AND_KERNEL.md | |
| D30 | the history holds 3,750 records, and a say or catch rides inside its bubble's record | a record per say and catch | RULES_AND_KERNEL.md | |
| D31 | a bubble record carries a SEALED bit | telling the replay separately | RULES_AND_KERNEL.md | |
| D32 | catch legality reads the LAST stamps as they stood at the bubble's open | the live stamps | RULES_AND_KERNEL.md | |
| D33 | saying it is legal only when exposed both now and at the bubble's open | at the open only | RULES_AND_KERNEL.md | |
| D34 | test 7.7.1's second case is a refusal | a catch on a stamped seat as a miss | RULES_AND_KERNEL.md | |
| D35 | the masked view has reveal fields filled only at the end, constant three-back fans, and the draft's flags | section 3.8's sketch | RULES_AND_KERNEL.md | |
| D36 | the "since" counts are 16 bits | 8 bits | RULES_AND_KERNEL.md | |
| D37 | the plan's open details (the deal in bubble 0, action-half framing, the end at seal) | leaving them open | RULES_AND_KERNEL.md | |
| D38 | captions are composed from one bubble's events, with the keys the table lacked | one caption function over the game, literal names | RULES_AND_KERNEL.md | |
| D39 | an undo that would empty the draft closes it | an open, empty draft | RULES_AND_KERNEL.md | |
| D40 | every header digit is a menu of the options that make a real bubble | fixed bases | RULES_AND_KERNEL.md | |
| D41 | the header grows a starter byte and a LEFT flag | deriving both | RULES_AND_KERNEL.md | |
| D42 | this device's seat record is (game id, tag) | (game id, seat number) | RULES_AND_KERNEL.md | |
| D43 | `make run` sends 30 games a size through the wire, and the 10,000 run by hand | 10,000 a size in `make run` | RULES_AND_KERNEL.md | |
| D44 | the end reveal is a row struct per seat | two arrays and a second entry point | RULES_AND_KERNEL.md | |
| D45 | the bignum is lifted to `shared/c/mixrad` and uttt calls it | a copy in `pk_code.c` | RULES_AND_KERNEL.md | |
| D46 | the generated Swift is a build output and the library is stamped with the layout hash | uttt's flat scalar accessors | RULES_AND_KERNEL.md | |
| D47 | the seat resolver's fourth witness is the nickname, behind foolish's lobby gate | stop at uttt's three witnesses | RULES_AND_KERNEL.md | |
| D48 | a body must be minimal and end exactly on the sentinel | tolerating trailing zero bytes | RULES_AND_KERNEL.md | |
| D49 | the CI lane runs `run`, `asan`, `structgen` and `datagen`, not `wasm` | running `wasm` too | RULES_AND_KERNEL.md | |
| D50 | a play whose penalty reshuffled the deck stays undoable | moving the floor at every penalty draw | RULES_AND_KERNEL.md | |
| D51 | a seat record with no row means "not seated" and overrules the other witnesses | forgetting the record on a leave | RULES_AND_KERNEL.md | |
| D52 | a started header's `lobby_rev` must be one its roster could have started from | leaving it unchecked once started | RULES_AND_KERNEL.md | |
| D53 | the build flags stay `-Wall -Wextra -Werror`, with the stricter three a review-time check | adding all three to `CFLAGS` now | RULES_AND_KERNEL.md | |
| D54 | the phone's arrangement is `my_slot`, a permutation over acquisition order; the wire is unchanged | making `my_hand` the arranged order | RULES_AND_KERNEL.md | |
| D55 | an arrangement entry is (card, receipt), so arrivals go right and an undo finds its slot | keying by card id, or pruning cards that left | RULES_AND_KERNEL.md | |
| D56 | the arrangement is folded in before each of my actions and wherever the hand is read | also on every adopt | RULES_AND_KERNEL.md | |
| D57 | the arrangements ride the seat records' bytes; a bad block reads as acquisition order | a separate store key | RULES_AND_KERNEL.md | |

## Visual (U), full entries in `UI_DECISIONS.md`

| Id | Choice | Rejected | File | Flag |
|---|---|---|---|---|
| U1 | foolish's shipped felt, card faces, deep-red edge, fern back and wood, exactly | the first draft's teal gradient and rounded pills | UI_DECISIONS.md | |
| U2 | the pile keeps foolish's board centre, lifted 24pt in the drawer | the pills overlapping the pile's corner | UI_DECISIONS.md | |
| U3 | a portrait 50 x 70 deck 10pt left of the pile, with its count on the top layer | a label under the deck, or foolish's landscape stock | UI_DECISIONS.md | |
| U4 | the freed top-left corner holds the status line | no status line | UI_DECISIONS.md | |
| U5 | whose turn it is shows as a brass bar and a brass name | only role marks, as foolish does | UI_DECISIONS.md | |
| U6 | no numeral on any fan, every back drawn, compressed to 96pt | foolish's uncapped fan with a count chip | UI_DECISIONS.md | |
| U7 | the expanded hand follows O4, and the drawer keeps one row | O4's two-row box in the drawer too | UI_DECISIONS.md | |
| U8 | overlapped cards keep a 40pt full face | overlapped thin faces | UI_DECISIONS.md | |
| U9 | pills in a row with Draw fixed in the trailing slot | foolish's pill column | UI_DECISIONS.md | |
| U10 | the staged line is chips inside the status corner | a line of words under the pile | UI_DECISIONS.md | |
| U11 | the Last card! pill replaces the left squares while exposed | a third pill on the right, or a stamp on the hand | UI_DECISIONS.md | |
| U12 | the slot under a badge holds only a player's own speech or a verdict | foolish's role row, or an app-placed LAST | UI_DECISIONS.md | |
| U13 | a staged catch presses the fan with a ring and a tip, and never previews the verdict | a confirm sheet | UI_DECISIONS.md | |
| U14 | the suit picker is four tiles at the compass points over a scrim | a row of tiles, or a bottom sheet | UI_DECISIONS.md | |
| U15 | a chosen wild carries a band of its suit's colour along its foot | recolouring the whole card | UI_DECISIONS.md | |
| U16 | buried start cards peek face up from under the deck | hiding them in the deck | UI_DECISIONS.md | |
| U17 | the bubble is foolish's 300 x 195 snapshot with countable fans and no counts | the first draft's 274pt card | UI_DECISIONS.md | |
| U18 | a play is foolish's 500ms flight | the first draft's 420ms | UI_DECISIONS.md | |
| U19 | a draw is its own 320ms flight and 180ms flip, draws 110ms apart | cards of one event flying together | UI_DECISIONS.md | |
| U20 | the deal is one 320ms flight per card, clamped start to start | a fixed stagger | UI_DECISIONS.md | |
| U21 | the reshuffle gag: gather, fatten, riffle twice, then the draw | a plain fade | UI_DECISIONS.md | |
| U22 | named motions for the stamps, skip, reverse and reveal | foolish's role-mark motions | UI_DECISIONS.md | |
| U23 | a refused undo of a draw shakes the newest card and says why | silently ignoring the tap | UI_DECISIONS.md | |
| U24 | the deck drag is foolish's `DragGesture(minimumDistance: 0)` with high priority | a UIKit pan with a blocking delegate | UI_DECISIONS.md | |
| U25 | the two call words are written once each in the stylesheet | literal strings | UI_DECISIONS.md | |

## iOS (I), full entries in `IOS_DECISIONS.md`

| Id | Choice | Rejected | File | Flag |
|---|---|---|---|---|
| I1 | every layout number the screens use is C, in `pk_lay.c` | foolish's geometry copied into Swift | IOS_DECISIONS.md | |
| I2 | the screens' missing words were added to the kernel's table | Swift string literals | IOS_DECISIONS.md | |
| I3 | card ids, the results order and the buried cards are kernel entry points | decoding and sorting in Swift | IOS_DECISIONS.md | |
| I4 | a draw stages nothing; the draft is staged once the turn plays or passes | re-staging after every draw | IOS_DECISIONS.md | VETO? |
| I5 | at Send the draft is sealed only when the sent bytes are its link, else they are adopted | always committing the resident draft | IOS_DECISIONS.md | |
| I6 | Messages' x is `pk_api_cancel` when the resident still holds that draft | re-reading the staged link | IOS_DECISIONS.md | |
| I7 | a join that fills the table starts it in the same bubble | Join, then a separate Start | IOS_DECISIONS.md | |
| I8 | a lobby alone carries no button | an Invite button | IOS_DECISIONS.md | |
| I9 | the deck's drag is U24 exactly, with a tap under 8pt | (none; the device proof is open) | IOS_DECISIONS.md | |
| I10 | SUPERSEDED by O9 and I38: the hand has no drag-to-reorder | foolish's reorder | IOS_DECISIONS.md | |
| I11 | while the hand scrolls, a card is played by tap + Play only | a long press that lifts it out | IOS_DECISIONS.md | VETO? |
| I12 | in the drawer the hand stays flat to 22pt, then overlaps to 16pt, then scrolls | overlapping as soon as a card would go thin | IOS_DECISIONS.md | VETO? |
| I13 | textures are baked by the shared tool and committed | baking at run time | IOS_DECISIONS.md | |
| I14 | one square on the left, the rulebook | foolish's gear and book | IOS_DECISIONS.md | |
| I15 | the glyphs are SwiftUI paths from UI.html's SVG coordinates | C polygon lists | IOS_DECISIONS.md | |
| I16 | SUPERSEDED by O6 and I27 (action cards drawn without their suit shape) | - | IOS_DECISIONS.md | |
| I17 | catch verdict stamps show for the newest bubble only | until that seat's next move | IOS_DECISIONS.md | |
| I18 | the end reveal is every hand face up on the ring and a plank with the results and Again | (none named) | IOS_DECISIONS.md | |
| I19 | the picker tile positions were Swift numbers, since moved to `pk_lay_picker` (A9) | a `pk_lay_picker` entry point at the time | IOS_DECISIONS.md | |
| I20 | no render-server collapse ride; the board relays out as the drawer moves | porting uttt's `CollapseSlide` | IOS_DECISIONS.md | |
| I21 | the shared Send reminder is not compiled yet | compiling it with a new caption key | IOS_DECISIONS.md | |
| I22 | a layout hash mismatch shows the unreadable screen and reads nothing | (none named) | IOS_DECISIONS.md | |
| I23 | the `cards.pickemup` bundle ids and a Debug-only App Group set per config | (none named) | IOS_DECISIONS.md | |
| I24 | the nickname and seat records live in the extension's own defaults | foolish's App Group nickname | IOS_DECISIONS.md | |
| I25 | no snapshot goldens; a render test pins the bubble size | foolish's snapshot tests | IOS_DECISIONS.md | |
| I26 | placeholder icons from a throwaway script | (none named) | IOS_DECISIONS.md | |
| I27 | O6 drawn as the study's corner column in two corners, and said through the kernel | the shape top-left only | IOS_DECISIONS.md | |
| I28 | the strip's chips are the glyph alone, as UI.html draws them | a thin face with a rank | IOS_DECISIONS.md | |
| I29 | which events an adopted bubble plays is the kernel's (`pk_api_adopt`, `pk_api_beats_now`) | keeping the Swift branch that only called kernel functions | IOS_DECISIONS.md | |
| I30 | a tap on a fan is one kernel call, and a refused new call keeps the old one | un-calling then calling in Swift | IOS_DECISIONS.md | |
| I31 | the board's zones, insets, hand padding, tap slop and pill height are C; a component's inside is its view's | (none named) | IOS_DECISIONS.md | |
| I32 | the toast and "drawn cards stay" times are timeline constants, and a showing carries a generation | (none named) | IOS_DECISIONS.md | |
| I33 | no string literal a player sees is left in a view | (none named) | IOS_DECISIONS.md | |
| I34 | a layout mismatch is two vectors with two owners, `Pk.snap` and `PickemupHost.readable` | (none named) | IOS_DECISIONS.md | |
| I35 | adopting a bubble that is not my staged draft voids any stage still resting before its insert | (none named) | IOS_DECISIONS.md | |
| I36 | the deck's drag is owned by one recognizer in this process; the drawer's swipe stays a phone proof | (none named) | IOS_DECISIONS.md | |
| I37 | the collapse flag, the stamp order and uttt's readiness fallback stay Swift on purpose | (none named) | IOS_DECISIONS.md | |
| I38 | foolish's drag-to-reorder in the hand row; the row beats the pile on release; none while it scrolls | a long press to lift out of the scroll | IOS_DECISIONS.md | |
| I39 | Swift names a card by its acquisition position; the slot is geometry, mapped only by the kernel | working in slots through `pk_api_play_slot` | IOS_DECISIONS.md | |

## Animation (A), full entries in `ANIMATION_DECISIONS.md`

| Id | Choice | Rejected | File | Flag |
|---|---|---|---|---|
| A1 | the whole timeline is C, and Swift only samples it | a Swift animator of sleeps and `withAnimation` | ANIMATION_DECISIONS.md | |
| A2 | a plan starts from the kernel's replay to bubble `from` | the view the host remembered | ANIMATION_DECISIONS.md | |
| A3 | a turn bar moves 25ms after the last motion of its turn | the demos' no-gap timing | ANIMATION_DECISIONS.md | VETO? |
| A4 | where the grid is silent the demo script is the number | dropping the rests the grid does not list | ANIMATION_DECISIONS.md | VETO? |
| A5 | keyframes are placed on eased progress, as the Web Animations API does | keyframes on linear time | ANIMATION_DECISIONS.md | |
| A6 | each event is played, done or held by its half: settle waits for Send | (none named) | ANIMATION_DECISIONS.md | |
| A7 | channel A finds a tap's new events by matching plans by content | "the events after index k" | ANIMATION_DECISIONS.md | |
| A8 | my drawn and dealt cards fly face down and turn where they land | (none named) | ANIMATION_DECISIONS.md | |
| A9 | the suit tiles pop where they stand, 30ms apart, as the demo does | popping out of the card to the compass points | ANIMATION_DECISIONS.md | VETO? |
| A10 | a picked suit's ring and the tiles' collapse lead the wild's stage plan | (none named) | ANIMATION_DECISIONS.md | |
| A11 | motion with no anchor on this phone keeps its time and draws nothing | (none named) | ANIMATION_DECISIONS.md | |
| A12 | the wild's band snaps with the card for now | a band overlay the beat slides | ANIMATION_DECISIONS.md | |
| A13 | the lobby's join and leave beats are laid out but not played | (deferred) | ANIMATION_DECISIONS.md | |
| A14 | uttt's `CollapseSlide` is still not compiled; the collapse waits the kernel's `settle_ms` | (deferred until Messages can run) | ANIMATION_DECISIONS.md | |
| A15 | the shared Send reminder is still not compiled | (deferred until Messages can run) | ANIMATION_DECISIONS.md | |
| A16 | U21's budget is held by the synthetic turn and the eight-player p99 | (none named) | ANIMATION_DECISIONS.md | |
| A17 | the starter's own Start plays the deal at once, as channel A of bubble 0 | waiting for Send | ANIMATION_DECISIONS.md | |

## Orchestration (O), full entries in `ORCHESTRATION.md`

| Id | Choice | Rejected | File | Flag |
|---|---|---|---|---|
| O1 | the extension is SwiftUI, copied from foolish's views with `COPIED from` headers | Core Animation with C-drawn polygons, as uttt | ORCHESTRATION.md | VETO? |
| O2 | the `pk_` prefix, uttt's layout and target names, `Pickemup*` Swift targets | foolish's names | ORCHESTRATION.md | |
| O3 | lifts S0 to S2 before the screens, S3 with the kernel, everything else copy-first | lifting everything first | ORCHESTRATION.md | |
| O4 | hands past thirteen overlap to a 16pt strip, then scroll; no hand cap | a hand cap of 13 | ORCHESTRATION.md | VETO? |
| O5 | the name stays a working title in one `GAME_NAME` string | (the search is the owner's) | ORCHESTRATION.md | |
| O6 | action cards carry their suit shape as well as its colour | colour only | ORCHESTRATION.md | |
| O7 | a draw is never staged on its own (affirms I4) | a bubble after every draw | ORCHESTRATION.md | |
| O8 | two failures in foolish's proof set were fixed here: the base32 shift and a test lint | only reporting them | ORCHESTRATION.md | VETO? |
| O9 | the hand keeps foolish's drag-to-reorder, per phone and off the wire, overriding D24 and I10 | D24 as written | ORCHESTRATION.md | |

## BLOCKED

The current state of every blocked item lives in `ORCHESTRATION.md` under BLOCKED; this is a pointer only.

- B1: iOS simulators on this Mac hang host-wide; the owner reboots the Mac.
- B2: the extension has not yet been seen inside Messages; owed after B1.
- B3: the filmed and measured animation take; owed after B1.
- The final game name: the USPTO search and the choice are the owner's (O5).
- App Store Connect, signing and upload: the owner's, by hand.

## Found on the way

Open, and not pickemup's to fix in this pass:

- `werewolf/docs/UI.html` fails `shared/tools/check_ui_doc.py` ("a `<template>` is never closed"), a literal template tag inside a script comment; still failing on 2026-09-27.
- `foolish/e2e/validation/ci_toolchain_validation.test.ts` reads every `make ... wasm` line in every workflow as a build of foolish's test module, so no other product's lane can build its own wasm (D49).
- `.github/workflows/uttt-c.yml` runs uttt's `run` and `asan` but not its `ios-smoke`, which REUSE_AUDIT S3 planned.
- The repository `.gitignore` (line 45, on main before this branch) holds an em dash and names the agent tool in a comment.
- `REUSE_AUDIT.md` section 8: `rig.sh` restores entitlements with `git checkout` (D1), the drawer-collapse numbers exist three times (D2), flight timing is typed twice (D3), foolish compiles the shared insert gating but never calls it (D4), uttt's iOS README is stale (D6), and two XCTest counts disagree (D7).
- `pickemup/c/tests/pk_check.h`'s `seed_of` deals only 256 different games; the fuzz and wire tests use `seed_wide`, and `seed_of` stays for the committed 7.3 goldens.

Found and fixed on this branch:

- foolish's `replay_b32_encode` shifted a signed accumulator into overflow under UBSan, and `TableWireTests.swift` broke the architecture lint under Xcode 27 (O8, commits `b5c35900` and `b78c5788`).
- `.github/workflows/uttt-web.yml` did not rebuild uttt.live when `shared/c/mixrad.*` changed (`d0ca1c99`).
- `shared/tools/check_ui_doc.py` defaulted to this product's UI.html, which named a product under `shared/` (S0).
- U20's "between 3.2s and 4.2s" did not hold at two or three players; the sentence now says "under 4.2s" (ANIMATION_DECISIONS, the FOUND under A16).
- UI.html still said a draw re-stages the bubble, which I4 and O7 overrule; its draw and undo rows and two notes now say a draw stages nothing (the drawing in the "turn" view still shows a staged field after two draws).
- The kernel README's size table and fuzz numbers, and RULES_AND_KERNEL 4.5's measured paragraph, were from an earlier build; both now carry the 2026-09-27 `make run` numbers.
- `shared/README.md` did not list `c/mixrad` or `c/wasm`, or SHED against the shared code it builds.
