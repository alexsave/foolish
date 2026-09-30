// BoardActionMenu - WHICH pills the board offers right now, as one value.
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
// THE COMPOSITION IS THE KERNEL'S (legal.h play_pills), so the website draws
// the same pills from the same rule. This file only translates: `Gates` to the
// PLAY_GATE_* bits, the probe to the PLAY_ANSWER_* bits, and the PLAY_PILL_*
// bits back. Every play enable is the kernel's answer (§17.16); Take is the one
// pill that reads no answer, and why is written beside the rule in legal.h.

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
        var iCanAct: Bool
        /// A move is staged and waiting on Messages' Send. While it is, the only
        /// control the board offers is Undo: the extension has already dropped
        /// the human at the compose bar.
        var canSend: Bool
        /// A move of mine is between the tap and the kernel's answer.
        var playInFlight: Bool
        /// The board is at rest enough to accept a play (`ActionPillSlot`/
        /// `UndoGate`). Computed by the caller because it reads statics nothing
        /// publishes, which is also why the pills are redrawn on a short timer.
        var boardStill: Bool
        /// Round 20: the kernel stood this seat down (a newer chain arrived).
        var superseded: Bool
        /// Round 16: the 15-second hold that gives attackers a fair chance to
        /// throw in before the defender may take. Non-zero means held.
        var pickupHeld: Bool
        var isDefender: Bool
        var isOut: Bool
        var tableIsEmpty: Bool
        var selectionIsEmpty: Bool

        public init(iCanAct: Bool, canSend: Bool, playInFlight: Bool, boardStill: Bool,
                    superseded: Bool, pickupHeld: Bool, isDefender: Bool, isOut: Bool,
                    tableIsEmpty: Bool, selectionIsEmpty: Bool) {
            self.iCanAct = iCanAct; self.canSend = canSend
            self.playInFlight = playInFlight; self.boardStill = boardStill
            self.superseded = superseded; self.pickupHeld = pickupHeld
            self.isDefender = isDefender; self.isOut = isOut
            self.tableIsEmpty = tableIsEmpty; self.selectionIsEmpty = selectionIsEmpty
        }

        /// These facts as the kernel's PLAY_GATE_* bits.
        var bits: UInt32 {
            let facts: [(Bool, Int)] = [
                (iCanAct, PLAY_GATE_I_CAN_ACT), (canSend, PLAY_GATE_CAN_SEND),
                (playInFlight, PLAY_GATE_PLAY_IN_FLIGHT), (boardStill, PLAY_GATE_BOARD_STILL),
                (superseded, PLAY_GATE_SUPERSEDED), (pickupHeld, PLAY_GATE_PICKUP_HELD),
                (isDefender, PLAY_GATE_IS_DEFENDER), (isOut, PLAY_GATE_IS_OUT),
                (tableIsEmpty, PLAY_GATE_TABLE_EMPTY), (selectionIsEmpty, PLAY_GATE_SELECTION_EMPTY),
            ]
            return facts.reduce(0) { $1.0 ? $0 | UInt32($1.1) : $0 }
        }
    }

    /// Nothing offered. The read-only board a spectator is looking at, and the
    /// resting value for a board with no view yet.
    static let none = BoardActionMenu(canAttack: false, canCover: false, canPass: false,
                                             canPickup: false, canDone: false)

    /// The five play pills, from one kernel probe and the board's own gates,
    /// composed by `play_pills` (legal.h), where the rule and its history live.
    public static func resolve(_ probe: PlayProbe, _ g: Gates) -> BoardActionMenu {
        let pills = PlayWire.pills(answers: probe.answers, gates: g.bits)
        func has(_ bit: Int) -> Bool { pills & UInt32(bit) != 0 }
        return BoardActionMenu(canAttack: has(PLAY_PILL_ATTACK),
                               canCover: has(PLAY_PILL_COVER),
                               canPass: has(PLAY_PILL_PASS),
                               canPickup: has(PLAY_PILL_PICKUP),
                               canDone: has(PLAY_PILL_GOOD))
    }

    /// THE UNDO PILL, which is its own answer because it is drawn in its own
    /// always-present slot (round 10g) and not in FActionBar's column.
    ///
    /// Three states, not two, because `UndoGate.hides` chooses between two
    /// presentations of "the board is moving": hidden, or dimmed. Written as an
    /// enum so the third state cannot be reached by accident - the rule the
    /// owner asked for is "shown enabled, or not shown - never dimmed", and
    /// with a Bool pair that is a convention rather than a shape.
    enum UndoPill: Equatable, Sendable {
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
    static func undoPill(canSend: Bool, retracting: Bool,
                                still: Bool, hides: Bool) -> UndoPill {
        guard canSend else { return .absent }
        let usable = !retracting && still
        if hides { return usable ? .enabled : .absent }
        return usable ? .enabled : .dimmed
    }
}
