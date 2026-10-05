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
The table screen carries no headline and no ask line (the board is the whole screen above the shelf); every screen is centred on a plank and the tile is six planks wide so the running bond survives its seams; a cup under 40 points of radius has its count set at 184 of 256 on its crown.
The camera is fixed at the leaning head's place on every screen at all times (round twenty-one: steadier than moving it for the peek), the bubble included; the peek only tips the cup. The held cup leans .38 radians toward my face while it is shaken, as a player holds it to look in, and the turn starts from that lean.
The eye is a seat's, not a lamp's: 720 points up and half the board's height past its centre toward me, with a shifted lens so the table and the layout on it stay 1:1 while every upright piece leans away from me (of my cup the side that faces me, of the others the side that faces the table).
The bake is deterministic to the bit across compilers and wasm (its own trig series, `-ffp-contract=off`, one random draw per statement) and a pinned golden holds the recipe; the bake knows no pip values, so whether the hand is the physics' own or K2's dice painted onto the up faces is the owner's call, and the bubble carries the hand, never the throw.
Every seat throws at a roll: a far cup's throw is the same bake with its own seed, shake length and start, its reach and gravity scaled to its size (dynamic similarity, the clock unchanged), and every shake length rounds to the bob's beat, since where the bob is when the turn starts is what decides whether the dice stay in.
`make -C c wasm-roll` links it as a browser module and `make -C c docs-roll` embeds that in `docs/UI.html`, which plays it.
The study's renderer is in the same module (`c/wasm/cn_scene.c`, browser-only, host-tested by `cn_scene_test`): a rasterizer with a shadow map, so a cup shadows the table and the dice, a die in a shaken cup shadows the cup's floor, and nothing leaks through a wall; the page transforms the meshes and reads back pixels.
Round twenty-two: every seat's cup is my cup's size (the far ones are smaller in the frame only as the tilt makes them), and the ring is fitted to the camera, not to fixed fractions of the board: the other seats sit on an ellipse through my cup at equal angles, as tall as the glass allows (the farthest cup's crown, leaning up the screen from the fixed eye, comes just under the plate after the tilt; a ring is at most twice as tall as wide) and as wide as the screen allows after the tilt; cups whose pictures would touch (mouth, crown and the body between) are all made smaller until none do, and a lying cup is drawn in toward the centre as far as it must to stay on the screen. A short board keeps the row, its cups as big as the row allows.
The HUD is off the table: the bid plate (at the top of the glass on a tall board, beside the row on a short one) and the picker sit flat and untilted over the leaning view; the names stay on the planks, under each cup's mouth or, where a nearer cup leans over that spot, at the foot of the cup toward the ring's centre.
Gravity is always on: a throw's dice sit settled on the cup's floor from its first frame (a half second of unseen settling before the frames), and each cup rises from low over the table to the held height over the first .35 s of its hold, eased so nothing jolts the dice; a seat whose throw waits its turn shows a cup low and dice at rest, never dice in the air.
The contacts are resolved one corner after another, and a corner taken first takes the most of a landing; in one fixed order that favoured the +x and +y faces by a seventh over thousands of throws (chi-square 35 over 1200 seeds). The order's axes are flipped with the step and the die, and the faces come up evenly (chi-square 4).
The renderer draws the faces nearest the eye first (bucketed by height, so a covered fragment is mostly never shaded), from the copy of each texture nearest the face's size on the screen (half-size copies down to 8 texels, made on first use), with a slope-scaled shadow bias (a surface the light grazes is pushed further toward it, or the map's steps stripe it), a shadow edge softened over two texels, and contact: the map gives the light's shadow only, so each body's footprint (a die's, a standing cup's mouth, a lying cup's side) darkens the table under and just past it, most at the rim and fading over .6 of the radius, less the higher it is held, and a wall's foot within ten points of the table is creased; without that a cup's lit side looked pasted on and its cast shadow detached; the cup's textures bake no light any more (the side's lit line at the crown's edge read as a bright ring all round, the crown's radial highlight and vignette ignored the light), and the crown stays flat over a fillet of .09 of the radius with its own normals. The shadow bias is a normal offset (the sample point moved out along the surface's normal by a texel and a half times the sine to the light) plus a small constant: a depth bias that grew with the slope pushed the top of a cup's shaded side out of the cup's own shadow, a lit band under the crown on the dark side, which is what the owner was pointing at. A body's light is the lamp's by the cosine, less the shadow on it, on an ambient floor (.74 dark to .04 lit of the tint), so a face turned from the lamp and a face in its shadow look alike and the self-shadow's edge hides in the terminator; the roll's frames are drawn at 1.5 a point at most and the still frame at up to 2.5. The canvas reaches above the board to the glass's top (untilted) and past its sides, since a far cup leans up and out; the arena is 256 MB.

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

### The table drawn by the kernel (packages A to D, folded in by D)

What the four packages built, in one place; the package reports (`chuiniu/c/docs_pkgA.md` to `docs_pkgD.md`) keep the measurements and the mutation tables.

- The renderer (A): `c/src/cn_scene.c`, a software rasterizer with a shadow map, one a process, drawing into a caller-owned arena (`CN_SCENE_ARENA_IOS` 48 MB, the study's 256 MB); a frame that does not fit fails cleanly; a frame is prepared once and then drawn in up to 16 horizontal bands a pass on any threads, the same bits for any band count; on an M1, 16 bands draw a 2x six-seat frame in 6.8 ms against 21 on one thread.
  196 frames captured from the study replay natively byte for byte, and clang, gcc and wasm draw the same hashes.
- The bodies, the camera and the layout (B): `cn_geom` (the cup and die meshes, a standard right-handed die with opposite faces summing to 7, a lying cup resting on both rims, the pose of a bake at any instant), `cn_cam` (the fixed leaning-head eye, the screen's turn as a CATransform3D and a homography, the peek's least tip), `cn_lay` (the study's screen: short or tall on the board's height, the ring fitted to the camera, the names, the throws, any seat may be me), pinned to the study's own numbers from headless Chromium at five sizes, 2 to 6 seats.
- The textures (C): a 345 KB pack baked at build time (`make tex`: the verdigris and bone tiles, the numerals 0 to 9 in IM Fell English at 112 and 184 texels), from which every cup's side, inside, floor and crown and every die's atlas is derived at upload, within 0.4 of 255 of the study's canvas textures; nothing is rendered procedurally at launch.

I19 (the hand): the dice are the kernel's fair deal (K2); the physics bake only decides which face of each die ends up, and the dealt value is painted on that face (`cn_die_cells` about the bake's up face), the other five faces a standard die round it.
The labelling is the die's own from the throw's first frame, so nothing changes face when the dice come to rest, and the stage has no hand to report: `cn_stage_test` reads the up face off every resting die's pose (not off the bake's own answer) and finds the dealt value on 300-odd dice over 40 throws, tables and reveals, with every opposite pair summing to 7.
A seat whose dice this phone may not know (another seat at the table) throws a die with the plain labelling, under its cup.

I20 (memory): one texture set a stage: one cup side, inside and floor (my cup's seed), one die atlas, and a crown a seat (its own seed, count and lie), 12.2 MB with the half-size copies; per-seat full sets (58 MB) never fit.
The set is a pure function of the begun table and goes up into an emptied renderer whenever the crowns change, followed by one throwaway frame that makes every half-size copy before a real frame's buffers are taken, so a picture never depends on what was drawn before (the purge test draws the same bytes from a new arena at another address).
Each frame's canvas is cut at the top to the highest point any body's picture reaches in it, never above the study's (which reaches to the glass's top, untilted: 258 to 903 points above the board), because the study's canvas made a 2x frame 42 MB on its own.
A frame that does not fit the arena at the scale asked is drawn at the next scale down (2, 1.5, 1) and the shot says which; throw frames are asked at 1.5, still frames at 2, never 3.
Measured at 48 MB, six seats, Fay out: 390 by 340 and 375 by 541 draw still frames at 2x; 390 by 718 and 430 by 830 on my turn at 1.5x (the 390 by 718 frame is 36.1 MB at 2x beside 12.2 of textures); 430 by 830 on their turn at 1x (its far cups lean 361 points above the board).
2x everywhere needs either a larger arena or a renderer with fewer bytes a pixel (24 now); that is the renderer's to decide, not the stage's.

I21 (the clock): the stage's clock is the host's display clock from the moment it began the stage, the same clock it samples `cn_api_beats_frame` on.
A table begun with `roll` throws from the current plan's SHAKE beat's start (from 0 when the plan has none), so every beat before it (the call, the lift, the count, the drop) plays uncut; every seat's throw runs its own length from there (its delay, shake and settle are the bake's).
`total_ms` is when every throw is idle and never before the SHAKE beat's end, and a compile-time assert holds the kernel's `CN_T_SHAKE` (760 ms) under the shortest shake a throw has (1.5 s), so the beat lies inside the roll.
`rest_ms` is when my own dice are at rest: nothing is staged before it (I12's settle comes after it), and `cn_stage_done` is the display link's stop.

I22 (the stage's input is the kernel's): the host names the screen, the drawer's size and its scale; the bridge fills the table from the resident game (counts, outs, whose turn, my dice as the view sorts them, so the HUD's die places line up with `CnView.my_dice` and `shown`; a reveal's dice are the newest call's).
A round's throw seed is `cn_stage_round_seed(game seed, round)` (FNV-1a over a tag, the seed and the round), the same on every phone; a reveal throws the called round again (the round before the current one, or the current one when the game is over), so its dice lie where they landed.
A table of a finished game, a reveal before any call and a spectator begin nothing.

I23 (the reveal): every standing cup tips about the far edge of its mouth, as my cup does for the peek, to the least tip that shows its own resting dice from the eye (`cn_cam_peek_angle` per cup), by the current plan's LIFT beat's progress (all cups together, as the kernel lifts them; with no LIFT in the plan they are up).
The study's reveal is a flat list of dice; the stage draws the dice where they lie and the HUD gives each one's place on the glass for the counting rings (I4).
The reveal's shelf is one row (Next round or New game), the roll shelf's height.

I24 (the bubble): 300 by 195 points exactly, no canvas past it (the cups' crowns stay inside), the study's row of cups at 46 (radius 19, 16 past four seats), a lying cup with an inside and a standing one without, from the camera the study turns about (150, 150); the plate and the names are the host's, at the HUD's places.

I25 (the HUD): FLAT places are the drawer's points before the camera's turn (the planks, the names, the board, the picture's canvas, all turned together by the HUD's `ca` about the origin); the plate and the shelf are never turned; GLASS places (my cup's tap ellipse, the dice) are after the turn.
The HUD and each frame's shot are read in Swift through structgen's readers only; the HUD says `rolls` rather than carrying a sentinel, because structgen writes `0xFFFFFFFFu` as -1 while the reader returns the u32.

I26: `cn_geom.h`'s die texture slot is `CN_TEX_DIE_ATLAS`; it was `CN_TEX_DIE`, which `cn_tex.h` also declares, so no file could include both (the icon tool had worked round it).

I27 (the study's materials on iOS, package T): the owner's verdict on the first stage build was that its wood "is closer to that of a foolish wooden table rather than the dark aquatic theme we worked so hard to design", so every surface Swift paints is now the study's, and nothing of foolish's (its walnut and felt JPEGs, its orange plank buttons, its brass, its sans) is left in chuiniu.
The table is the study's planks (`TABLE_MATS.woodgrey2`: foolish's streak march in a drowned grey-teal palette under the stone passes, six planks of 86 points in a running bond, the nails baked in), made by `chuiniu/c/tools/cn_texgen.c` at build time as one seamless tile, 516 by 830 points at 2 texels a point (`cn_planks.jpg`, 1032 by 1660, JPEG at quality 80 through macOS's `sips`, about 185 KB); at the study's own scale and without nails it is the study's canvas to a mean of 0.38 of 255 (`cn_texgen --compare-planks`).
2 texels a point, not the study's 1: the stage turns the plane toward the eye and magnifies its near edge, and the nails (SVG in the study, sharp at any scale) are baked into the tile; the march itself is texel-sized and is read bilinearly at 2, as a browser shows the 1x canvas on a 2x screen.
The nail rows are the study's rule from its stage seed, with a row that would crowd the tile's seam left out, so they repeat with the tile; in the study they were drawn per stage and did not repeat.
Being seamless, the tile is laid as plain tiles and the walnut's mirrored 2 by 2 workaround is deleted; a plank's middle runs down the drawer's centre (`plankCentred`) and the tile's top is 58 points up, on the stage's 190% by 220% overdraw, the flat screens and the bubble alike.
The plates and every button are the study's verdigris (`cn_verd.png`, the pack's own tile, 128 points), with the plate's barnacle crust (`cn_crust.png`, `TEX.crust` ported to the same tool, 512 by 128 RGBA, a mean of 1.66 of 255 from the study's canvas) and the bone tile for the flat dice (`cn_bone.png`); all four are git-ignored build outputs of `make tex-ios`, as the pack is.
The faces are IM Fell English and IM Fell English SC (SIL OFL 1.1, google/fonts; the SC file was added to `chuiniu/c/tools/fonts`, the licence text is the same for both).
They live in ChuiniuKit's bundle, so they are registered for the process with Core Text at first use (`FType.registered`), not with `UIAppFonts`, which reads only the extension's main bundle.
A button is the study's `.plank` including the frame the browser drew round it: every plank in the study is a `<button>`, and its default 2 px outset border (#a8a8a8 top and left, #545454 bottom and right, measured on the capture) is part of what was signed off, so it is drawn.
Liar is the blood plate when the kernel offers the call, Raise the lit bronze plate when the kernel would take the bid, either sunk when not allowed; the stepper's minus is quiet, its plus bronze, Next round quiet, Start and Join lit.
The stepper's numeral alone is the system serif's lining figures, the fallback the study itself names (its Open tab): Fell's old-style 1 is a small capital I, and on the phone "1" read as "I".
The lobby is on the planks, like every screen, where the study put it on open water (`.sea`, whose caustic tile is not baked); the CJK lockup is not drawn (no Noto Serif TC in the bundle) and the kernel's title is set in the roman.

I28 (no headline on the table): the caption plate that said "Your turn: open the bidding" is gone; the study dropped the headline and the ask line in round nineteen.
The plate carries the bid on the table and nothing else, so before the first bid there is no plate; whose turn it is is the glow bar under a name, the name bright on its turn and dim otherwise (`.t-name`, `.t-name.dim`), in the small caps at 14 (12 on a short board), tracked .14em.
The reveal's tally steps down from the study's 26 through 22 and 18 to the roman's floor of 15.5 to fit the plate, then takes two lines, and is never cut; its outcome line sits on a `.seatband`'s dark wash so it reads over a cup the stage put under it.

I29 (nothing leaves the drawer, package V2): the kernel keeps every body CN_LAY_EDGE inside the drawer on every screen, at rest, through every throw frame and through every frame of the reveal's lift, 281 to 900 points tall and 2 to 6 seats (`cn_lay_test`, `cn_stage_test`).
The top margin grows a point a point from 8 to 30 over the 22 points above 400, where the study stepped it, so short and tall is one threshold (a drawer under 400 is short) and the board never shrinks as the drawer grows.
The ring is fitted to the glass as painted and kept off the plate (the study's own map is up to 36 points off far up a tall drawer); where the study's ring fitted, nothing moved, and 430 by 830 on their turn at four to six seats drew its side seats in by 3 to 9 points.
A far seat throws only where its held cup stays inside for its whole throw at the study's reach; elsewhere its cup stays down, as on a short board (on a tall drawer a top or side seat's throw left by 25 to 110 points; about one far seat in ten still throws, on drawers of 750 and up).
My throw's reach is the least that fits each of the drawer's screens, so it stays one reach; my dice fit my peek on a tall board too, and on a short board my standing cup stays off the row's names.
The reveal on a short board is one row of every seat, mine first, as the study's reveal lists them: the short table's row has no room above it for the lift (I23 swung its crowns 42 to 69 points past the top) and my tipped cup covered the middle seat; the row is as big and as high as lets every cup tip in full with its name, the loser's stamp and the outcome line under it, the tally's plate beside it where there is room, else under the names, else none.
The reveal's plate is 240 wide (the tally, "There were twelve", is longer than a bid), and the stage's lift is fitted to the drawer and the plate as the last word (`cn_lay_lift_fit`); 93% of cups over the tested drawers lift in full, every one on a short board.
The throw's lift starts with the cup's lowest rim 1 point (times the reach) over the planks, never under them.

I30 (the word): the call's button is Liar, the owner's word and the study's blood plate; the lines that name the button say it ("Your turn: raise or call Liar", "Send to call Liar on four 3s", the fourth rule), and the captions keep the study's verb ("Bo calls three 3s", "Bo calls. Three 3s was true, Bo loses a die").

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
