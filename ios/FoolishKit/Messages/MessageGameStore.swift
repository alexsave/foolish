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
///
/// `chatKey` is the conversation this row belongs to (`ChatKey.make` over the
/// conversation's whole participant set — see that type for why the local
/// participant identifier ALONE is not a conversation identity). Without it
/// `games()` was device-wide: opening the extension in chat B with no bubble
/// selected could resolve `.known` off chat A's cached seat and stage chat A's
/// deal-seed-bearing payload into chat B, leaking chat A players' hands to chat
/// B's participants. Every read below is scoped by this field for that reason.
public struct MessageGameRecord: Codable, Equatable, Sendable {
    public var gameId: String
    public var chatKey: String
    public var mySeat: Int
    public var nPlayers: Int
    public var round: Int
    public var turn: Int
    public var phase: Int              // 0 WAITING · 1 ACCEPT · 2 LIVE · 3 FINISHED
    public var finished: Bool
    public var names: [Int: String]
    public var payloadBase32: String   // the preferred chain (URL body), for Rule P
    public var updatedAt: Double        // seconds since 1970; newest sorts first in the drawer

    public init(gameId: String, chatKey: String, mySeat: Int, nPlayers: Int, round: Int, turn: Int,
                phase: Int, finished: Bool, names: [Int: String],
                payloadBase32: String, updatedAt: Double) {
        self.gameId = gameId; self.chatKey = chatKey; self.mySeat = mySeat; self.nPlayers = nPlayers
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
    /// The App Group this target owns. A `var`, not a `let`, for one reason only:
    /// the FoolishHarness test app rebinds it to a per-fake-participant suite when
    /// you switch players, so each of the 2-8 pretend people gets its OWN seat
    /// cache (which is what a real device has). The shipping extension never
    /// touches this — it runs on the group named in its own Info.plist.
    public static var shared = MessageGameStore(suiteName: defaultSuiteName)

    /// Read from the RUNNING target's Info.plist rather than hardcoded, because
    /// FoolishKit is linked by more than one product and they do not share a
    /// group: the standalone iMessage app owns `group.cards.foolish.msg`, while
    /// the host app's group is `group.cards.foolish`. A literal here would bake
    /// one product's group into a framework the other product also loads.
    ///
    /// `Bundle.main` inside an app extension is the .appex, so this resolves to
    /// FoolishMessages/Info.plist when the extension runs.
    public static var defaultSuiteName: String {
        (Bundle.main.object(forInfoDictionaryKey: "FoolishAppGroup") as? String)
            ?? "group.cards.foolish.msg"
    }

    private let defaults: UserDefaults?
    // v1 -> v2: `MessageGameRecord` gained a required `chatKey` field (the chat-
    // scoping security fix — see the type doc). Swift's synthesized Codable does
    // NOT fall back to a default when a key is missing, it throws — so a v1 blob
    // would decode every row to nil and `all()` would silently drop them, which
    // reads the same as "cache lost" (harmless) EXCEPT it would do so non-
    // deterministically depending on what happened to be cached at upgrade time.
    // Bumping the storage key instead makes old blobs simply invisible, always,
    // which is the same "purely an optimization, §6/§7 survive its total loss"
    // guarantee the file header already promises — cheaper and more honest than
    // a custom decoder just to preserve rows that carry no chatKey to be correct.
    private let key = "fmsg.games.v2"
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

    /// This device's seat in `gameId` WITHIN `chatKey`, or nil if unknown — the
    /// §6.1 primary answer. nil means fall through to §6.2/§6.3. A row that
    /// belongs to a different chat is treated exactly like a cache miss, never
    /// returned: Rule P (§7.2) compares a cached chain against an incoming
    /// bubble, and it must never compare across chats, or a stale/foreign row
    /// could out-rank (or falsely "confirm") a bubble it was never sealed
    /// against.
    public func seat(gameId: String, chatKey: String) -> Int? {
        record(gameId: gameId, chatKey: chatKey)?.mySeat
    }

    public func record(gameId: String, chatKey: String) -> MessageGameRecord? {
        guard let rec = all()[gameId], rec.chatKey == chatKey else { return nil }
        return rec
    }

    /// The row for `gameId` regardless of which chat it was cached under — the
    /// lookup for a bubble that is IN HAND (tapped, or just arrived). `ChatKey`
    /// is the sorted participant-UUID set, so ADDING OR REMOVING A GROUP MEMBER
    /// changes it mid-game — after which every strictly-scoped read above
    /// misses, this device's own seat degrades to §6.2/§6.3 (the Release
    /// spectator board: a seated player suddenly "can't act"), and Rule P loses
    /// its cached side. When the caller holds an actual bubble, the bubble's
    /// `game_id` — a `UInt64.random` minted once at creation — is itself the
    /// proof it is this game's row: a row can only exist because THIS DEVICE
    /// created/joined/played that game, whatever key the conversation hashed to
    /// at the time. The cross-chat leak the scoping fixed lived in the OTHER
    /// lookup — `games(chatKey:)`, the no-bubble reopen, which has no gameId to
    /// anchor on and stays strictly scoped. The next `put` re-keys the row to
    /// the current chatKey, so one tap heals the scoped reads too.
    public func recordForBubble(gameId: String) -> MessageGameRecord? { all()[gameId] }

    /// `seat(gameId:chatKey:)`'s bubble-anchored twin (see recordForBubble).
    public func seatForBubble(gameId: String) -> Int? { recordForBubble(gameId: gameId)?.mySeat }

    /// Rule P extended to lobby (phase-0/WAITING) bubbles (note 15,
    /// HARNESS_NOTES_R2). A WAITING envelope is otherwise exempt from Rule P's
    /// round/turn comparison — every lobby sits at round 0/turn 0, so that
    /// comparison is meaningless — but a STALE WAITING invite can still be
    /// tapped after the SAME game has since gone LIVE or FINISHED elsewhere,
    /// and every WAITING envelope renders as an open lobby regardless of how
    /// many actually joined (`createWaiting`'s doc in MessagesRootView.swift),
    /// so a stale one rendered a phantom full lobby instead of the real board.
    /// True means the cache is strictly newer and the caller should adopt IT
    /// instead of showing the incoming (stale) lobby bubble. `cachedPhase` nil
    /// means nothing is cached for this game yet, which never wins.
    public static func lobbyCachePreferred(cachedPhase: Int?, incomingPhase: Int) -> Bool {
        guard let cachedPhase else { return false }
        return cachedPhase > incomingPhase
    }

    /// Every cached game IN THIS CHAT, newest first — the drawer list source
    /// (§10 compact). Unscoped would be device-wide (the bug this type doc's
    /// chatKey paragraph describes): reopening the extension with no bubble
    /// selected would resolve the newest game from ANY conversation, not this
    /// one.
    public func games(chatKey: String) -> [MessageGameRecord] {
        all().values.filter { $0.chatKey == chatKey }.sorted { $0.updatedAt > $1.updatedAt }
    }

    // MARK: write

    /// Insert or replace a game's row. Callers pass a row they built at adopt/
    /// seal time (they know the seat and the freshly-preferred chain); this only
    /// persists it. Writing a row with a strictly-older `updatedAt` than what is
    /// stored is ignored, so a late-delivered stale bubble can't roll the cache
    /// backward (§7.2 is delivery-order-independent).
    ///
    /// The underlying map is still keyed by `gameId` alone, device-wide, not by
    /// `(chatKey, gameId)` — a cross-chat gameId collision would make two
    /// different games fight over one row. Left as-is deliberately: `gameId` is
    /// a `UInt64.random` (createWaiting/startGenesis), so a collision is as
    /// astronomically unlikely here as it is for the pending ledger (§ below,
    /// same reasoning) — every READ is chatKey-scoped regardless, so the only
    /// consequence of the pathological collision would be one row's
    /// `updatedAt`/eviction racing the other's, not a leak.
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
    //
    // AUDITED for the chat-scoping fix, deliberately left keyed by gameId ALONE
    // (no chatKey): a pending action can only exist for a game this device is
    // actively staging a move in, and `gameId` is a `UInt64.random` chosen at
    // create time (createWaiting/startGenesis) — a same-device collision across
    // two different chats' games is not a practical concern (same argument as
    // `put` above). Adding chatKey here would only guard against a threat that
    // does not exist, for no reader-side benefit: unlike `record`/`seat`/`games`,
    // nothing about `pending` can leak another chat's hand — it is this device's
    // OWN staged moves, never another chat's cached seat or payload.

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

    /// Drop EVERY game's pending ledger. The harness uses this on deliver (its
    /// stand-in for didStartSending) to commit staged moves without threading a
    /// gameId through; a real device clears the one game via clearPending(gameId:).
    public func clearAllPending() { persistPending([:]) }

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
