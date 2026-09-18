// BoardActionMenu — WHICH pills the board offers right now, as one value.
//
// Pulled out of `MessageTableView.actionBar` and `MessageTableView.undoSlot`,
// which is where every one of these enable states was decided inside a
// `some View` and could therefore only be checked by looking at a screen.
// Nothing here is a view: it takes ONE kernel answer about the current
// selection (`PlayProbe`, i.e. `fio_play_probe`) plus the handful of facts the
// board knows about itself that the kernel cannot - a move already staged, a
// flight still in the air, a retraction being peeked at - and returns what to
// draw. A test can enumerate it; a board cannot disagree with it.
//
// EVERY PLAY ENABLE IS THE KERNEL'S ANSWER (§17.16). `canAttack`, `canCover`,
// `canPass` and `canDone` are `probe.*` AND'ed with the board's own gates, and
// there is no rule of this file's own in any of them. `canPickup` is the ONE
// exception in the whole board, and it is now stated here where a reader looks
// for it rather than buried in the middle of a view body - see its doc.

import Foundation

public struct BoardActionMenu: Equatable, Sendable {
    public let canAttack: Bool
    public let canCover: Bool
    public let canPass: Bool
    public let canPickup: Bool
    public let canDone: Bool

    /// What the board knows about ITSELF - the gates that are not the kernel's
    /// to answer, because they are about this screen rather than about Durak.
    public struct Gates: Equatable, Sendable {
        /// The kernel published a menu for my seat (`controller.iCanAct`).
        public var iCanAct: Bool
        /// A move is staged and waiting on Messages' Send. While it is, the only
        /// control the board offers is Undo: the extension has already dropped
        /// the human at the compose bar.
        public var canSend: Bool
        /// A move of mine is between the tap and the kernel's answer.
        public var playInFlight: Bool
        /// The board is at rest enough to accept a play (`ActionPillSlot`/
        /// `UndoGate`). Computed by the caller because it reads statics nothing
        /// publishes, which is also why the pills are redrawn on a short timer.
        public var boardStill: Bool
        /// Round 20: the kernel stood this seat down (a newer chain arrived).
        public var superseded: Bool
        /// Round 16: the 15-second hold that gives attackers a fair chance to
        /// throw in before the defender may take. Non-zero means held.
        public var pickupHeld: Bool
        public var isDefender: Bool
        public var isOut: Bool
        public var tableIsEmpty: Bool
        public var selectionIsEmpty: Bool

        public init(iCanAct: Bool, canSend: Bool, playInFlight: Bool, boardStill: Bool,
                    superseded: Bool, pickupHeld: Bool, isDefender: Bool, isOut: Bool,
                    tableIsEmpty: Bool, selectionIsEmpty: Bool) {
            self.iCanAct = iCanAct; self.canSend = canSend
            self.playInFlight = playInFlight; self.boardStill = boardStill
            self.superseded = superseded; self.pickupHeld = pickupHeld
            self.isDefender = isDefender; self.isOut = isOut
            self.tableIsEmpty = tableIsEmpty; self.selectionIsEmpty = selectionIsEmpty
        }
    }

    /// Nothing offered. The read-only board a spectator is looking at, and the
    /// resting value for a board with no view yet.
    public static let none = BoardActionMenu(canAttack: false, canCover: false, canPass: false,
                                             canPickup: false, canDone: false)

    /// The five play pills, from one kernel probe and the board's own gates.
    ///
    /// `acting` is the gate every play button shares: the kernel offered me a
    /// menu, I have not already staged, my last tap has landed and the board is
    /// still. Whatever that gate says, the kernel still has the final word on
    /// each individual pill - so a board that wrongly believed itself to be
    /// acting could at worst offer a move the kernel had already listed.
    public static func resolve(_ probe: PlayProbe, _ g: Gates) -> BoardActionMenu {
        let acting = g.iCanAct && !g.canSend && !g.playInFlight && g.boardStill
        return BoardActionMenu(
            canAttack: acting && !g.isDefender && probe.canAttack,
            canCover: acting && g.isDefender && probe.canCover,
            canPass: acting && g.isDefender && probe.canPass,
            canPickup: pickup(g),
            // Selection-aware, like Take: with cards selected, Good must
            // disappear - a stray tap on it mid-selection would abandon the
            // cards you had picked (web parity TODO).
            canDone: acting && probe.canSayGood && g.selectionIsEmpty)
    }

    /// TAKE — THE ONE PILL ON THIS BOARD THAT IS NOT THE KERNEL'S LEGAL MENU,
    /// and the reason is worth reading before anyone "fixes" it.
    ///
    /// The condition is the web's own (`rawPickup = isDefending &&
    /// table_battles > 0`) and NOT `probe`'s, because the kernel stops LISTING
    /// pickup once every attack on the table is covered - while still ACCEPTING
    /// the move. Reading the menu here would therefore take Take away from a
    /// defender who is allowed to take, which is a rule this screen would be
    /// getting wrong in the strict direction. It is a duplicated rule either
    /// way; this is the duplication that plays correctly. The honest fix is a
    /// kernel answer for "may this seat pick up", and until there is one the
    /// exception lives here, named, with a test on it.
    ///
    /// Everything AROUND it is still this board's own business:
    ///
    ///  - `selectionIsEmpty` so a stray tap cannot abandon a picked selection;
    ///  - `!canSend`, added in round 7 on the owner's read on device. This
    ///    REVERSES the earlier "Take survives the staged/all-covered state":
    ///    leaving Take up while Undo appeared BELOW it shoved the
    ///    bottom-anchored column upward, so the Take pill visibly rode up as
    ///    Undo popped in (the "ghostly Pickup floating above Undo"). The owner
    ///    chose the clean swap - to take your own covered table now, Undo
    ///    first, then Take. The kernel still accepts the move, so no reject;
    ///  - `!pickupHeld`, round 16 (owner: "you cannot pickup within 15 seconds
    ///    of the attack ... this is to give attackers a fair chance to throw in
    ///    additional cards"). While the hold stands the pill is simply not there
    ///    - no greyed-out button, no countdown, nothing to press - and it
    ///    appears on its own when the hold lapses. The same number refuses the
    ///    move in `MessageTurnController.apply`, so this is the polite half of
    ///    the rule, not the rule;
    ///  - `!superseded` EXPLICITLY, round 20, precisely because this pill does
    ///    not read the menu: standing `iCanAct` down does not reach it, so a
    ///    read-only board would otherwise keep offering Take.
    private static func pickup(_ g: Gates) -> Bool {
        g.isDefender && !g.tableIsEmpty && g.selectionIsEmpty && !g.isOut
            && !g.canSend && !g.pickupHeld && !g.superseded
            && !g.playInFlight && g.boardStill
    }

    /// THE UNDO PILL, which is its own answer because it is drawn in its own
    /// always-present slot (round 10g) and not in FActionBar's column.
    ///
    /// Three states, not two, because `UndoGate.hides` chooses between two
    /// presentations of "the board is moving": hidden, or dimmed. Written as an
    /// enum so the third state cannot be reached by accident - the rule the
    /// owner asked for is "shown enabled, or not shown - never dimmed", and
    /// with a Bool pair that is a convention rather than a shape.
    public enum UndoPill: Equatable, Sendable {
        /// No pill at all: nothing is staged, or the board is moving and
        /// `UndoGate.hides` says a moving board hides rather than dims.
        case absent
        case enabled
        /// Drawn, greyed. Only ever reachable with `hides` false.
        case dimmed
    }

    /// - `canSend`: something is staged, so there is something to take back.
    /// - `retracting`: a conflict retraction is already in flight (audit U8).
    ///   Every other door into the controller asks first, and this one did not,
    ///   so a tap during the conflict peek ran `undo`, found nothing to take
    ///   back, and RE-STAGED the very chain being retracted. Disabled rather
    ///   than guarded inside the action, on the owner's call: "let's disable the
    ///   undo button during that then." A control that cannot be pressed has no
    ///   door to forget.
    /// - `still`: the board is not animating and my play is not mid-stage.
    /// - `hides`: `UndoGate.hides` - see the enum.
    public static func undoPill(canSend: Bool, retracting: Bool,
                                still: Bool, hides: Bool) -> UndoPill {
        guard canSend else { return .absent }
        let usable = !retracting && still
        if hides { return usable ? .enabled : .absent }
        return usable ? .enabled : .dimmed
    }
}
