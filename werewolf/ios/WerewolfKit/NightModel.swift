// What the night screen is showing, and the one send it can make.
//
// THE MODEL HOLDS NO RULE. Whether this seat may send, what it is allowed to
// see, whose call tonight is, whether the carry is offered - all of it is read
// from the kernel on demand. What lives here is the two things SwiftUI needs and
// C cannot give it: a wall clock ticking the send floor down, and the seat the
// finger is currently on.
import Combine
import Foundation
import SwiftUI

@MainActor
public final class NightModel: ObservableObject {

    /// The seat this device is. -1 is a spectator, which is what an unresolved
    /// seat degrades to rather than guessing.
    public let mySeat: Int

    /// The seat currently picked, or nil. Staged locally: nothing reaches the
    /// kernel until Send, because the kernel records are the chain and a chain is
    /// not a draft.
    @Published public private(set) var picked: Int?

    /// The wolves' line, typed. Empty for everyone else, and the screen does not
    /// show the field to them - which is the point of the design, not a nicety:
    /// a field that appears only for wolves is a field a wolf must never be seen
    /// to use, and they are not, because it rides inside the same record.
    @Published public var line: String = ""

    /// Seconds left on the send floor. Drives the button's label, and the button
    /// asks the kernel again before it sends - this is the countdown, not the gate.
    @Published public private(set) var floorRemaining: Int

    /// Set once the record has gone into the kernel and the bubble is staged.
    @Published public private(set) var staged = false

    /// The last refusal the kernel gave, for the one line of UI that shows it.
    @Published public private(set) var refusal: String?

    private let kernel: Kernel
    private let openedAt: UInt16
    private var ticker: AnyCancellable?
    private let clock: () -> UInt16

    /// `clock` is injectable so a test can spend the floor without waiting ten
    /// seconds, and so the floor is never measured against a timer's own drift.
    public init(mySeat: Int,
                kernel: Kernel = .shared,
                clock: @escaping () -> UInt16 = { UInt16(truncatingIfNeeded: Int(Date().timeIntervalSince1970)) }) {
        self.mySeat = mySeat
        self.kernel = kernel
        self.clock = clock
        self.openedAt = clock()
        self.floorRemaining = kernel.sendFloorSeconds
        tick()
        ticker = Timer.publish(every: 0.25, on: .main, in: .common).autoconnect()
            .sink { [weak self] _ in self?.tick() }
    }

    private func tick() {
        let left = kernel.sendFloorRemaining(openedAt: openedAt, now: clock())
        if left != floorRemaining { floorRemaining = left }
        if left == 0 { ticker?.cancel(); ticker = nil }
    }

    // ------------------------------------------------------------- reading ---

    public var phase: Phase { kernel.phase(mySeat) }
    public var night: Int { kernel.night(mySeat) }
    public var seatCount: Int { kernel.playerCount(mySeat) }
    public var myRole: Role { kernel.myRole(mySeat) }

    /// A living wolf, and therefore the only viewer with a channel. Asked of the
    /// kernel rather than remembered: a wolf who is lynched loses the channel, and
    /// a cached bool would keep handing it to a corpse.
    public var amWolf: Bool { myRole == .wolf }

    public func name(_ seat: Int) -> String {
        for i in 0..<kernel.rosterCount where kernel.rosterSeat(i) == seat {
            if let n = kernel.rosterName(i) { return n }
        }
        return "Seat \(seat + 1)"
    }

    public func isAlive(_ seat: Int) -> Bool { kernel.isAlive(mySeat, seat) }
    public func sent(_ seat: Int) -> Sent { kernel.sent(mySeat, seat) }
    public func role(of seat: Int) -> Role { kernel.role(mySeat, of: seat) }

    /// Tonight's deciding wolf, or nil - which is what every non-wolf is handed.
    public var decider: Int? {
        let d = kernel.decider(mySeat)
        return d == noSeat ? nil : d
    }

    public var channel: [(seat: Int, line: String)] {
        (0..<kernel.chatCount(mySeat)).compactMap { i in
            guard let l = kernel.chatLine(mySeat, i) else { return nil }
            return (kernel.chatSeat(mySeat, i), l)
        }
    }

    /// What the seer was told about a seat, if this viewer is the seer.
    public func reading(of seat: Int) -> Team { kernel.reading(mySeat, of: seat) }

    /// Seats that may be picked: the living, minus this one.
    public var choosable: [Int] {
        (0..<seatCount).filter { $0 != mySeat && isAlive($0) }
    }

    /// Everyone still to send. The screen may show this because it is everyone -
    /// which is exactly why the night is built this way.
    public var waitingOn: [Int] {
        (0..<seatCount).filter { isAlive($0) && sent($0) == .no }
    }

    /// Have I already spent my one record tonight?
    public var iHaveSent: Bool { mySeat >= 0 && sent(mySeat) != .no }

    /// The one prompt, and it is the SAME prompt for every role. That sameness is
    /// the design: a wolf's pick is a kill vote, the seer's is a question, a
    /// villager's is who they dreamt about, and none of them can be told apart by
    /// anyone watching the thread.
    public var prompt: String {
        switch myRole {
        case .wolf: return "Who do you want gone?"
        case .seer: return "Who do you want to see?"
        default:    return "Who did you dream about?"
        }
    }

    // ------------------------------------------------------------- writing ---

    public func pick(_ seat: Int) {
        // A spectator may not pick. `mySeat < 0` is what an unresolved seat
        // degrades to, and a device that let one stage a choice would be a device
        // holding a night record it can never legally send.
        guard mySeat >= 0, !staged, seat != mySeat, isAlive(seat) else { return }
        picked = (picked == seat) ? nil : seat
        refusal = nil
    }

    /// Whether the carry may be OFFERED. The kernel's clock decides, because the
    /// minute is a rule; whether there is anybody late is read off the view.
    public func carryOffered(lastSealAt: UInt16) -> Bool {
        guard kernel.mayCarry(lastSealAt: lastSealAt, now: clock()) else { return false }
        return waitingOn.contains { $0 != mySeat }
    }

    /// ONE TAP, ONE RECORD, ONE BUBBLE. The line rides inside the record; it is
    /// never a second call and never a second message.
    ///
    /// Returns the payload to stage, or nil if the kernel refused - and it asks
    /// the kernel for the floor again here rather than trusting `floorRemaining`,
    /// because the published value is a countdown for the label and a stale
    /// countdown must not be able to buy an early send.
    public func send(gameId: UInt64, parent: Data?, carry: Bool = false) -> Data? {
        guard mySeat >= 0, !staged else { return nil }
        let now = clock()
        guard kernel.sendFloorRemaining(openedAt: openedAt, now: now) == 0 else {
            refusal = "Take a moment."
            return nil
        }
        do {
            // THE LINE GOES THROUGH WHATEVER THIS SEAT IS. It is tempting to gate
            // it on `amWolf` here, and that is the bug: a model that quietly drops
            // a line it will not send is a client that believes it sent one, and a
            // client that believes it sent one will send a second bubble to fix it.
            // One gate, in C, which refuses (WW_ECHAT) and says so.
            try kernel.nightAct(seat: mySeat,
                                target: picked ?? noSeat,
                                line: line.isEmpty ? nil : line,
                                carry: carry)
            let payload = try kernel.seal(gameId: gameId, sentAt: now, parent: parent)
            staged = true
            refusal = nil
            return payload
        } catch {
            refusal = "That move was refused."
            return nil
        }
    }
}
