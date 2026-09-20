import Combine
import SwiftUI

/// What the screen is allowed to know. Every answer comes from the kernel;
/// this only remembers which of them is on screen and how far a stroke has
/// been drawn.
@MainActor
public final class UtttModel: ObservableObject {
    @Published public private(set) var positionKey = 0
    @Published public private(set) var animating: (move: Int, t: Double)?
    @Published public private(set) var busy = false

    public private(set) var seed: Int32
    public private(set) var you: Uttt.Mark

    /// NOBODY PLAYS A BOT HERE. The bots exist - `random` through `nib`, in
    /// `uttt/c/src/uttt_bots.c` - and they were written to answer one
    /// question, how long a real game's code is, which needed thousands of
    /// plausible games and no human. They are research. This game is a thing
    /// two people do to each other in a thread, and an opponent that is
    /// always available and never loses interest is the opposite of that.
    ///
    /// So there is no `solo`, no reply, and no name to put on the other side
    /// of the bar: it is whoever has the other seat, drawn as their mark.
    public init(seed: Int32, you: Uttt.Mark = .x) {
        self.seed = seed; self.you = you
        Uttt.newGame(seed: seed)
    }

    public var active: Int {
        guard Uttt.over == .none else { return -1 }
        let f = Uttt.forced
        return (f != 255 && Uttt.block(f) == .none) ? f : 9
    }
    public var last: Int { Uttt.plyCount > 0 ? Uttt.move(at: Uttt.plyCount - 1) : -1 }

    /// WHAT THE BAR SAYS, and the other side is NAMED BY ITS MARK - drawn,
    /// not spelled. "Waiting on O" puts a letter of the alphabet where the
    /// rest of the screen has a pen stroke, and the same screen is already
    /// showing "you are" over a drawn mark two inches away. A mark is also
    /// the only name this side actually has: a Messages extension gets a
    /// per-device UUID and no display name.
    public enum Headline: Equatable {
        case text(String)
        /// Words, a drawn mark, words: "Waiting on <O>", "<X> wins".
        case mark(String, Uttt.Mark, String)
    }

    public var headline: Headline {
        switch Uttt.over {
        case .draw:  return .text("Drawn")
        case .x, .o:
            if Uttt.over == you { return .text("You win") }
            return .mark("", Uttt.over, " wins")
        case .none:
            if Uttt.turn == you { return .text("Your move") }
            return .mark("Waiting on ", Uttt.turn, "")
        }
    }

    /// The line under it. When it is not your turn this is WHERE YOU SENT
    /// THEM, which is the one thing worth reading on a board you cannot touch,
    /// and it comes from the kernel because the names of the nine blocks are
    /// not the screen's to invent.
    public var subline: String {
        if Uttt.over == .draw { return "Nine blocks, no line." }
        if Uttt.over != .none { return "\(Uttt.plyCount) moves." }
        if Uttt.turn == you { return active == 9 ? "Anywhere you like." : "" }
        if active == 9 { return "Anywhere they like." }
        let p = UtttBubble.placeName(spoken: false)
        return p.isEmpty ? "" : p.prefix(1).uppercased() + p.dropFirst() + "."
    }

    /// The harness loads a position behind the model's back; this is how it
    /// tells the screen to look again.
    public func refresh() { positionKey &+= 1 }

    /// A MOVE THAT IS STAGED IS STILL A DRAFT. It sits in the input field
    /// with an X on it until a human taps the arrow, so until then the player
    /// is allowed to change their mind - and the way they say so is by
    /// tapping a different square, not by hunting for Apple's little X.
    ///
    /// True while this device's last move is staged and unsent.
    @Published public private(set) var pending = false

    /// A tap in the board's own 0..1 space.
    public func tap(at p: CGPoint) {
        guard !busy, Uttt.over == .none else { return }
        guard Uttt.turn == you || pending else { return }
        let bx = min(2, Int(p.x * 3)), by = min(2, Int(p.y * 3))
        let cx = min(2, Int((p.x * 3 - CGFloat(bx)) * 3))
        let cy = min(2, Int((p.y * 3 - CGFloat(by)) * 3))
        let mv = (by * 3 + bx) * 9 + (cy * 3 + cx)

        if pending {
            /* THE SAME SQUARE IS NOT A CHANGE OF MIND, and re-staging the
             * identical bubble would make Messages cancel and re-insert for
             * nothing. */
            guard mv != Uttt.move(at: Uttt.plyCount - 1) else { return }
            Task { await replace(with: mv) }
            return
        }
        guard Uttt.legal.contains(UInt8(mv)) else { return }
        Task { await playerMove(mv) }
    }

    /// Take the staged move back and play another one.
    ///
    /// FOUR BEATS, IN THIS ORDER, because that is the order a hand does it
    /// in: the wash comes back to the block you were sent to, the ink leaves
    /// the square you regret, the new mark draws, and only then does the wash
    /// go where the new move sends them. Doing the undo and the redraw as one
    /// swap reads as a glitch; doing it in four reads as somebody rubbing
    /// something out.
    private func replace(with mv: Int) async {
        busy = true
        Uttt.undo()
        positionKey &+= 1                 // the wash is back where it was
        try? await Task.sleep(nanoseconds: 170_000_000)
        guard Uttt.legal.contains(UInt8(mv)) else {
            // Not a legal square in the position we just came back to. Put
            // the move we took back where it was and pretend nothing happened.
            Uttt.play(lastPlayed)
            positionKey &+= 1
            busy = false
            return
        }
        Uttt.play(mv)
        lastPlayed = mv
        await draw(mv)
        positionKey &+= 1
        busy = false
    }

    /// The move this device staged, so a rejected replacement can be put back.
    private var lastPlayed = -1

    /// The host says whether a draft is outstanding: it is the only thing that
    /// knows whether the bubble is still in the input field.
    public func setPending(_ on: Bool) { pending = on }

    private func playerMove(_ mv: Int) async {
        busy = true
        Uttt.play(mv)
        lastPlayed = mv
        await draw(mv)
        positionKey &+= 1
        busy = false
    }

    /// The same draw, stopped early - which is why there is no separate
    /// animation model anywhere in this target.
    private func draw(_ mv: Int) async {
        let frames = 26
        for f in 0...frames {
            animating = (mv, Double(f) / Double(frames))
            try? await Task.sleep(nanoseconds: 16_000_000)
        }
        animating = nil
    }
}
