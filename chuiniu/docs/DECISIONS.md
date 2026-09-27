# Chui Niu - decisions

The one decisions doc for the proof of concept.
The owner asked for no full design phase: decide, write one paragraph here, move on.
Each section has one owner; a worker writes only inside its own section.
The owner can veto any decision individually.

## Rules (owner: the kernel worker; R1 onward)

R1: 2 to 6 seats, five six-sided dice each at the start, one die lost per lost challenge, out at zero dice, last seat with dice wins.

R2: a bid is (quantity, face) with face 2 to 6; 1s are wild and count toward every face.
A bid is higher than the last when its quantity is greater, or its quantity is equal and its face is higher.
There is no bid on 1s and no "spot on" call in the proof of concept.

R3: instead of raising, the player on turn calls the last bid.
Every die is revealed.
If the count of the bid's face plus the wild 1s is at least the bid's quantity the caller loses a die, otherwise the bidder loses a die.

R4: the loser of the call bids first in the next round; if the loser is out, the next live seat after it in seat order does.
Round 1 opens with the host (seat 0).
Turn order is seat order among the seats that still hold dice.

R5: the opening bid of a round is any (quantity, face) with quantity at least 1.

R6: every round's dice are derived, never rolled and never sent (see K2), so cancelling a staged bubble and reopening the extension produces the same dice.

R7: a bid's quantity is at most the dice on the table, so a round has a top bid (that many 6s) after which only the call is left.
Without the cap a round has no longest bid sequence and no game has a size bound (K5).

R8: after a call the table stays revealed (every die as it stood at the call, and who lost) until the next round's opener bids; the next round's dice are already rolled under the cups and the opener sees their own.
Turn order skips seats with no dice, and a seat that is out keeps its row and its "You're out" line to the end.

## Kernel and wire (owner: the kernel worker; K1 onward)

K1: C prefix `cn_`, kernel in `chuiniu/c/src/cn_*.{c,h}`, tests in `chuiniu/c/tests/`, bridge in `chuiniu/c/ios/` with the one header Swift sees (`cn_api.h`, module `CChuiniu`), Makefile targets `run`, `asan`, `ios-smoke`, `structgen`, `datagen`, `swift-smoke`, `ios-lib` as in `pickemup/c/Makefile`.

K2: dice derivation goes through `shared/c/deal_rng` only, seeded from a SHA-256 over already-sent state (the game seed, the round index and the digest of the log as it stood when the round opened) plus the seat, so no player can pick a roll by cancelling and re-staging.

K3: the recipe, pinned by a golden and by the test's own recomputation (`tests/cn_dice_test.c`): `log = SHA-256("chuiniu.log.1|" || q0 f0 q1 f1 ...)` over the moves before the round opened (a call is q 0, f 0), `key = SHA-256("chuiniu.dice.1|" || seed || round as u16 little-endian || log)`, and die i of seat s is `1 + deal_rng_bounded(6)`, the i-th draw of `deal_rng` keyed by `key` at block `s << 32`.
A seat on k dice holds the first k draws of its own stretch, so no seat's dice depend on another seat's count, and changing any of it is a new format.

K4: one move is one bubble, and the body is pickemup's history-as-code: each move is its index in `cn_legal`'s menu (the call first when legal, then every legal bid by ascending rank) as one mixed-radix digit (`shared/c/mixrad`), a move with one option costs nothing, and the header (magic 0xC5, format 1, phase, flags DM and LEFT, seed, lobby_rev, moves, seats, starter, roster, a 2-byte check) is pickemup's cut to what a dice game has.
Decode is replay through `cn_apply` and the header must agree with the replay; the encoder decodes its own output before handing it over.

K5: every game fits one `MSMessage.url`: the longest game there can be is 2,349 moves at six seats (`CN_MAX_MOVES`, every rank bid in every round), every digit is under 8 bits, and with six 48-byte names the bound is 4,391 characters, asserted at compile time; `cn_msg_test` builds that game and measures 3,058.
Real games measure about 130 characters at two seats and 275 at six (median), 370 at most over 40 games a size.

K6: the race rule is pickemup's Rule P cut to one move a bubble: another game loses to the tapped one, a started chain beats a lobby, more moves win, then the higher lobby_rev and more seats, then the smaller SHA-256 of the envelope bytes.
The seat resolver (record, tag, sender, name, the lobby gate and a gone record) and the seat records are pickemup's unchanged, with `chuiniu.` salts.

K7: the lobby is pickemup's (D27) at capacity 2 in a DM and 6 in a group; whoever presses Start, seat 0 opens round 1 (R4), and the starter is kept in the header only so the start bubble has a sender.

K8: a staged call reveals nothing: the bridge's view is always the committed game, the staged plan and beats stop at the CALL, and a call's caption names the call only ("Bo calls four 3s"), because a staged bubble and its caption are on the caller's screen before it is sent and a cancel would otherwise buy a free look.
The outcome ("Bo calls. Four 3s was true, Bo loses a die") is a screen line (`CN_API_W_OUTCOME`) once the call is committed.

K9: the outcome line says every clause, the call and its loser first, then who went out, then who won (the winner's clause replaces the last seat's "is out"); captions are not held to a column cap in the proof of concept.

K10: the view is the one read a host draws from: my own dice sorted ascending, every seat's count, the menu (`can_raise`, `can_call`, the lowest raise, `max_q`, and `min_q_face[f]`, the least legal quantity per face, I3), and the newest call's reveal with `shown_counts` marking the dice that count (I4); another seat's dice reach it only through a reveal.
Plan events carry no hidden information (a REVEAL's dice are public), so a plan needs no viewer.

K11: the words are pickemup's i18n shape (`c/i18n/keys.h` and `strings_en.c`, read by `shared/tools/datagen`), English only; quantities are words to twelve and digits above, with a capitalised set for the start of a sentence.

K12: the motion is one beat per plan event on a fixed clock (a reveal is a LIFT and a COUNT), and `cn_beats_frame(now)` is the whole interface, as `pk_beats_frame`; `make ios-lib` writes `ios/Generated/ChuiniuKernel.swift` and `ios/Generated/i18n/ChuiniuStringKeys.swift` and `ChuiniuStringsEn.swift`, and stamps the library with the layout hash (`cn_api_layout_hash` against `SG_LAYOUT_HASH`).

K13: hidden dice are grade B (`docs/IMESSAGE_APP_IDEAS.md` 1.3): every die is derivable from the link by anyone with a decoder, and only the honest client masks them.

## iOS and rendering (owner: the iOS worker; I1 onward)

I1: SwiftUI, in `pickemup/`'s shape: `ChuiniuKit`, `ChuiniuMessages`, `ChuiniuMessagesApp`, project `chuiniu/ios/Chuiniu.xcodeproj` from `chuiniu/ios/project.yml`, module `CChuiniu`, `chuiniu/ios/vendor/Chuiniu.xcframework` from `make -C c ios-lib`.

I2: every kernel read goes through one file, `ChuiniuKit/Kernel/KernelSeam.swift`: a plain struct `TableModel` the screens draw and a `Kernel` protocol with one method per touch.
Its `FakeKernel` is the only place a fake value lives, and the tie-together replaces it by returning a bridge from `KernelSeam.make()`.

I3: the bid picker never ranks two bids: the kernel's `Menu` carries the least legal quantity for each face 2 to 6 (`minQuantityByFace`) plus the opening selection (`minimumRaise`), and Raise is lit when the chosen quantity reaches that face's number.
So the kernel must export that per-face table (or the tie-together derives it in C), not only "the minimum raise".

I4: which dice count at a reveal is the kernel's per-die flag (`Reveal.counts`), drawn as a brass ring, the rest dimmed; Swift knows 1s are wild only as a look, the 1 face's pip in foolish's deep red #8B1A1A.

I5: the roll's durations and curves live in one enum, `RollBeats` in `Board/DiceRoll.swift`, and `FMotion` in the copied `Tokens.swift` types pickemup's three kernel numbers as literals; both give way to the kernel's beats when it exports them.

I6: seat placement is `DiceTableLayout` in Swift for the scaffold: my seat is the bottom band with my dice, the others go left to right in seat order round the upper half of an ellipse, the bid plate sits between.
It is a pure function with a test, so moving it into C behind the seam (as pickemup's `pk_lay.c`) changes no screen.

I7: the felt and wood are pickemup's baked JPEGs copied into `ChuiniuKit/Resources`; the icons are two placeholder dice on felt green drawn by a throwaway script, to be replaced before any store build.

I8: `Design/Die.swift`, `Design/Cup.swift` and `Board/DiceRoll.swift` name no product and no rule, so they are candidates for a later lift into `shared/swift` (not done in the proof of concept).

I9: bundle ids `cards.chuiniu` (container), `cards.chuiniu.msg`, `cards.chuiniu.kit`, `cards.chuiniu.kit.tests`, and the App Group `group.cards.chuiniu` asked for by Debug only, from a hand-set `DebugAppGroup.entitlements` that xcodegen cannot blank.

## Orchestration (owner: the orchestrator; O1 onward)

O1: the proof of concept is built in three parallel packages (kernel and wire; iOS scaffold with the dice and cup primitive; legal, README and CI) and one tie-together package that wires the screens to the bridge and proves it on a simulator inside Messages.

O2: the working name is Chui Niu (吹牛), the owner's choice and the game's own generic Chinese name; it is threaded through one `GAME_NAME` string like Pick 'Em Up's.
The names Perudo and Dudo, pirate theming and any published product's cup or box art are avoided (`LEGAL.md`).

## BLOCKED (things only the owner can decide)

- A real trademark search on "Chui Niu" and on "Liar's Dice" as a store name.
- App Store Connect, signing, TestFlight and store metadata: skipped for the proof of concept on purpose.
