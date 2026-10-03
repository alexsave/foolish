import Foundation

/// THE REMATCH LOBBY a finished board's New game creates.
///
/// Lifted out of `GameSurface` unchanged, so the bytes a rematch tap seals can
/// be asserted without rendering a SwiftUI view: GameSurface keeps the surface
/// half (the fade, the stage, the X), and this keeps the half that decides what
/// the lobby IS.
public enum RematchLobby {

    /// THE SAME CHAIN (owner: "It should not start a new chain I think, it
    /// should collapse the same game (yes, wiping out the history)"). On, a
    /// rematch is the KERNEL's lobby for the finished chain - the same game id,
    /// the finished chain as its parent, seated as it finished - and its bubble
    /// collapses onto the finished game's. Off, it is today's fresh chain:
    /// rotated to the tapper, a random seed and id, its own new bubble.
    /// `rematch.samechain=0` in `dev.flags` turns it off in a debug build.
    public static let sameChainByDefault = true

    public static var sameChain: Bool {
        #if DEBUG || SOLO_TESTING
        return MessageDevBoard.flag("rematch.samechain", shipping: sameChainByDefault)
        #else
        return sameChainByDefault
        #endif
    }

    /// What a New game tap on a FINISHED board seals, and the seat this device
    /// holds in it. nil when the finished game cannot be rematched (an unnamed
    /// seat, no fool, a chain the kernel will not rematch), and the tap is an
    /// ordinary New game instead.
    ///
    /// `finished` is the finished chain on screen, `view`/`names`/`mySeat` the
    /// board built from it, `myName` this device's nickname, `capacity` the
    /// lobby's size in this chat. The same-chain lobby reads none of the last
    /// four: the kernel builds it from `finished` alone, which is what makes
    /// every tap the same lobby, and this device keeps the seat it finished in.
    public static func build(finished: Data?, view: GameView?, names: [Int: String],
                             mySeat: Int, myName: String, passing: Bool, capacity: Int,
                             sentAt: Int = MessageKernel.clockNow(),
                             sameChain: Bool = RematchLobby.sameChain)
        async throws -> (payload: Data, mySeat: Int)? {
        if sameChain {
            guard let finished, view?.isOver == true, mySeat >= 0 else { return nil }
            do {
                let payload = try await MessageKernel.shared.rematch(finished: finished,
                                                                     sentAt: sentAt)
                AnimLog.say("rematch lobby: the kernel's, same chain, seat \(mySeat)")
                return (payload, mySeat)
            } catch {
                AnimLog.say("rematch refused by the kernel (\(error)) - an ordinary New game")
                return nil
            }
        }
        guard let r = rotatedRoster(view: view, names: names, mySeat: mySeat,
                                    myName: myName) else { return nil }
        let payload = try await freshChain(joins: r.joins, foolSeat: r.foolSeat,
                                           passing: passing,
                                           capacity: max(capacity, r.joins.count),
                                           sentAt: sentAt)
        return (payload, 0)
    }

    /// The rematch roster, read STRAIGHT OFF the finished board: the same table,
    /// in the same cycle, rotated so this device sits at seat 0. nil when this
    /// is not a game a rematch can be built from.
    ///
    /// Rotated because seat 0 is the creator's by construction (`createWaiting`)
    /// and whoever taps New game is the creator. Preserving the CYCLE is what
    /// matters, not the numbers - the wire keys a roster rotation-canonically
    /// for exactly this reason - so the same table comes back as the same
    /// table however it is spun.
    ///
    /// My own name comes from the store, not from the old game's join: this
    /// device may have been renamed since, and the name it seals now is the one
    /// its seat will be recognised by.
    public static func rotatedRoster(view: GameView?, names: [Int: String], mySeat me: Int,
                                     myName: String) -> (joins: [MessageJoin], foolSeat: Int)? {
        guard let v = view, v.isOver, v.gameOver >= 0 else { return nil }
        let n = v.players.count
        guard n >= 2, me >= 0, me < n, v.gameOver < n else { return nil }

        // Names BY SEAT. A seat with no name cannot be recognised by its owner
        // on the other device (SeatIdentity.seatClaimedByName is what lets a
        // prefilled lobby seat people who never tapped Join), so a roster
        // missing one is not a rematch roster at all - the tap falls back to an
        // ordinary new game rather than seating somebody as a blank.
        var joins: [MessageJoin] = []
        for s in 0..<n {
            let old = (s + me) % n
            let name = old == me ? myName : (names[old] ?? "")
            guard !name.isEmpty else { return nil }
            joins.append(MessageJoin(seat: s, name: name))
        }
        return (joins, (v.gameOver - me + n) % n)
    }

    /// The lobby itself, sealed as a FRESH chain: a new random seed and game
    /// id, a zero parent, dealt at the chat's capacity, with the rematch carry
    /// armed. Leaves the lobby's deal resident, as every lobby seal does.
    ///
    /// `passing` is the finished game's own rule, carried across: `newGame`
    /// resets the kernel's rules to the classic transfer game, so a rematch of a
    /// podkidnoy table would otherwise silently deal a perevodnoy one.
    public static func freshChain(joins: [MessageJoin], foolSeat: Int, passing: Bool,
                                  capacity: Int,
                                  sentAt: Int = MessageKernel.clockNow()) async throws -> Data {
        var seed = Data(count: 32)
        for i in 0..<32 { seed[i] = UInt8.random(in: 0...UInt8.max) }
        let gameId = UInt64.random(in: 1...UInt64.max)
        try await MessageKernel.shared.newGame(seed: seed, players: capacity)
        await MessageKernel.shared.setPassing(passing)
        let armed = await MessageKernel.shared.armRematchCarry(joins: joins, foolSeat: foolSeat)
        AnimLog.say("rematch lobby: n=\(joins.count) fool@\(foolSeat) armed=\(armed)")
        return try await MessageKernel.shared.seal(
            phase: 0, lastActorSeat: 0, gameId: gameId,
            parent8: Data(repeating: 0, count: 8), joins: joins, sentAt: sentAt)
    }
}
