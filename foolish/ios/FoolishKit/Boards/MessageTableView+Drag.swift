// DRAGGING A CARD - what a release would do, and the word that says so.
//
// The verb hint and the pass ghost-slot both resolve the SAME `BoardDrop.target`
// + kernel probe `onDragEnded` will use, so neither can disagree with what
// actually happens on release. The pill rides the dragged CARD's own live
// centre, not the fingertip: a fingertip is only where the card is if you
// happened to grab it dead centre.

import SwiftUI
import Foundation

extension MessageTableView {

    /// How far above the dragged CARD'S CENTRE the verb hint floats (round-6
    /// bug 5 re-anchored it from the fingertip to the card; the number itself is
    /// unchanged). A hand card is 72pt tall, so 52 leaves the pill ~16pt clear
    /// of the card's top edge: reads as attached to the card, still well out
    /// from under the thumb holding it.
    static let dragHintLift: CGFloat = 52
    /// The highest the pill may sit in the board's own coordinates. Dragging a
    /// card up to the deck/discard corners would otherwise push it off the top
    /// edge (a 52pt lift off a card whose centre is 40pt down is -12), and a
    /// verb you cannot read is the same bug in a different direction. Half the
    /// pill's own height (13pt text + 2x FSpace.xs) plus a couple of points of
    /// margin, since `.position` places its CENTRE. When the pill cannot fit
    /// above the card it goes BELOW it rather than parking on this line - see
    /// `dragHintPosition`.
    static let dragHintMinY: CGFloat = 16

    /// Where the verb-hint pill sits, in the board GeometryReader's own local
    /// coordinates (round-6 bug 5). Pure + static so the anchoring can be
    /// asserted directly instead of eyeballed mid-drag on a device.
    ///
    /// `cardCentre` is the dragged card's live visual centre in `boardSpace`
    /// (FHandFan.onDragCardMoved). `restingCentre` is that card's hand slot,
    /// used for the ONE frame at the start of a drag where the board has a drag
    /// point but no reported card centre yet - which is the frame the card has
    /// not moved in anyway, so the slot IS the card. `finger` is the last
    /// resort, and only reachable if the dragged card has no published frame at
    /// all. A multi-card selection anchors on the card actually being dragged:
    /// the rest of the selection has not moved out of the fan, so that is the
    /// only card the pill could sensibly point at.
    ///
    /// Drag a card up against the board's top edge (the deck / discard corners)
    /// and there is no room for the pill above it. Clamping it to the ceiling
    /// then parks it ON TOP OF the card, hiding the very card it describes
    /// (screenshotted on the simulator mid-fix), so it FLIPS below the card
    /// instead: still touching the card, still fully on screen, and never
    /// covering it, since the flip distance is the same lift that already
    /// clears half a card.
    static func dragHintPosition(cardCentre: CGPoint?, restingCentre: CGPoint?,
                                 finger: CGPoint, origin: CGPoint) -> CGPoint {
        let anchor = cardCentre ?? restingCentre ?? finger
        let y = anchor.y - origin.y
        return CGPoint(x: anchor.x - origin.x,
                       y: y - dragHintLift >= dragHintMinY ? y - dragHintLift : y + dragHintLift)
    }

    /// Round-7 #3 ("rearranging while in the compact view keeps giving 'move not
    /// allowed'"): the hand's DROP/cancel hit-region, grown generously upward (and
    /// a little down) from the published hand frame. In the compact drawer the fan
    /// is cropped to a ~44pt strip at the very bottom, so a horizontal rearrange
    /// whose finger drifts up off that thin strip fell OUTSIDE `handFrame` - and a
    /// release there resolves to `.table`, i.e. an attack/pass, which the kernel
    /// rejects with the "move not allowed" toast. Battles are hit-tested FIRST in
    /// `BoardDrop.target`, so widening the hand band never swallows a real cover;
    /// and a genuine open-table attack is dropped well above this band (the centre
    /// of the board), so it still resolves to `.table`. A rearrange that never
    /// reached a battle now cancels quietly instead of rejecting.
    private var handDropFrame: CGRect {
        guard handFrame != .zero else { return handFrame }
        let up: CGFloat = 64, down: CGFloat = 24
        return CGRect(x: handFrame.minX, y: handFrame.minY - up,
                      width: handFrame.width, height: handFrame.height + up + down)
    }

    /// The move a release right now would resolve to, if any — the SAME
    /// `BoardDrop.target` + kernel probe math `onDragEnded` uses, shared by the
    /// verb hint (note 33) and the pass ghost-slot preview (note 34) so neither
    /// can disagree with what actually happens on release.
    private func dragPreview(_ view: GameView) -> (target: PlayTarget, move: Move)? {
        guard let card = dragCard, let point = dragPoint else { return nil }
        let target = BoardDrop.target(at: point, battles: battleFrames, handFrame: handDropFrame)
        guard target != .hand,
              let move = probe(view, playCards(for: card, view), target).move
        else { return nil }
        return (target, move)
    }

    /// note 34: is the live drag currently previewing a PASS onto open table
    /// space? Gates both the ghost slot and the suppression of the ordinary
    /// per-battle cover highlight while it's showing.
    private func isPassPreview(_ view: GameView) -> Bool {
        guard let preview = dragPreview(view) else { return false }
        return preview.target == .table && preview.move.type == .pass
    }

    /// Whether the table shows the pass preview's empty slot - the kernel's
    /// answer (`anim_pass_slot_shown`), which keeps it through a crossing and a
    /// release. See `PassSlot`.
    func passSlotShown(_ view: GameView) -> Bool {
        PassSlotWire.shown(previewing: isPassPreview(view), dragging: dragCard != nil,
                           seenThisDrag: passSeenThisDrag, overDeadPair: isOverDeadPair(view),
                           heldAt: passHeldAt, battles: view.battles.count,
                           hold: PassSlot.hold, sticky: PassSlot.sticky)
    }

    /// The finger is over a pair the dragged card can make no legal move onto.
    private func isOverDeadPair(_ view: GameView) -> Bool {
        guard let card = dragCard, let point = dragPoint else { return false }
        let target = BoardDrop.target(at: point, battles: battleFrames, handFrame: handDropFrame)
        guard case .battle = target else { return false }
        return probe(view, playCards(for: card, view), target).move == nil
    }

    /// note 33: what a release would do, localized — "Attack" / "Cover" /
    /// "Pass". Nothing over the hand (that's a reorder, not a play) or for a
    /// drop the kernel can't resolve into a legal move.
    private func dragHintText(_ view: GameView) -> String? {
        guard let move = dragPreview(view)?.move else { return nil }
        switch move.type {
        case .attack: return FStrings.t("attack")
        case .cover: return FStrings.t("cover")
        case .pass: return FStrings.t("pass")
        default: return nil
        }
    }

    /// note 33: the word for what releasing would do - web DragShadow parity.
    /// Round-4 note 4: it tracks the fingertip (see boardContent), which is
    /// what "anchored to a fixed spot above the battles" was traded against and
    /// lost - the fixed anchor was easy to place but sat at the top of the
    /// screen while your hand was at the bottom, so it read as unrelated to the
    /// drag.
    ///
    /// ROUND 46 - NO PILL, JUST THE WORD (owner: "the text is fine to keep but
    /// scrap the bubble holding it"). It used to be `FColor.card` on an 85%
    /// `FColor.ink` capsule with its own shadow: a second opaque object riding
    /// half a card above the card you are already dragging, on a board whose
    /// whole vocabulary is cards and wood. Two floating rectangles instead of
    /// one.
    ///
    /// THE WORD ITSELF IS UNCHANGED - same string, same 13pt semibold, same
    /// `FColor.card` ink. Only the plate under it goes. The one thing kept from
    /// it is a shadow, moved from the pill onto the text, because the pill was
    /// doing a real job: holding one small word legible over felt, over wool,
    /// and over the face of whatever card it passes. A shadow is not a bubble.
    ///
    /// AND IT DOES NOT MOVE. `dragHint` is placed by `.position(x:y:)`, which
    /// centres a view on the given point, and the pill's padding was symmetric
    /// - so the text's centre always WAS the pill's centre. Dropping the
    /// padding changes the view's size and not its centre, so the word stays on
    /// exactly the point `dragHintPosition` returns (owner: "the positioning
    /// should stay the same without the pill").
    @ViewBuilder
    func dragHint(_ view: GameView) -> some View {
        if let text = dragHintText(view) {
            Text(text)
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(FColor.card)
                .shadow(color: .black.opacity(0.55), radius: 2, y: 1)
                .transition(.opacity)
                .allowsHitTesting(false)
        }
    }

    /// notes 33/34: FHandFan already delivers the live boardSpace point on
    /// every change — kept now (previously discarded at the call site) so the
    /// verb hint / ghost-slot preview can resolve the same drop target live.
    func onDragChanged(_ card: Card, at point: CGPoint) {
        if dragCard == nil { passSeenThisDrag = false; passHeldAt = nil }
        dragCard = card
        dragPoint = point
        if !passSeenThisDrag, let view = controller.view, isPassPreview(view) { passSeenThisDrag = true }
    }

    func onDragEnded(_ card: Card, at point: CGPoint, _ view: GameView) {
        // Round-6 bug 13: WHERE the card was when the finger let go, captured
        // BEFORE the drag state is torn down two lines down. That teardown used
        // to happen first and `playAt` ran with no memory of the drag at all, so
        // the card returned to its hand slot and the play animated out of the
        // HAND - "it should animate from where we let go to the table, not back
        // from its original position in hand". The card's own centre, not the
        // fingertip, for the same reason the verb hint uses it (bug 5): the
        // finger is wherever you grabbed the card, which is not where the card
        // is. Falls back to the finger only if no card centre was ever reported.
        let releaseCentre = dragCardCenter ?? point
        // Read while the drag still exists: a pass let go of holds its slot.
        let passing = isPassPreview(view)
        dragCard = nil
        dragPoint = nil
        dragCardCenter = nil
        // Round-7 #3: `handDropFrame`, not the raw published `handFrame` - a
        // compact-drawer rearrange that drifts off the thin cropped strip still
        // reads as a reorder/cancel, not a rejected attack. See `handDropFrame`.
        let target = BoardDrop.target(at: point, battles: battleFrames, handFrame: handDropFrame)
        // Dropped back in the fan — cancel. Nothing to fly: FHandFan has already
        // sprung the card home to its slot, which is the right animation for a
        // drag that played nothing.
        if target == .hand { return }
        if passing, target == .table { passHeldAt = view.battles.count }
        playAt(target, playCards(for: card, view), view, released: (card, releaseCentre))
    }
}
