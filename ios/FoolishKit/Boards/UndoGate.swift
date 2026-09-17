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
///   - Messages is still sliding the sheet (`CollapseTween.isPresenting`).
///
/// The Undo PILL asks, and only the pill. The X on the staged bubble does not:
/// by the time the board hears of it Messages has already removed the bubble,
/// and refusing that undo would leave a staged move with nothing to send it in.
public enum UndoGate {
    /// Whether an Undo tap is taken. `waits` is the flag: off, every tap is.
    public static func accepts(waits: Bool, sequencing: Bool, tweening: Bool,
                               presenting: Bool) -> Bool {
        !waits || !(sequencing || tweening || presenting)
    }

    /// The live answer, off the live state.
    @MainActor public static var acceptsNow: Bool {
        accepts(waits: waits, sequencing: BoardAnimator.isSequencing,
                tweening: CollapseTween.isTweening, presenting: CollapseTween.isPresenting)
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
