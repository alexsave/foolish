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

    public init(seed: Int32, you: Uttt.Mark = .x, solo: Bool = true) {
        self.seed = seed; self.you = you; self.solo = solo
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
        case .draw: return "Drawn"
        case .x, .o: return Uttt.over == you ? "You take it" : "nib takes it"
        case .none: return Uttt.turn == you ? "Your move" : "nib"
        }
    }
    public var subline: String {
        if Uttt.over != .none { return "\(Uttt.plyCount) moves." }
        if Uttt.turn != you { return "thinking" }
        return active == 9 ? "Anywhere you like." : ""
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
