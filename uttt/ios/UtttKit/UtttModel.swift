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
    private var solo: Bool

    /// WHO IS ON THE OTHER SIDE, and it is not always a bot. The screens said
    /// "nib" whenever the other side was moving, which is the name of the
    /// demo opponent - so in a real thread every player read the name of a bot
    /// on every one of their own turns-in-waiting. A thread passes the
    /// participant's name; nil is the honest answer when nobody has taken the
    /// board yet, and the copy falls back to "Waiting" rather than inventing a
    /// pronoun for an empty chair.
    public var opponent: String?

    public init(seed: Int32, you: Uttt.Mark = .x, solo: Bool = true,
                opponent: String? = nil) {
        self.seed = seed; self.you = you; self.solo = solo
        self.opponent = opponent ?? (solo ? "nib" : nil)
        Uttt.newGame(seed: seed)
        if solo, Uttt.turn != you { Task { await botTurn() } }
    }

    public var active: Int {
        guard Uttt.over == .none else { return -1 }
        let f = Uttt.forced
        return (f != 255 && Uttt.block(f) == .none) ? f : 9
    }
    public var last: Int { Uttt.plyCount > 0 ? Uttt.move(at: Uttt.plyCount - 1) : -1 }

    public var headline: String {
        switch Uttt.over {
        case .draw:  return "Drawn"
        case .x, .o:
            if Uttt.over == you { return "You take it" }
            return opponent.map { "\($0) takes it" } ?? "They take it"
        case .none:
            if Uttt.turn == you { return "Your move" }
            return opponent.map { "Waiting on \($0)" } ?? "Waiting"
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
        if solo { return "thinking" }
        if active == 9 { return "Anywhere they like." }
        let p = UtttBubble.placeName(spoken: false)
        return p.isEmpty ? "" : p.prefix(1).uppercased() + p.dropFirst() + "."
    }

    /// The harness loads a position behind the model's back; this is how it
    /// tells the screen to look again.
    public func refresh() { positionKey &+= 1 }

    /// A tap in the board's own 0..1 space.
    public func tap(at p: CGPoint) {
        guard !busy, Uttt.over == .none, Uttt.turn == you else { return }
        let bx = min(2, Int(p.x * 3)), by = min(2, Int(p.y * 3))
        let cx = min(2, Int((p.x * 3 - CGFloat(bx)) * 3))
        let cy = min(2, Int((p.y * 3 - CGFloat(by)) * 3))
        let mv = (by * 3 + bx) * 9 + (cy * 3 + cx)
        guard Uttt.legal.contains(UInt8(mv)) else { return }
        Task { await playerMove(mv) }
    }

    private func playerMove(_ mv: Int) async {
        busy = true
        Uttt.play(mv)
        /* NIB THINKS WHILE THE INK IS STILL LANDING. The search is a few tens
         * of milliseconds and the draw is a second, so the reply is already
         * chosen when the stroke finishes - which reads as a bot that answers
         * rather than one that pauses. */
        let reply: Int? = (solo && Uttt.over == .none)
            ? await Task.detached(priority: .userInitiated) { Uttt.botMove() }.value
            : nil
        await draw(mv)
        positionKey &+= 1
        if let r = reply, Uttt.over == .none {
            Uttt.play(r)
            await draw(r)
            positionKey &+= 1
        }
        busy = false
    }

    private func botTurn() async {
        busy = true
        let m = await Task.detached(priority: .userInitiated) { Uttt.botMove() }.value
        Uttt.play(m)
        await draw(m)
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
