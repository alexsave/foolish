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

K14: the roll is a baked rigid-body throw, the kernel's (`c/src/cn_roll.c`): five rounded cubes in a cup that is held mouth up, shaken, flipped about the grip and slammed (`CN_THROW_CUP`), or poured out of a tipped cup onto the planks (`CN_THROW_TABLE`, kept for other dice games), simulated once at 240 Hz with contact impulses, Coulomb friction and the dice against each other, and handed to the host as every 60 Hz frame (a position and a unit quaternion for the cup and each die) plus which face of each die is up.
A host plays the frames on its display clock and integrates nothing; the dice stay in the cup by the throw's own speed, not by a barrier, and the throw's numbers were searched (the turn is four tenths of a second, half a second being the slowest that keeps the dice in; the shake runs two seconds, carries on through the turn, and is the hardest the cup holds, which is why the cup is 2.1 of its radius tall, `CN_CUP_TALL`, and the study draws it so); `cn_roll_test` measures that over 300 seeds, and that the six faces come up evenly.
A seat with no dice has its cup lying on its side across its badge, dim.
The peek is the head moving: the eye drops to 560 points and comes forward to 1.35 boards past the centre, the camera's turn is done as a homography of the whole screen (planks, badges, plate, shelf and canvas together, over a stage 1.9 by 2.2 of the screen so the tilt uncovers planks), and the cup tips only as far as every die needs, found per screen.
The table screen carries no headline and no ask line (the board is the whole screen above the shelf); every screen is centred on a plank and the tile is six planks wide so the running bond survives its seams; the far seats' counts are set at 184 of 256 on their crowns.
The eye is a seat's, not a lamp's: 720 points up and half the board's height past its centre toward me, with a shifted lens so the table and the layout on it stay 1:1 while every upright piece leans away from me (of my cup the side that faces me, of the others the side that faces the table).
The bake is deterministic to the bit across compilers and wasm (its own trig series, `-ffp-contract=off`, one random draw per statement) and a pinned golden holds the recipe; the bake knows no pip values, so whether the hand is the physics' own or K2's dice painted onto the up faces is the owner's call, and the bubble carries the hand, never the throw.
Every seat throws at a roll: a far cup's throw is the same bake with its own seed, shake length and start, its reach and gravity scaled to its size (dynamic similarity, the clock unchanged), and every shake length rounds to the bob's beat, since where the bob is when the turn starts is what decides whether the dice stay in.
`make -C c wasm-roll` links it as a browser module and `make -C c docs-roll` embeds that in `docs/UI.html`, which plays it.

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

I10: the reveal's motion is the kernel's: `RevealScreen` samples `cn_api_beats_frame` every display tick from the moment the plan was built, keeps the cups down until the LIFT beat, lights the counting dice one by one as the COUNT beat's `highlight_n` says (in seat order, a position and not a tally), and shows the loser's stamp and the outcome line only once the plan hands over to the next round's SHAKE.
The roll of a seat's own new dice (`DiceRoll`) still runs on `RollBeats`: its shake, lift and tumble are one seat's decoration with no kernel event of its own beyond the SHAKE, and driving it from the frame would mean rebuilding the primitive around a timeline for no visible gain in a proof of concept.

I11: `BridgeKernel` is the seam's one kernel and the only Swift that imports `CChuiniu`; every read goes through the generated readers and a stale pair (`cn_api_layout_hash` against `SG_LAYOUT_HASH`) reads nothing and refuses every adopt.
Where the model's shape differs from the kernel's the mapping is a representation and says so: a face with no legal raise is 0 in `min_q_face` and `maxQuantity + 1` in `Menu`, and with no raise left `maxQuantity` is the dice on the table.

I12: a move stages once it has rested, pickemup's collapse-after-settle cut to what the kernel exports: wait `cn_api_beats_staged`'s `total_ms`, collapse an expanded drawer, then insert; a lobby bubble (the invitation, a join, a start) goes in at once.
Chui Niu has no `settle_ms` of its own, so there is no extra lead or tail.

I13: R8's revealed table is kept by a look, never a move: after a call every phone with dice gets Next round (the kernel's `BTN_NEXT`), which only switches this phone to the next round's table (its own new dice, and the opener's menu); nothing is staged or sent, and the look is dropped by the next adopt.

I14: the seat records and the nickname live in the extension's own `UserDefaults`, as pickemup's do, because Release asks for no App Group; they are the kernel's `CN_API_REC_BYTES` bytes and a string, flushed after every call that can dirty them.
A device with no nickname creates a lobby under the kernel's fallback name ("Player 1") and first names itself in the Join field; there is no separate name gate in the proof of concept.

I15: two people on one simulator, for the rig: in a Debug build the App Group file `dev.seat` (`rig.sh seat WORD`) makes the extension that person, with identity bytes, nickname and seat records of its own, exactly as `cn_twophone_test.c`'s `be()` switches phone, and Messages' sender fact is withheld because the one real participant says nothing about the person.
A Debug build also writes the newest staged and sent links to `dev.staged` and `dev.sent`, so `tests/cn_link_dump.c` can decode exactly what the screen drew.

I16: the host's fixed labels are four new entries in the kernel's string table, read by key through `cn_api_string` (a bridge read, no kernel logic): `NAME_PROMPT`, `BTN_NEXT`, `STAMP_LOSES`, `STAMP_OUT`.
The scaffold's `quantity`, `face`, `wins` and `lobbyAlone` words were drawn by no screen and are gone; a seat alone in its lobby shows `LOBBY_WAITING`.

I17: the compact drawer's table is a short board: under 280pt the other seats go in one row along the top, the bid plate takes the rest of that row (at least 100pt, so at five or more other seats it is not drawn and the headline alone carries the turn), and my band is 64pt, one row of dice with the name and turn bar; the roll's cup may stand above that row.
The first run inside Messages showed the ellipse's seat and the plate drawn over each other there, which `DiceTableLayoutTests` did not see because it measured expanded boards only; it now measures three short ones too.

I18: the bubble's picture shows my staged raise (`TableModel.stagedBid`), because the committed table does not hold it until it is sent, and every name on it is the seat's bare name, since a bubble is seen by every phone and "(You)" belongs to the lobby screen's own row (`SeatModel.lobbyRow`).

## Orchestration (owner: the orchestrator; O1 onward)

O1: the proof of concept is built in three parallel packages (kernel and wire; iOS scaffold with the dice and cup primitive; legal, README and CI) and one tie-together package that wires the screens to the bridge and proves it on a simulator inside Messages.

O2: the working name is Chui Niu (吹牛), the owner's choice and the game's own generic Chinese name; it is threaded through one `GAME_NAME` string like Pick 'Em Up's.
The names Perudo and Dudo, pirate theming and any published product's cup or box art are avoided (`LEGAL.md`).

## BLOCKED (things only the owner can decide)

- A real trademark search on "Chui Niu" and on "Liar's Dice" as a store name.
- App Store Connect, signing, TestFlight and store metadata: skipped for the proof of concept on purpose.

## Bot (owner: the bot worker; B1 onward)

B1: the bot is offline only: `chuiniu/c/bot/` (a C module, its tests and the arena), built by its own `chuiniu/c/bot/Makefile` against the kernel's sources by relative path.
Nothing of it is in the bridge, the lobby or `chuiniu/ios/`, and it decides from a `CnSeen` (my dice, the counts, the moves, the hands shown at past calls), never from a `CnGame`.

B2: the opponent model is a quantal response: a seat raises or calls with probability proportional to exp(8 x the chance the claim is true given its own hand and the prior for everyone else), normalised over every legal option.
Each seat's belief is the exact posterior over its hand as a multiset of faces (252 at five dice), not a per-face weight table, because 1s are wild and so one die moves every face at once.
Rollouts are level 1: I read the other seats' rollout bids, the rollout opponents judge by their own dice and the prior and read nobody's bids; the arena measured every other reading as worse (`BOT.md`).

B3: the claim question "is this bid true right now" is answered in closed form, never by sampling: the direct binomial sum for the flat p (`cn_claim_prob`), and the exact convolution of the other seats' posterior count distributions when the belief refines it (`cn_belief_claim`); the call's value is that closed form, and only raises are rolled out.

B4: the only baseline is uniform over the legal options (the owner's call); the bot's own ablations, each one switch away from it, are what measure the opponent model.
Design, constants and the measured numbers: `chuiniu/docs/BOT.md`.

B5: the arena's dice-lost standard error takes the Bessel correction (divide by n - 1), from `shared/c/stats`, the same estimator every other arena uses; before it divided by n, which reads the interval a hair narrow (at 20 games, prior's [2.058, 3.742] becomes [2.036, 3.764]; at 400 games nothing prints differently).
