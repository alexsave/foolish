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

DECISION I16: the Skip and Reverse faces are drawn as the study draws them: the action glyph in the suit's colour, with no suit shape.
FOUND for the owner: that makes those two cards' suit readable by colour alone, which the shape-and-colour rule was meant to avoid; the study should decide whether they carry the suit shape too.
Confidence: low.

DECISION I17: the Caught you! and Wrong call stamps show for the newest bubble's verdict only (`pk_api_since` of the last bubble).
Alternative: "until that seat's next move", which the kernel does not report.
Confidence: medium.

DECISION I18: the end reveal draws every hand face up on the ring (the reveal rows) and a wood plank on the pile with the headline, the kernel's results order and Again (a new lobby in the same chat shape).
Confidence: medium.

DECISION I19: the suit picker's tile positions (96 across, 104 up and down from the pile's centre) are Swift numbers in `SuitPicker.swift`.
Alternative: a `pk_lay_picker` entry point.
Why: they are the one layout number the study leaves unstated; they should move to `pk_lay.c` with the flight layer that pops the tiles out, which will need them in C anyway.
Confidence: low.

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
