// MessageGameStore — the App Group cache (design §6.1 / §9.3).
//
// The extension gets no background time and no push (§11.4): the message IS the
// state. So this cache is PURELY an optimization plus the drawer's game list —
// §6/§7 must survive its total loss, and every method here is written so that a
// missing or corrupt suite degrades to "no games", never a crash.
//
// It holds two things per game_id: the durable seat identity this device claimed
// (§6.1, the one fact a fresh bubble cannot always recover), and the preferred
// chain seen so far as raw payload bytes, so Rule P (§7.2, decided in C over two
// payloads) can compare an incoming bubble against what we already trust — even a
// stale collapsed bubble the human taps.

import Foundation

/// One game's cached row. Denormalized display fields (round/turn/phase/names)
/// let the drawer list render WITHOUT decoding each chain (decoding adopts, §7.3,
/// so it must not happen just to draw a list). `payloadBase32` is the preferred
/// chain itself, for Rule P.
public struct MessageGameRecord: Codable, Equatable, Sendable {
    public var gameId: String
    public var mySeat: Int
    public var nPlayers: Int
    public var round: Int
    public var turn: Int
    public var phase: Int              // 0 WAITING · 1 ACCEPT · 2 LIVE · 3 FINISHED
    public var finished: Bool
    public var names: [Int: String]
    public var payloadBase32: String   // the preferred chain (URL body), for Rule P
    public var updatedAt: Double        // seconds since 1970; newest sorts first in the drawer

    public init(gameId: String, mySeat: Int, nPlayers: Int, round: Int, turn: Int,
                phase: Int, finished: Bool, names: [Int: String],
                payloadBase32: String, updatedAt: Double) {
        self.gameId = gameId; self.mySeat = mySeat; self.nPlayers = nPlayers
        self.round = round; self.turn = turn; self.phase = phase
        self.finished = finished; self.names = names
        self.payloadBase32 = payloadBase32; self.updatedAt = updatedAt
    }

    public func name(_ seat: Int) -> String { names[seat] ?? "Seat \(seat + 1)" }
}

/// One staged-but-unsent action in a game's pending ledger (§7.4 / §17.15). It is
/// what Rule R replays when a preferred chain is adopted that does not contain it:
/// `round` is the bout it was composed against (the round-boundary guard's key),
/// `seat` who staged it, `move` the action itself. Kept small and durable so a
/// killed extension or a bubble that arrives mid-staging never strands the move.
public struct PendingAction: Codable, Equatable, Sendable {
    public var seat: Int
    public var round: Int
    public var move: Move
    public init(seat: Int, round: Int, move: Move) {
        self.seat = seat; self.round = round; self.move = move
    }
}

public final class MessageGameStore {
    /// The shared group both the app (drawer) and the extension read/write. A
    /// `var`, not a `let`, for one reason only: the FoolishHarness test app
    /// rebinds it to a per-fake-participant suite when you switch players, so each
    /// of the 2-8 pretend people gets its OWN seat cache (which is what a real
    /// device has). The shipping app and extension never touch this — they run on
    /// the real App Group.
    public static var shared = MessageGameStore(suiteName: "group.cards.foolish")

    private let defaults: UserDefaults?
    private let key = "fmsg.games.v1"
    private let pendingKey = "fmsg.pending.v1"
    // The suite is nil on an unsigned/misconfigured build (no App Group). That is
    // not fatal — the cache simply reports empty and every §6/§7 rule still holds
    // off the payload — so this class NEVER force-unwraps it.
    public init(suiteName: String) {
        self.defaults = UserDefaults(suiteName: suiteName)
    }
    /// For tests: an explicit UserDefaults (a throwaway suite), no App Group.
    public init(defaults: UserDefaults) { self.defaults = defaults }

    /// This device's display name, seated into `joins` when I create/join a game
    /// (§5.2). Defaults to a neutral label; the human can change it. Lives in the
    /// group so the app and extension agree.
    public var nickname: String {
        get { defaults?.string(forKey: "fmsg.nickname") ?? "Me" }
        set { defaults?.set(newValue, forKey: "fmsg.nickname") }
    }

    /// Whether the human has ever chosen a name on this device (§B3). False means
    /// `nickname` is still the neutral default, so a player about to be seated must
    /// be asked once (the 2-player receiver has no setup/lobby screen to enter it,
    /// unlike the creator and the 3-8p lobby joiners). Once set, every later game
    /// reuses it and never re-asks.
    public var hasSetNickname: Bool {
        guard let n = defaults?.string(forKey: "fmsg.nickname") else { return false }
        return !n.trimmingCharacters(in: .whitespaces).isEmpty
    }

    // MARK: read

    /// This device's seat in `gameId`, or nil if unknown — the §6.1 primary
    /// answer. nil means fall through to §6.2/§6.3.
    public func seat(gameId: String) -> Int? { record(gameId: gameId)?.mySeat }

    public func record(gameId: String) -> MessageGameRecord? { all()[gameId] }

    /// Every cached game, newest first — the drawer list source (§10 compact).
    public func games() -> [MessageGameRecord] {
        all().values.sorted { $0.updatedAt > $1.updatedAt }
    }

    // MARK: write

    /// Insert or replace a game's row. Callers pass a row they built at adopt/
    /// seal time (they know the seat and the freshly-preferred chain); this only
    /// persists it. Writing a row with a strictly-older `updatedAt` than what is
    /// stored is ignored, so a late-delivered stale bubble can't roll the cache
    /// backward (§7.2 is delivery-order-independent).
    public func put(_ rec: MessageGameRecord) {
        var map = all()
        if let existing = map[rec.gameId], existing.updatedAt > rec.updatedAt { return }
        map[rec.gameId] = rec
        persist(map)
    }

    public func remove(gameId: String) {
        var map = all()
        guard map.removeValue(forKey: gameId) != nil else { return }
        persist(map)
    }

    // MARK: pending ledger (§7.4 Rule R / §17.15) — durable, small, current-round

    /// This game's staged-but-unsent actions, in ledger order — what Rule R
    /// replays onto a newly-adopted chain so no local move is silently lost.
    public func pending(gameId: String) -> [PendingAction] { allPending()[gameId] ?? [] }

    /// Replace a game's pending ledger. The controller writes its whole staged
    /// list here on every apply/undo so a mid-staging interruption (a bubble that
    /// arrives, or a killed extension) survives; the adopt path reads it back and
    /// rebases. An empty list clears the row.
    public func setPending(_ list: [PendingAction], gameId: String) {
        var map = allPending()
        if list.isEmpty { guard map.removeValue(forKey: gameId) != nil else { return } }
        else { map[gameId] = list }
        persistPending(map)
    }

    /// Drop this game's pending ledger — called once a chain containing these
    /// moves is committed to the thread (§7.6 didStartSending), so they are no
    /// longer unacked and must never be replayed on top of themselves.
    public func clearPending(gameId: String) { setPending([], gameId: gameId) }

    private func allPending() -> [String: [PendingAction]] {
        guard let data = defaults?.data(forKey: pendingKey),
              let map = try? JSONDecoder().decode([String: [PendingAction]].self, from: data)
        else { return [:] }
        return map
    }

    private func persistPending(_ map: [String: [PendingAction]]) {
        guard let data = try? JSONEncoder().encode(map) else { return }
        defaults?.set(data, forKey: pendingKey)
    }

    // MARK: storage (a corrupt blob is treated as empty, never thrown)

    private func all() -> [String: MessageGameRecord] {
        guard let data = defaults?.data(forKey: key),
              let map = try? JSONDecoder().decode([String: MessageGameRecord].self, from: data)
        else { return [:] }
        return map
    }

    private func persist(_ map: [String: MessageGameRecord]) {
        guard let data = try? JSONEncoder().encode(map) else { return }
        defaults?.set(data, forKey: key)
    }
}
