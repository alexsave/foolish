// SeatIdentity — "which seat am I?" for an iMessage game, design §6.
//
// There are NO accounts and NO server. Apple's participant UUIDs are scoped
// per-device-per-conversation (§6), so they can never travel in the payload and
// they never identify a seat across devices. Seat identity is therefore resolved
// from three signals, in strict priority — and this type is the pure decision
// over those signals, with zero Messages-framework coupling so it can be tested
// exhaustively (the actual UUID comparison happens in the extension and is fed in
// here as `senderIsLocal`).
//
// This decides who you ARE, never what you MAY DO — legality stays in the kernel.

import Foundation

public enum SeatIdentity {

    /// The outcome of resolving a seat. `.ambiguous` is not an error: it means
    /// the only honest answer is to ask the human (§6.3 nickname picker).
    public enum Resolution: Equatable, Sendable {
        case known(Int)
        case ambiguous          // N≥3, no cache, and I'm not the last actor
    }

    /// Resolve `mySeat` from the three §6 layers, highest priority first:
    ///
    /// 1. **App Group cache** (`cachedSeat`) — set with certainty at create/join
    ///    time; authoritative for the life of the install (§6.1).
    /// 2. **Sender inference** (§6.2, Rule S1) — if THIS device sent the tapped
    ///    bubble, I am its `lastActorSeat`, exact for any N. In a 2-player game I
    ///    am the *other* seat when I did not send it (there are only two).
    /// 3. Otherwise **ambiguous** — N≥3, cache lost, not the last actor: the
    ///    caller must show the nickname picker (§6.3).
    ///
    /// `lastActorSeat` is the envelope's `last_actor_seat`; `nPlayers` its
    /// `n_players`. A cached seat outside `0..<nPlayers` is treated as absent
    /// (a stale cache from a different game must never seat you out of range).
    public static func resolve(cachedSeat: Int?,
                               senderIsLocal: Bool,
                               nPlayers: Int,
                               lastActorSeat: Int) -> Resolution {
        if let s = cachedSeat, s >= 0, s < nPlayers {
            return .known(s)
        }
        if senderIsLocal, lastActorSeat >= 0, lastActorSeat < nPlayers {
            return .known(lastActorSeat)            // S1: I am the last actor, exact for any N
        }
        if nPlayers == 2, lastActorSeat >= 0, lastActorSeat < 2 {
            return .known(1 - lastActorSeat)        // S1: 2p and I didn't send it ⇒ the other seat
        }
        return .ambiguous
    }

    /// `resolve`, gated for a LOBBY bubble specifically (note 14, HARNESS_NOTES_R2):
    /// a resolved seat only counts as MINE if this bubble's own `joins` list
    /// actually contains it. `resolve` alone answers "who does the cache/sender
    /// signal say I am" — correct for a live board, where every chain in a game
    /// carries every seated player forward — but wrong for a lobby: an OLDER
    /// WAITING bubble, reopened after I've since joined, still resolves my
    /// cached seat even though THAT bubble's own `joins` predate my join. That
    /// granted Start/Send to someone the lobby does not list. nil covers both
    /// `.ambiguous` and "resolved, but not actually in this bubble's joins".
    public static func resolveInLobby(cachedSeat: Int?, senderIsLocal: Bool,
                                      nPlayers: Int, lastActorSeat: Int,
                                      joins: [MessageJoin]) -> Int? {
        switch resolve(cachedSeat: cachedSeat, senderIsLocal: senderIsLocal,
                       nPlayers: nPlayers, lastActorSeat: lastActorSeat) {
        case .known(let s): return joins.contains { $0.seat == s } ? s : nil
        case .ambiguous:    return nil
        }
    }
}
