# Pick 'Em Up - iOS decisions made on the owner's behalf

Every choice in `pickemup/ios/` the owner did not dictate and the study (`UI.html`, `UI_DECISIONS.md`) did not settle.
Each one can be vetoed on its own.
Rules are `RULES_AND_KERNEL.md`'s (Dn), visuals `UI_DECISIONS.md`'s (Un), orchestration `ORCHESTRATION.md`'s (On).

## Where the numbers and the words live

DECISION I1: every layout number the screens draw with is C, in `pickemup/c/ios/pk_lay.c`: the hand row (O4, U7, U8), the seat ring, the fan step (U6), the deck's layers, the pile's lift and the deck beside it (U2, U3), and which pill stands in which slot (U9).
Alternative: copy `FHandFan`'s geometry and `ringPoint` into Swift, as foolish has them.
Why: the owner's rule is C over a higher-level language, and REUSE_AUDIT.md 3.2 already recommends moving that geometry to C (S11, S12); in C the thresholds are pinned by the bridge smoke on any machine, with a mutation row each.
Confidence: high.

DECISION I2: the words the screens need and the kernel did not have were added to the kernel's table and to `pk_api_words`, never written in Swift: `STAMP_OUT`, `STAMP_WRONG`, `TOAST_NO_MATCH`, `LOBBY_ROW`, `LOBBY_ROW_YOU`, `BTN_UNDO`, and the composed lines `W_STAGED_CAPTION` (the caption of what `pk_api_text` writes), `W_LOBBY_ROW`, `W_PUBLIC_ROW` (the bubble's roster, with no "(You)"), `W_LOBBY_DEALER`, `W_ERROR` and `W_RANK_ROW`.
Alternative: Swift string literals for the few UI-only words.
Why: every word is the kernel's (the brief's rule), so a language is one C file.
Confidence: high.

DECISION I3: what a card id is (`pk_api_card_suit`, `pk_api_card_rank`), the results order (`pk_api_ranks`) and the buried start cards still under the deck (`pk_api_buried`, U16) are kernel entry points.
Alternative: decode the id order (3.2) in Swift, sort the reveal rows in Swift, and leave U16 out.
Why: the id order is part of the format, ranking is a rule, and whether a buried card is still at the bottom needs the deck; each is a few lines of C with a test and a mutation row.
Confidence: high.

## Staging and the conversation

DECISION I4: a draw stages nothing.
The kernel refuses to seal a draft whose last action is a draw (`pk_can_seal`, "mid-turn"), so the draft is staged only once the turn plays or passes (or carries a Last card! or a catch).
Alternative: the study's "re-staged after every draw, so Send is live" (motion grid, Draw x1), which the kernel cannot write.
Why: the kernel is the truth, and a draws-only bubble would be a turn with no end; nothing is lost, because the deck order is fixed by the seed and a reopened drawer draws the same cards again.
The study's rows for Draw x1 and "x on a draws-only bubble" should be updated.
Confidence: high.

DECISION I5: at `didStartSending` the resident draft is sealed (`pk_api_commit`) only when the sent bytes are exactly its link; otherwise the sent bytes are ADOPTED (`pk_api_read`).
Alternative: always commit the resident draft, as the bridge smoke does.
Why: Messages has no API to take a staged bubble back, so an Undo to nothing can leave an older bubble in the field; if that is the one sent, it is the move that happened.
Confidence: high.

DECISION I6: Messages' X on the staged bubble is `pk_api_cancel` (D9: back to the floor, the draws stay, SUB_DRAWN_STAY shows), and only when the resident still holds that draft; otherwise the screen is re-presented from the tapped bubble.
Alternative: re-read the staged link, which would seal the draft and lose the undo.
Confidence: high.

DECISION I7: a Join that fills the table joins and starts in one bubble (`pk_api_join_start`, 4.6.5), so the second player in a DM never sees a Start of their own.
Alternative: offer Join, then a separate Start.
Why: it is foolish's DM flow and the kernel offers it exactly then; a group table filling at eight starts the same way.
Confidence: medium.

DECISION I8: a lobby alone (`PK_LOBBY_INVITE`) carries no button, only "Only you so far".
Alternative: an Invite button that restages the lobby.
Why: the study draws none, and there is no word for one.
Confidence: medium.

## Gestures

DECISION I9: the deck's drag is U24 exactly: `DragGesture(minimumDistance: 0)` in the board space, attached with `highPriorityGesture`, a touch under 8pt is a tap, and a release in the hand band (the hand grown 64 up and 24 down) draws.
Nothing in a drag asks Messages for a presentation change.
OPEN: whether a downward drag starting mid-board can still collapse the drawer is only answerable on a phone; it has not been proven on a device.
Confidence: medium.

DECISION I10: the hand has no drag-to-reorder.
SUPERSEDED by ORCHESTRATION O9 and I38 below.
Alternative: keep FHandFan's reorder.
Why: the hand is in acquisition order and the kernel owns it (D24).
Confidence: high.

DECISION I11: while the hand scrolls (O4 past the 16pt strip), a card takes a tap only, and playing it is tap + Play; the drag-to-play is off there.
Alternative: a long press that lifts the card out of the scroll view.
Why: a drag with no minimum distance inside a horizontal scroll view takes the pan the scroll needs; the long press is a new gesture the study does not draw.
Confidence: medium.

DECISION I12: in the drawer (collapse of one half and over, `pk_lay_max_rows`) the hand is flat down to foolish's 22pt floor, then overlapped with 40pt faces down to a 16pt strip, then it scrolls (U7).
Alternative: overlap as soon as a flat card would go thin (under 40pt).
Why: it matches "Collapsed 02, fourteen at a 24pt strip" and keeps foolish's floor as the one flat limit.
Confidence: medium.

## Surfaces

DECISION I13: textures are baked by `shared/tools/textures/regenerate_textures.sh` and the five this product uses (felt and wood in both schemes, the fern back) are committed in `PickemupKit/Resources`; nothing renders a texture at run time.
Alternative: bake at run time from the shared palettes.
Why: foolish's round-6 lesson (a procedural render took an extension down on a phone); the bake is byte-identical to foolish's.
Confidence: high.

DECISION I14: the left of the pill row holds one square, the rulebook, where foolish has two; the Last card! pill takes its place (U11).
Alternative: foolish's gear and book, with an empty settings sheet.
Why: this product has nothing to set yet.
Confidence: medium.

DECISION I15: the suit, Skip, Reverse and wild glyphs are SwiftUI paths drawn from UI.html's own 100-unit SVG coordinates.
Alternative: C polygon lists, as uttt draws its board (REUSE_AUDIT.md 4).
Why: two of the four suits are curves (a circle, a rounded square), which a polygon list would approximate; the coordinates are the study's, in one file, and a later lift can move them.
Confidence: medium.

DECISION I16: SUPERSEDED by ORCHESTRATION O6 and I27 below.
It drew the Skip and Reverse faces as the study draws them, the action glyph in the suit's colour with no suit shape, which left their suit readable by colour alone.

DECISION I17: the Caught you! and Wrong call stamps show for the newest bubble's verdict only (`pk_api_since` of the last bubble).
Alternative: "until that seat's next move", which the kernel does not report.
Confidence: medium.

DECISION I18: the end reveal draws every hand face up on the ring (the reveal rows) and a wood plank on the pile with the headline, the kernel's results order and Again (a new lobby in the same chat shape).
Confidence: medium.

DECISION I19: the suit picker's tile positions (96 across, 104 up and down from the pile's centre) are Swift numbers in `SuitPicker.swift`.
Alternative: a `pk_lay_picker` entry point.
Why: they are the one layout number the study leaves unstated; they should move to `pk_lay.c` with the flight layer that pops the tiles out, which will need them in C anyway.
Confidence: low.
Done by the motion worker: the tiles now stand where `pk_lay_picker` puts them (`PkLayout.pickerTile`), and the tile and x sizes are `PK_LAY_PICKER_TILE` and `PK_LAY_PICKER_X` (ANIMATION_DECISIONS A9).

DECISION I20: no auto-collapse ride on render-server layers (uttt's `CollapseSlide`): the SwiftUI board relays out as the drawer moves, and a staged play asks for compact after foolish's 250 + 500 ms rest.
Alternative: port `CollapseSlide` with a set of collapse numbers in the kernel.
Why: this kernel has no collapse curve yet, and the flight worker owns motion; the `collapse` anchors are in place.
Confidence: medium.

DECISION I21: the shared Send reminder (`SendHint`) is not compiled in yet; the status corner's HEAD_STAGED line says the bubble is staged.
Alternative: compile it with a new caption key.
Why: it is motion and chrome the next worker places together with the flights.
Confidence: medium.

DECISION I22: a layout mismatch between the library and the generated readers (`Pk.layoutMatches`) shows the unreadable screen with the newer-version line, and reads nothing.
Confidence: high.

## Build and identity

DECISION I23: bundle ids `cards.pickemup` (the container), `cards.pickemup.msg` (the extension), `cards.pickemup.kit`, `cards.pickemup.kit.tests`, and the Debug-only App Group `group.cards.pickemup` through a committed `DebugAppGroup.entitlements` set per config, which xcodegen does not know about (uttt's trick).
The `.xcodeproj` is generated and git-ignored, as uttt's is.
Confidence: high.

DECISION I24: the nickname and the seat records live in the extension's own `UserDefaults.standard` (raw bytes and a string), not the App Group.
Alternative: foolish's App Group nickname.
Why: Release has no App Group (I23) and the extension is the only reader.
Confidence: high.

DECISION I25: no snapshot goldens.
Alternative: foolish's `ComponentSnapshotTests` on swift-snapshot-testing.
Why: that pattern adds a package dependency and git-ignored references recorded on one Mac; a render test pins the bubble's 300 x 195 instead, and snapshot references are a follow-up once the flight layer settles what a frame at rest is.
Confidence: medium.

DECISION I26: the icons are placeholders drawn by a throwaway CoreGraphics script (the wild's four suits on a cream card on the felt).
Why: no store metadata or art direction in this pass; LaunchServices will not register an app with no icon.
Confidence: high.

## Found and fixed on the way to the simulator

DECISION I27: O6 is drawn as the study's own corner column (`.cr`, a flex column with a 1pt gap): the index (the Skip or Reverse glyph, or +2) with the suit's shape at half the index size under it, top-left, and the same column turned half round at bottom-right.
A thin face (under 40pt) and an overlapped hand card show the shape under the centred index at the top, since that strip is all of the card a hand shows.
The card says itself through the kernel: its accessibility label is `W_CARD` ("skip on squares") and, for an action card, its value is the kernel's one-shape noun (`SUIT_ONE_n`, "square"), read from the same `CardFace.cornerSuit` that draws the corners.
Alternative: the shape only top-left, where a number card has its one index.
Why: O6 says two corners, and a card upside down on the pile or half hidden in a fan still shows one of them; a number card's shape is its big centre glyph, so it needs no corner mark.
Confidence: medium.

DECISION I28: the staged strip's chips are UI.html's `.strip .chip .cf`: the glyph alone at 76 x 64 percent, radius 2, no index and no pip (`PkCard(chip:)`).
Before this the chip was a thin face with a 7pt rank over a half-size glyph.
Confidence: high.

## The architecture review (2026-09-27)

An adversarial pass over `PickemupKit`, `PickemupMessages`, `PickemupMessagesApp` and `PickemupKitTests` against `docs/ARCHITECTURE_AS_A_PATTERN.md`, with no simulator (ORCHESTRATION B2, B3).
Each fix below has a C smoke check seen red (`pickemup/c/tests/MUTATIONS.md`) or a Swift test whose planned mutant is listed under "Not mutated" in `pickemup/ios/TESTS_MUTATED.md`.

DECISION I29: which events an adopted bubble plays is the kernel's (`pk_api_adopt`, `pk_api_beats_now`).
`PickemupHost.adopt` had held the prior table, its sealed link and my staged play across the read and chosen between a lost race, a further-on range, nothing and a cold open in Swift; that is a decision about the game's chains, so it moved into the call that adopts, and the host now plays whatever plan the kernel laid out.
Alternative: keep the Swift branch, since it only called kernel functions.
Why: the comparison is the rule (4.8), and doing it in the same call as the read means nothing on the host side holds the one resident slot across a decode.
Confidence: high.

DECISION I30: a tap on a fan is one kernel call (`pk_api_tap_fan`: called, uncalled, moved or refused).
The Swift version un-called and then called, so a refused new call silently lost the old one; the kernel tries a move on a copy and keeps the old call on a refusal.
Confidence: high.

DECISION I31: the board's zones are `pk_lay_zone` (the U24 draw band, the pile's drop target, the pill row, the toast's centre, the direction box), and the board inset, the hand padding, the tap slop and the pill height are kernel constants (`PK_LAY_INSET_*`, `PK_LAY_HAND_PAD`, `PK_LAY_TAP_SLOP`, `PK_LAY_PILL_H`).
What stays Swift is how a component draws itself inside the rect it is given: font sizes, paddings, radii, the pill's 96pt width, the chip sizes, the buried and under-card tilts, and the bubble picture's own composition.
The rule for a new number: a position on the board, a hit test or a threshold is C; the inside of a component is its view's.
Confidence: medium.

DECISION I32: the toast's 1600ms and the "drawn cards stay" line's 2400ms are `PK_T_TOAST` and `PK_T_DRAWN_STAY` beside the rest of the timeline, and a showing of that line carries a generation, so an older timer never hides a newer showing.
Confidence: high.

DECISION I33: no string literal a player sees is left in a view.
The corner index is `PK_API_W_INDEX` ("7", "+2", and the new `INDEX_PLUS4` "+4"), the strip's count is `PK_API_W_STRIP_DRAWS` (the new `STRIP_DRAWS`), the picker's x is an `xmark` symbol spoken as the new `BTN_CANCEL`, and the strip's middle dot is a drawn 3pt circle.
The rules page reads lines until the kernel answers -1, so it holds no count.
`uppercased()` on the direction word stays: it is the study's CSS `text-transform`, applied to the kernel's word.
Confidence: high.

DECISION I34: a mismatched library and readers (I22) is two vectors with two owners, written down in both places so they are not collapsed.
`Pk.snap` is the one path every generated read takes, and it reads nothing for a mismatched pair, which covers the model's first refresh before any screen is chosen; `PickemupHost.readable` is the one place that turns a mismatch into the unreadable screen, and `showResident`, `adopt` and the controller's `create` all ask it.
Before this, `viewDidLoad` set the unreadable screen and the next `present` adopted over it at a wrong offset.
Confidence: high.

DECISION I35: adopting a bubble that is not my staged draft voids any stage still resting before its insert (the settle sleep, the collapse wait, an insert retry), so an older bubble can never be put in the field after the resident moved on.
What is already in the field stays known (`staged`, `draftURL`), so its Send is adopted as sent (I5) and its X is still recognised.
Confidence: medium, until it is seen on a phone.

DECISION I36: the deck's drag is owned by one recognizer, the SwiftUI `DragGesture(minimumDistance: 0)` that `highPriorityGesture` puts on the deck, in this process; it begins on touch-down, so nothing else in the extension's view tree can take the touch, and nothing in it asks Messages for a presentation change.
The drawer's own swipe-down is Messages' recognizer in another process, which an extension can neither fail nor require to fail, so I9's open question stays open: only a phone can show that a downward drag off the deck never collapses the drawer.
Confidence: medium.

DECISION I37: what the review left in Swift, on purpose.
The collapse flag a touch stages with (a play and a pass collapse, a lone Last card! collapses, a draw or a call does not) is drawer policy read off the kernel's own draft events, and the stamp's display order (OUT, then the newest verdict, then LAST) orders kernel verdicts without deciding one; both are candidates for the next lift, not rules.
The 3 second readiness fallback and the silence and error beats are uttt's shared lifecycle (`InsertStaging`), kept whole.
Found clean: no JSON or `Codable` anywhere (the seat records are the kernel's fixed-layout bytes, the nickname a string); no byte layout outside `Generated/` (the only `withUnsafeBytes` calls pass the participant id, the nickname and the seat records to C as bytes); no force unwrap on a kernel return; and no path from another seat's card count to a view, an accessibility label or an overlay (the fan is `PK_FAN_BACKS`, the reveal rows exist only once the game is over, and `NoCountLeakTests` walks a three-seat table's accessibility tree for digits).
Confidence: high.

DECISION I38: the hand row keeps foolish's drag-to-reorder (O9), copied from `FHandFan.reorder` with a `COPIED from` header.
While the finger is in the hand row the dragged card asks for the slot whose centre is nearest its own centre (`pk_lay_hand_nearest`, FHandFan's `slotIndex`, ties to the lower slot) and goes there live under the card spring, with FHandFan's `reorderShift` pinning it to the finger; a release in the row is a rearrange and never a play, on the pile it plays (`pk_lay_drop`, FHandFan's `boardPoint` rule, the row tested first).
While the hand scrolls there is no rearranging, as there is no drag to play (I11): no clean way was found to share the horizontal pan with a drag of no minimum distance.
Alternative: a long press that lifts a card out of the scroll view to rearrange it there.
Why: the owner named hand ordering first among what to reuse from foolish; the long press is a gesture the study does not draw.
Confidence: medium.

DECISION I39: Swift names a hand card by its acquisition position, and the arranged slot is geometry only.
`HandRow` draws position i at `layout.slots[slotOf[i]]` and keeps `hand.i` as its anchor, so a flight lands on the right card whatever the arrangement; the selection, the picker's wild, the staged play and the undo's `PK_HM_UNDO` stay positions; the only maps between a position and a slot are the kernel's (`PkView.my_slot` one way, `pk_api_arranged_pos` the other).
A drag rearranges only while the board shows the settled hand: while a plan's frame shows a different hand, a position would name a different card, so the drag moves nothing.
Alternative: have the row and the model work in slots and map every touch through `pk_api_play_slot`.
Why: every event, anchor and wire position is an acquisition position, so working in slots would need a map at every one of them; `pk_api_play_slot` stays for a host that addresses the hand by slot and is pinned by the C tests.
Confidence: medium.

DECISION I40: a test that reads the accessibility tree of a hosted SwiftUI view first switches accessibility automation on (`PickemupKitTests/AXTree.swift`), and walks the tree through that one helper.
SwiftUI builds its accessibility elements only once an assistive client has asked, and a unit-test process is not one, so the first simulator run of `ActionCardCornerTests` and `NoCountLeakTests` found an empty tree and went red on nothing; the views were right.
`AXTree.enable` calls libAccessibility's `_AXSSetAutomationEnabled(1)` once per process, which is what VoiceOver and an XCUITest runner switch on before they read.
Alternative: move both checks into an XCUITest target, which runs with automation on; or test the label strings as functions and not the tree.
Why: the owner's rule is that no other seat's count reaches the tree, so the test must read the tree itself; a UI test target needs a host app this product does not have (the container is codeless), and a private symbol in a test bundle never ships.
If the symbol ever disappears, both tests fail on "is on the tree"; they can never pass on an empty walk.
Confidence: high.

DECISION I41: a Debug build reads a `dev.persona` file ("1 Bo") from the App Group, and an appex process that finds it sits down as another person: its participant id's last byte XORed with the number, its own seat records and nickname (`pickemup.seats.v1.p1`, `pickemup.nickname.p1`).
The simulator's Messages gives this extension ONE `localParticipantIdentifier` in every thread, so the rig's two-thread trick (a bubble sent in one stub thread arrives in the other) seated the same person twice and Bo's tap on Alex's invitation showed "1. Alex (You)".
It is read once per process (`static let`), and the rig ends the process between the two threads (`rig.sh leave`, `killappex`), so a flipped file never splits one process's identity.
Alternative: two simulators, one per person; there is no way to carry a bubble from one simulator's Messages to another's.
Why: it is the only way to play a two-seat game inside Messages on one host, and every reader of it is inside `#if DEBUG` beside the other dev files (`PickemupDev.swift`), so Release has none of it.
Confidence: high.

DECISION I42: an anchor a flight aims at is laid out where it is (`.position`, a frame), never moved there with `.offset`.
The pile's anchor was a clear 82 x 115 frame in an overlay, `.offset` to the pile's centre; an offset is a render transform, the anchor's GeometryReader measured the un-offset frame at the board's origin, and on the simulator every play, start card, bury and reshuffle gather flew to the top-left corner and snapped onto the pile when its ghost ended (`shots/motion/deal_bury_before_fix_sheet.png`).
`BeatPlayerTests` could not see it: they hand the player synthetic anchors. `AnchorTests` hosts a real `TableScreen` and checks the pile's anchor is the pile, the deck is beside it on its line, and every anchor is on the board.
Alternative: measure the pile's anchor on `PileView` itself; its frame carries the halo and the stack's lean, not one card.
Confidence: high.
