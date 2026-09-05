// MessageGameStore — the App Group store (design §6.1 / §9.3).
//
// The extension gets no background time and no push (§11.4): the message IS the
// state. So this store is PURELY an optimization — §6/§7 must survive its total
// loss, and every method here is written so that a missing or corrupt suite
// degrades to "nothing stored", never a crash.
//
// ROUND 7 (owner: "if we are storing anything other than the player nickname I
// strongly suggest removing it — the last text has everything we need to animate
// the last move; the cache seems to only be hurting us"): the thing that could
// make the extension render something OTHER than the tapped bubble — the
// PREFERRED-CHAIN payload cache + its denormalized display fields (Rule P §7.2) —
// is GONE. The router now always renders exactly the bubble you tapped, and the
// last-move animation was already derived from that bubble alone. `put`/`record`/
// `games`/`remove` are retained as inert stubs so the §5/§6/§7 call sites keep
// compiling and simply behave as "nothing cached", which those paths were always
// written to survive.
//
// What the store still keeps, all device-local and none of it touching what the
// board renders/animates:
//   • the device NICKNAME;
//   • per game, the SEAT this device holds (§6.1 — the one fact a fresh bubble
//     cannot always recover, and which a 3+ player game is unplayable without);
//   • the cosmetic HAND ARRANGEMENT per game (round-8 #4), and the one-shot
//     JUST-SENT marker (round-9 #5) — both presentation-only.
// ROUND 9 (owner): the pending-move LEDGER (Rule R §7.4) is removed too — see
// the note at its old section below.

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

/// Round 7: the whole of what the store keeps per game now — the seat this device
/// holds, scoped to the chat it was claimed in (a device-wide seat could seat you
/// into another conversation's game; see `MessageGameRecord`'s chatKey note).
public struct SeatRow: Codable, Equatable, Sendable {
    public var chatKey: String
    public var seat: Int
    public init(chatKey: String, seat: Int) { self.chatKey = chatKey; self.seat = seat }
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
    // Round-8 #4: the local player's cosmetic hand arrangement, per game (see
    // the "hand order" section below).
    private let handOrderKey = "fmsg.handorder.v1"
    // Round 7: the ONLY per-game fact still persisted — the seat this device holds
    // in a game, scoped by chat (see `SeatRow`/`chatKey`). A fresh key, so a
    // device upgrading from the old `fmsg.games.v2` blob simply starts empty here
    // (harmless: 2p seats re-infer from the payload, and a mid-game 3+ seat re-
    // caches the next time this device seals a move into that game).
    private let seatsKey = "fmsg.seats.v1"
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
        guard let row = allSeats()[gameId], row.chatKey == chatKey else { return nil }
        return row.seat
    }

    /// Persist this device's seat in `gameId` within `chatKey` (§6.1) — the one
    /// per-game fact kept in round 7. Overwrites any prior seat for the game (a
    /// device only ever holds one seat in a given game).
    public func setSeat(gameId: String, chatKey: String, seat: Int) {
        var map = allSeats()
        map[gameId] = SeatRow(chatKey: chatKey, seat: seat)
        guard let data = try? JSONEncoder().encode(map) else { return }
        defaults?.set(data, forKey: seatsKey)
    }

    /// Drop this device's seat in `gameId` - the LEAVE half of `setSeat`
    /// (round 16). Unscoped by chatKey on purpose, mirroring `seatForBubble`:
    /// the caller is holding the bubble it just resealed, so the gameId is
    /// itself the proof, and a group-membership change that re-keyed the chat
    /// must not leave a row behind that seats me in a lobby I walked out of.
    public func forgetSeat(gameId: String) {
        var map = allSeats()
        guard map.removeValue(forKey: gameId) != nil else { return }
        guard let data = try? JSONEncoder().encode(map) else { return }
        defaults?.set(data, forKey: seatsKey)
    }

    private func allSeats() -> [String: SeatRow] {
        guard let data = defaults?.data(forKey: seatsKey),
              let map = try? JSONDecoder().decode([String: SeatRow].self, from: data)
        else { return [:] }
        return map
    }

    /// My seat for `gameId` regardless of which chat it was claimed under - the
    /// lookup for a bubble that is IN HAND (tapped, or just arrived). `ChatKey`
    /// is the sorted participant-UUID set, so ADDING OR REMOVING A GROUP MEMBER
    /// changes it mid-game - after which the strictly-scoped `seat(gameId:
    /// chatKey:)` misses and this device's own seat degrades to §6.2/§6.3 (the
    /// Release spectator board: a seated player suddenly "can't act"). When the
    /// caller holds an actual bubble, the bubble's `game_id` - a `UInt64.random`
    /// minted once at creation - is itself the proof this row is this game's: a
    /// row can only exist because THIS DEVICE created/joined/played that game,
    /// whatever key the conversation hashed to at the time. The cross-chat leak
    /// the scoping fixed lived in the NO-BUBBLE reopen, which has no gameId to
    /// anchor on and keeps using the scoped read. The next `setSeat` re-keys the
    /// row to the current chatKey, so one tap heals the scoped read too.
    public func seatForBubble(gameId: String) -> Int? { allSeats()[gameId]?.seat }

    // MARK: the high-water chain (round 20)

    /// ROUND 20, the owner: "prevent offline players from staging moves. They
    /// might be trying to cheat by holding an older state and branching from it
    /// instead of live game."
    ///
    /// THE NEWEST CHAIN THIS DEVICE HAS SEEN for a game - the mark a freshly
    /// opened bubble is weighed against, so a board built on an OLD bubble can
    /// be recognised as a branch and made read-only (GameSurface.rankAgainst
    /// HighWater). Messages offers no way to enumerate a transcript, so
    /// "is this the latest?" has no answer at all without a note of what has
    /// already been through this device.
    ///
    /// NOT the round-7 payload cache coming back. That one decided WHAT TO
    /// RENDER, and the owner removed it because the extension has to show
    /// exactly the bubble you tapped. This one never renders anything: it
    /// gates staging, and offers a button. Everything §6/§7 promises still
    /// holds if it is lost - a device with no note simply trusts every bubble
    /// it opens, which is precisely the behaviour before this round.
    ///
    /// Keyed by game AND chat, like `seat(gameId:chatKey:)`, and stored as the
    /// raw payload because Rule P is a comparison of whole chains.
    ///
    /// CLEARED WHEN THE GAME ENDS (round 43, owner: "easy - clear when the last
    /// move plays"). `StaleBranchGate.rank` drops the row the moment the chain
    /// it is ranking says FINISHED. A finished board cannot be staged on -
    /// `iCanAct` is false and the legal menu is empty - so the mark gates
    /// nothing there and is pure storage. Without that, this map was the one
    /// thing in this file that only ever grew: every game, in every chat,
    /// forever, each row a whole base64 chain, re-encoded on every write and
    /// re-decoded every time a bubble is opened. In an extension under a hard
    /// memory ceiling that is the shape of a slow degradation.
    ///
    /// v1 -> v2: the row grew from a bare base64 string to `LatestRow`, for the
    /// eviction backstop below. Swift's synthesized Codable THROWS on a shape
    /// change rather than defaulting, so the key is bumped instead of decoded
    /// around - the same call this file already made at `fmsg.games.v1`. Old
    /// rows simply become invisible, which is precisely what this mark is
    /// written to survive (see the paragraph above: a device with no note
    /// trusts every bubble it opens, the behaviour before round 20).
    private let latestKey = "fmsg.latest.v2"

    /// One game's high-water row.
    ///
    /// `updatedAt` is read by the eviction backstop ALONE. Nothing about the
    /// gate's answer depends on it: Rule P and `StaleBranchGate.isAhead` both
    /// read the chains themselves, never a local clock, because a device clock
    /// is not evidence about a game (two devices disagree, and a cheater's
    /// agrees with nobody).
    public struct LatestRow: Codable, Equatable, Sendable {
        public var chain: String            // the payload, base64
        public var updatedAt: TimeInterval  // seconds since 1970; orders eviction only
        public init(chain: String, updatedAt: TimeInterval) {
            self.chain = chain; self.updatedAt = updatedAt
        }
    }

    /// The composite row key.
    ///
    /// `chatKey` is ITSELF a "|"-joined participant set (`ChatKey.make`), so
    /// this string carries many separators - but a `gameId` is `String(UInt64)`
    /// (`MessageEnvelope`'s own note: "a u64: a String because JSON numbers are
    /// doubles"), all digits and never a "|", so the LAST separator is always
    /// the one that splits the pair. That is what lets `forgetLatestChain`
    /// match on the suffix without having to know the chat.
    static func latestRowKey(gameId: String, chatKey: String) -> String { "\(chatKey)|\(gameId)" }

    public func latestChain(gameId: String, chatKey: String) -> Data? {
        guard let row = allLatest()[Self.latestRowKey(gameId: gameId, chatKey: chatKey)]
        else { return nil }
        return Data(base64Encoded: row.chain)
    }

    /// Record `payload` as the newest chain seen for this game. THE CALLER has
    /// already decided it wins (Rule P lives in the kernel, and this type does
    /// no async work) - this only writes what it is told.
    public func setLatestChain(gameId: String, chatKey: String, payload: Data) {
        var map = allLatest()
        map[Self.latestRowKey(gameId: gameId, chatKey: chatKey)] =
            LatestRow(chain: payload.base64EncodedString(),
                      updatedAt: Date().timeIntervalSince1970)
        evictOldestLatestBeyondCap(&map)
        persistLatest(map)
    }

    /// Drop every high-water row for `gameId` - the mirror of `setLatestChain`,
    /// called by `StaleBranchGate.rank` when the chain it is ranking carries a
    /// FINISHED game (the round-43 clear; see `latestKey`'s doc for why a
    /// finished game's mark is dead weight).
    ///
    /// UNSCOPED BY chatKey, on purpose, and for the same two reasons `forgetSeat`
    /// is - plus one this map has and the seats map does not:
    ///
    ///   * the caller is holding the bubble it just ranked, and a `gameId` is a
    ///     `UInt64.random` minted once at creation, so the gameId is itself the
    ///     proof the row is this game's, whatever key the conversation hashed to
    ///     when it was written;
    ///   * ADDING OR REMOVING A GROUP MEMBER RE-KEYS `chatKey` MID-GAME
    ///     (`seatForBubble`'s doc spells this out), and this map is keyed by the
    ///     pair - so a re-key does not REPLACE the row, it writes the same game a
    ///     SECOND time under the new key. A chatKey-scoped delete would clear the
    ///     row for today's roster and leave yesterday's behind with nothing that
    ///     could ever collect it. The seats map cannot accumulate that way
    ///     because it is keyed by gameId alone.
    ///
    /// The cross-chat leak the scoping exists to stop lives in READS (a foreign
    /// row must never be returned, or a stale chain could out-rank a bubble it
    /// was never sealed against). Deleting rows leaks nothing: the worst a wrong
    /// delete could do is drop a mark, and a missing mark is the fail-open state
    /// this whole feature is built to degrade to.
    public func forgetLatestChain(gameId: String) {
        var map = allLatest()
        let suffix = "|\(gameId)"
        let doomed = map.keys.filter { $0.hasSuffix(suffix) }
        guard !doomed.isEmpty else { return }
        for key in doomed { map.removeValue(forKey: key) }
        persistLatest(map)
    }

    // ---- BACKSTOP (round 43), separable from the game-over clear above ----
    //
    // The clear only reclaims games that actually REACH game over on this
    // device. A thread that goes quiet at move 4 and is never opened again
    // never calls it, so abandoned games would still accumulate one whole
    // chain each, forever. This is the same bound `handOrder` already carries
    // (`handOrderCap`, and see its doc: "abandoned games never call the
    // end-of-game clear, so the map could grow forever") applied to the map
    // next door, which was the only difference between the two.
    //
    // What eviction COSTS, stated plainly: dropping a mark re-opens the
    // round-20 hole for that one game - a branch off an old bubble is trusted
    // again. That is why the cap is generous rather than tight, and why the
    // victim is the LEAST RECENTLY WRITTEN row, which is the game least likely
    // to still be running.
    //
    // TO REVERT, leaving the game-over clear intact: delete this comment, the
    // `latestChainCap` constant and `evictOldestLatestBeyondCap` (and its one
    // call in `setLatestChain`); `LatestRow.updatedAt` can stay, unread.

    /// Deliberately the SAME number as `handOrderCap`, so there is one bound to
    /// reason about rather than two. A row here is far bigger than a hand-order
    /// row - a whole sealed chain, hundreds of bytes of base64, against ~36
    /// short card ids - but 32 chains is still tens of kilobytes, and nobody
    /// has 32 live games across all their threads at once.
    static let latestChainCap = 32

    private func evictOldestLatestBeyondCap(_ map: inout [String: LatestRow]) {
        while map.count > Self.latestChainCap,
              let oldest = map.min(by: { $0.value.updatedAt < $1.value.updatedAt }) {
            map.removeValue(forKey: oldest.key)
        }
    }

    // ---- end backstop ----

    private func allLatest() -> [String: LatestRow] {
        guard let data = defaults?.data(forKey: latestKey),
              let map = try? JSONDecoder().decode([String: LatestRow].self, from: data)
        else { return [:] }
        return map
    }

    private func persistLatest(_ map: [String: LatestRow]) {
        guard let data = try? JSONEncoder().encode(map) else { return }
        defaults?.set(data, forKey: latestKey)
    }

    /// ROUND 7: the preferred-chain record is gone; nothing is stored per game but
    /// the seat (`seat(gameId:chatKey:)`). Retained as a no-op returning nil so the
    /// §5/§6/§7 call sites compile and behave as "nothing cached".
    public func record(gameId: String, chatKey: String) -> MessageGameRecord? { nil }

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
    /// astronomically unlikely here as it is for the hand-order rows (§ below,
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

    // ROUND-9 (owner): the durable pending ledger (§7.4 Rule R's safety net) is
    // REMOVED - "caching has caused A LOT of problems in the past. The extension
    // is rarely killed, and it's rare that an arriving bubble can happen mid
    // staging." The staged-but-unsent moves live only in the controller's
    // in-memory `pending` now; the staged input-field bubble itself still
    // carries them (a sealed chain IS the moves), so nothing a human actually
    // sends can be lost - only an un-sent staging dies with the extension,
    // which is the accepted trade. Old `fmsg.pending.v1` blobs are simply
    // never read again.

    // MARK: just sent (round-9 #5) — the chain this device just committed to Send
    //
    // Pressing Send can tear the extension down (didStartSending's dismiss(),
    // which round 16 narrowed to sends made while EXPANDED; a VC swap on the
    // auto-reopen), so the in-memory `lastSentPayload` that
    // keeps StagedBubbleRouting from reloading my own bubble does not always
    // survive to the reopen. When it doesn't, the reopen rebuilds the surface
    // from my own just-sent chain and the open-replay played MY OWN move back
    // at me ("replays the move I just did! Super confusing"). This durable
    // ONE-SHOT marker is the cross-teardown half of that same signal: written
    // synchronously in didStartSending, consumed by the first adopt - a match
    // means "this is the chain I just sent, open it QUIETLY" (no replay).
    // Consumed (cleared) on ANY adopt, match or not, so a stale marker can
    // never suppress a later genuine replay.

    private let justSentKey = "fmsg.justsent.v1"

    /// Record the chain the human just pressed Send on (didStartSending, §7.6).
    public func markJustSent(payload: Data) { defaults?.set(payload, forKey: justSentKey) }

    /// Check at adopt: true iff `payload` is byte-identical to the marked chain,
    /// clearing the marker when it is.
    ///
    /// ROUND 12 #11: it used to clear on EVERY adopt, match or not, to stop a
    /// stale marker silencing a later genuine replay. But the marker is already
    /// byte-exact - it can only ever silence the one chain this device sealed,
    /// which is the one move its owner does not need played back at them - so
    /// clearing on a MISS bought nothing and cost the guarantee: any unrelated
    /// adopt between the send and the reopen (an opponent's bubble, a
    /// loopback delivery of my own send) burned the marker, and the reopen of my
    /// own chain replayed my own move. Superseded instead by the next send,
    /// which overwrites it.
    public func consumeJustSent(matching payload: Data) -> Bool {
        guard let d = defaults?.data(forKey: justSentKey), d == payload else { return false }
        defaults?.removeObject(forKey: justSentKey)
        return true
    }

    /// Drop the marker unspent. THE EXTENSION ACTIVATING IS WHAT CALLS THIS, so
    /// a quiet open is only ever the activation that did the sending.
    ///
    /// Owner, 1.0(27): "when I send a move, close it by swiping down, and open
    /// it up soon after, it doesn't replay the move. I have to then close it
    /// again, and then pressing it again will replay it... as soon as we close
    /// it by swiping down, we should get rid of some state we're holding."
    /// That is this marker exactly - the first reopen spends it, which is why
    /// the second one replays and every one after that does too.
    ///
    /// The marker was made durable in round-9 #5 because a send used to TEAR
    /// THE EXTENSION DOWN, so the reopen it silenced was the one Messages
    /// forced on you a heartbeat later. Round 16 took that teardown away for
    /// the compact drawer ("just keep it collapsed so they can keep playing"),
    /// and what is left is a reopen the player chose - which is a request to
    /// see the bubble, and every other bubble answers it by playing its move.
    /// So the marker keeps its job inside one activation and loses it at the
    /// edge. Burning it on ACTIVATION rather than on the way out is deliberate:
    /// an extension that is jetsammed never gets to run its goodbye, and this
    /// board is jetsammed often enough that the trail has a verdict for it.
    @discardableResult
    public func clearJustSent() -> Bool {
        guard defaults?.data(forKey: justSentKey) != nil else { return false }
        defaults?.removeObject(forKey: justSentKey)
        return true
    }

    // MARK: hand order (round-8 #4) — the sticky arrangement memory, per game
    //
    // The web keeps a client-side "arrangement memory" per game
    // (src/state/clientReconcile.ts reconcileHandMemory/displayedHand): the
    // RENDERED hand is the authoritative hand ordered by that memory, so a
    // player who sorts their cards keeps the sorted order across reloads. On
    // iMessage the same memory lived only in FHandFan's @State ("never
    // persisted or sent anywhere"), so every reopen of a game reset the hand
    // to the kernel's canonical order. This store is the web's memory made
    // durable: card IDENTITIES in display order, keyed by gameId.
    //
    // It is deliberately NOT in the kernel and NOT in the wire: the resident
    // Game is rebuilt from the shared chain on every decode (the canonical
    // hand order every player agrees on), while this is one device's cosmetic
    // preference - putting it in the chain would leak a hand-order signal to
    // opponents and fork digests over a sort. Same trust level as the seats
    // row: losing it costs a convenience, never correctness.
    //
    // reasoning). Rows are cleared when the game ends
    // (MessageTurnController.begin on a finished chain / markSent on the final
    // move); the cap below bounds what abandoned games can leak.

    /// One game's stored arrangement. `updatedAt` orders eviction only.
    public struct HandOrderRow: Codable, Equatable, Sendable {
        public var order: [String]
        public var updatedAt: TimeInterval
        public init(order: [String], updatedAt: TimeInterval) {
            self.order = order; self.updatedAt = updatedAt
        }
    }

    /// Abandoned games never call the end-of-game clear, so the map could grow
    /// forever; at 32 games x ~36 card ids it is still tiny, and the oldest row
    /// beyond the cap is evicted on write.
    static let handOrderCap = 32

    /// The stored arrangement for `gameId` — card identities in display order —
    /// or empty when none was ever saved (kernel order applies).
    public func handOrder(gameId: String) -> [String] {
        allHandOrders()[gameId]?.order ?? []
    }

    /// Persist `order` as `gameId`'s arrangement. Empty clears the row.
    ///
    /// ROUND 30: the fan reports GROWTH as well as reorders (FHandFan.remembering),
    /// so a row exists from the first hand drawn rather than only once somebody
    /// has dragged a card. That is the point - an arrangement nobody has
    /// rearranged still has to be an arrangement, or the board falls back to
    /// whatever array order it was handed, and two derivations of the same game
    /// do not agree about that.
    public func setHandOrder(_ order: [String], gameId: String) {
        var map = allHandOrders()
        if order.isEmpty { guard map.removeValue(forKey: gameId) != nil else { return } }
        else {
            map[gameId] = HandOrderRow(order: order, updatedAt: Date().timeIntervalSince1970)
            while map.count > Self.handOrderCap,
                  let oldest = map.min(by: { $0.value.updatedAt < $1.value.updatedAt }) {
                map.removeValue(forKey: oldest.key)
            }
        }
        persistHandOrders(map)
    }

    /// Drop `gameId`'s arrangement — called when the game ends (§ the section
    /// doc above): a finished game's hand no longer needs a preferred order.
    public func clearHandOrder(gameId: String) { setHandOrder([], gameId: gameId) }

    private func allHandOrders() -> [String: HandOrderRow] {
        guard let data = defaults?.data(forKey: handOrderKey),
              let map = try? JSONDecoder().decode([String: HandOrderRow].self, from: data)
        else { return [:] }
        return map
    }

    private func persistHandOrders(_ map: [String: HandOrderRow]) {
        guard let data = try? JSONEncoder().encode(map) else { return }
        defaults?.set(data, forKey: handOrderKey)
    }

    // MARK: storage (a corrupt blob is treated as empty, never thrown)
    //
    // ROUND 7: the preferred-chain game-record cache (Rule P) is removed. `all()`
    // reports empty and `persist` is a no-op, so `record`/`games`/`put`/`remove`
    // are inert — the router always renders the tapped bubble, and seat identity
    // moved to its own `fmsg.seats.v1` store (`seat`/`setSeat` above).
    private func all() -> [String: MessageGameRecord] { [:] }
    private func persist(_ map: [String: MessageGameRecord]) {}
}
