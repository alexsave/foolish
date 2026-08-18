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
    ///    am the *other* seat when I did not send it — but ONLY in a DM
    ///    (`chatIsDM`): that inference's whole premise is "only two humans can
    ///    be holding a phone in this thread". In a GROUP chat a 2-player game's
    ///    bubble can be tapped by any member — the thread this hardening comes
    ///    from had a third player one tap away from being silently seated as
    ///    the second player's seat, with that player's hand face-up.
    /// 3. Otherwise **ambiguous** — cache lost and no exact signal: the caller
    ///    shows the §6.3 picker (DEBUG) or the public spectator board (Release).
    ///
    /// `lastActorSeat` is the envelope's `last_actor_seat`; `nPlayers` its
    /// `n_players`. A cached seat outside `0..<nPlayers` is treated as absent
    /// (a stale cache from a different game must never seat you out of range).
    public static func resolve(cachedSeat: Int?,
                               senderIsLocal: Bool,
                               nPlayers: Int,
                               lastActorSeat: Int,
                               chatIsDM: Bool) -> Resolution {
        if let s = cachedSeat, s >= 0, s < nPlayers {
            return .known(s)
        }
        if senderIsLocal, lastActorSeat >= 0, lastActorSeat < nPlayers {
            return .known(lastActorSeat)            // S1: I am the last actor, exact for any N
        }
        if chatIsDM, nPlayers == 2, lastActorSeat >= 0, lastActorSeat < 2 {
            return .known(1 - lastActorSeat)        // S1: DM 2p and I didn't send it ⇒ the other seat
        }
        return .ambiguous
    }

    /// Does this bubble's own roster DISOWN the cached seat — list it under a
    /// different name than the one this device recorded when it claimed it?
    ///
    /// The cache says "I am seat s"; the chain's `joins` say who seat s IS. In
    /// the ordinary flow they agree (a claim's name propagates verbatim down
    /// every chain built on it). They disagree in exactly one situation: a
    /// seat-claim race, where two people claimed the same seat off the same
    /// stale bubble and this device's claim lost — the canonical chain now
    /// carries the OTHER person at that seat. Trusting the cache then seats
    /// this device on someone else's hand, face-up, and lets it move for them.
    ///
    /// `recordedName` is the name THIS DEVICE's cache row stored for the seat
    /// at claim time — not the current nickname, which the human can change
    /// mid-game. Either side missing (no row name, or the chain has no join at
    /// that seat) is NOT a disownment: stay permissive, the range check and
    /// the §6 fallbacks still apply. Equal names collide only when two humans
    /// picked the same nickname — the same trust level §6.3 already accepts.
    public static func cacheDisownedByJoins(cachedSeat: Int?, recordedName: String?,
                                            joins: [MessageJoin]) -> Bool {
        guard let s = cachedSeat, let mine = recordedName,
              let listed = joins.first(where: { $0.seat == s })?.name else { return false }
        return listed != mine
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
    ///
    /// `recordedName` extends the membership check by NAME (cacheDisownedByJoins):
    /// a lobby whose join at my cached seat carries somebody else's name is a
    /// claim race this device LOST — reading as "not joined" here is what puts
    /// the Join button back, so the loser re-claims the next free seat (§5.2's
    /// original promise) instead of sitting "seated" on a seat that is no
    /// longer theirs. nil skips the name half (seat-membership only, as before).
    public static func resolveInLobby(cachedSeat: Int?, senderIsLocal: Bool,
                                      nPlayers: Int, lastActorSeat: Int,
                                      joins: [MessageJoin], chatIsDM: Bool,
                                      recordedName: String? = nil) -> Int? {
        let cached = cacheDisownedByJoins(cachedSeat: cachedSeat, recordedName: recordedName,
                                          joins: joins) ? nil : cachedSeat
        switch resolve(cachedSeat: cached, senderIsLocal: senderIsLocal,
                       nPlayers: nPlayers, lastActorSeat: lastActorSeat, chatIsDM: chatIsDM) {
        case .known(let s): return joins.contains { $0.seat == s } ? s : nil
        case .ambiguous:    return nil
        }
    }
}
