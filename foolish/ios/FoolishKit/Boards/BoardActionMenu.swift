// BoardActionMenu - WHICH pills the board offers right now, as one value.
//
// Pulled out of `MessageTableView.actionBar` and `MessageTableView.undoSlot`,
// which is where every one of these enable states was decided inside a
// `some View` and could therefore only be checked by looking at a screen.
// Nothing here is a view.
//
// THE PLAY PILLS ARE THE KERNEL'S, WHOLE (legal.h play_pills): one call over
// the board, the selection and the host's own PLAY_HOST_* bits
// (`PlayWire.pills`), so the website draws the same pills from the same rule
// and no Swift restates who defends, whether the table or the selection is
// empty, whether the seat is out or whether it has a move. This file only
// turns the PLAY_PILL_* bits into the five Bools FActionBar draws. Take is the
// one pill that reads no answer, and why is written beside the rule in legal.h.

import Foundation

public struct BoardActionMenu: Equatable, Sendable {
    public let canAttack: Bool
    public let canCover: Bool
    public let canPass: Bool
    public let canPickup: Bool
    public let canDone: Bool

    /// Nothing offered. The read-only board a spectator is looking at, and the
    /// resting value for a board with no view yet.
    static let none = BoardActionMenu(pills: 0)

    /// The five play pills, from the kernel's PLAY_PILL_* bits.
    public init(pills: UInt32) {
        func has(_ bit: Int) -> Bool { pills & UInt32(bit) != 0 }
        canAttack = has(PLAY_PILL_ATTACK)
        canCover = has(PLAY_PILL_COVER)
        canPass = has(PLAY_PILL_PASS)
        canPickup = has(PLAY_PILL_PICKUP)
        canDone = has(PLAY_PILL_GOOD)
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
