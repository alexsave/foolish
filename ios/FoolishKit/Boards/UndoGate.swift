import Foundation

/// UNDO WAITS FOR THE BOARD TO BE STILL.
///
/// Owner, on build 72: "lets just disable the undo card until the animations
/// and the collapses are done. Cuz i dont want to get into a weird scenario
/// where we undo mid animation."
///
/// Moving is any of three things, each with its one existing answer:
///   - an animated sequence holds the board (`BoardAnimator.isSequencing`: a
///     placement flight, a sweep, a deal, an open replay);
///   - the collapse tween is running (`CollapseTween.isTweening`);
///   - Messages is still sliding the sheet (`CollapseTween.isPresenting`);
///   - the extension's auto-collapse is under way, INCLUDING its deliberate
///     rest before the collapse starts (`CollapseTween.isAutoCollapsing`).
///
/// The Undo PILL asks, and only the pill. The X on the staged bubble does not:
/// by the time the board hears of it Messages has already removed the bubble,
/// and refusing that undo would leave a staged move with nothing to send it in.
public enum UndoGate {
    /// Whether an Undo tap is taken. `waits` is the flag: off, every tap is.
    public static func accepts(waits: Bool, sequencing: Bool, tweening: Bool,
                               presenting: Bool, autoCollapsing: Bool = false,
                               cardsVeiled: Bool = false) -> Bool {
        !waits || !(sequencing || tweening || presenting || autoCollapsing || cardsVeiled)
    }

    /// NOT SHOWN, NEVER DIMMED. Owner: "instead of dimming, you just dont show
    /// any action button between when the attack animation starts playing and
    /// the autocollapse finishes" - and "basically I never want to see a
    /// dimmed undo button." The pill's slot keeps its fixed size, so nothing
    /// moves when it goes. Ships on; `undo.hides=0` in `dev.flags` puts back
    /// the dimmed pill.
    public static let hidesByDefault = true

    public static var hides: Bool {
        #if DEBUG || SOLO_TESTING
        return MessageDevBoard.flag("undo.hides", shipping: hidesByDefault)
        #else
        return hidesByDefault
        #endif
    }

    /// The live answer, off the live state.
    /// `cardsVeiled`: the board's own veil (`BoardAnimator.hidden`) is not
    /// empty. It is the one signal already true in the very turn a move is
    /// played - the card is hidden before the apply - while the sequence hold
    /// is taken a Task later and `stage` marks the auto-collapse later still.
    /// Without it Undo showed for a few frames between the tap and the flight.
    @MainActor public static func acceptsNow(cardsVeiled: Bool = false) -> Bool {
        accepts(waits: waits, sequencing: BoardAnimator.isSequencing,
                tweening: CollapseTween.isTweening, presenting: CollapseTween.isPresenting,
                autoCollapsing: CollapseTween.isAutoCollapsing, cardsVeiled: cardsVeiled)
    }

    /// Ships on. `undo.waits=0` in `dev.flags` puts back an Undo that can be
    /// pressed mid-animation.
    public static let waitsByDefault = true

    /// The shipping default in Release; in DEBUG, whatever `dev.flags` says.
    public static var waits: Bool {
        #if DEBUG || SOLO_TESTING
        return MessageDevBoard.flag("undo.waits", shipping: waitsByDefault)
        #else
        return waitsByDefault
        #endif
    }
}
