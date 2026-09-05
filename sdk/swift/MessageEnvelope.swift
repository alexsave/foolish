// MessageEnvelope — the Swift face of FMSG (c/src/msg_wire.h).
//
// PURE MARSHALLING. Every question this type looks like it answers — which chain
// wins, whether a pending move survives, what is legal now — is answered in C
// and merely relayed here. That is not tidiness: Rule P decides which game every
// player SEES, so a phone disagreeing with a browser forks the game mid-bout.
// There is exactly one implementation, in msg_wire.c, and the same fixtures pin
// it through wasm (e2e/msg_wire.test.ts) and through libfoolish.a
// (FoolishTests/MessageEnvelopeTests). The M3 plan's Swift port of the
// concurrency model is cancelled — there is nothing to port.
//
// Base32 + the '1' text-version prefix are the URL layer (§4.3), and they live
// in Swift only because a URL is a Swift thing; the bytes underneath are the
// kernel's.
import Foundation
import CFoolish

public struct MessageJoin: Codable, Sendable, Equatable {
    public let seat: Int
    public let name: String
    public init(seat: Int, name: String) { self.seat = seat; self.name = name }
}

/// A decoded, VALIDATED payload. Holding one means the chain replayed cleanly
/// through the kernel and the game it describes is now the resident one.
public struct MessageEnvelope: Codable, Sendable, Equatable {
    public let phase: Int              // 0 WAITING · 1 ACCEPT · 2 LIVE · 3 FINISHED
    public let turn: Int               // atoms applied — Rule P's second key
    public let round: Int              // completed bouts — Rule P's first key
    public let nPlayers: Int
    public let lastActorSeat: Int
    public let gameId: String          // a u64: a String because JSON numbers are doubles
    public let parent8: String         // hex
    public let digest: String          // hex — SHA-256 of the payload; Rule P's tiebreak
    /// ROUND 16 — the SEND CLOCK: unix seconds mod 65536 as the sending device
    /// read them, or 0 for a chain that carries none (format 2, i.e. anything
    /// sealed by 1.0(15) or earlier). Feeds one rule and only one:
    /// `MessageKernel.pickupHold`, the 15-second wait a defender owes a fresh
    /// attack. 0 means no wait, which is exactly what makes an old bubble play.
    public let sentAt: Int
    /// ROUND 16 - the BUBBLE DELTA: how many atoms this bubble added to the
    /// chain, or 0 for a chain that does not say (format 2, or a delta that did
    /// not fit its byte). It is what makes "replay the move I just opened"
    /// exact: without it the kernel can only guess where the previous bubble
    /// ended, and the guess replays a cover twice when its sender sent one
    /// cover per bubble. Read through `atomsBefore`, which is the form the
    /// kernel takes. See c/src/msg_wire.h's n_new.
    ///
    /// `newAtomsNothing` is the third state: this bubble added NO atoms. It is
    /// not 0 because 0 asks the reader to guess, and there is nothing here to
    /// guess at - see `atomsBefore`.
    public let newAtoms: Int
    /// c/src/msg_wire.h's MSG_NEW_NOTHING.
    public static let newAtomsNothing = 255

    /// THE FOOL'S PENALTY (c/src/msg_wire.h format 4). `opening` is the seat
    /// this game was DEALT to open on, or nil for the ordinary lowest-trump
    /// rule - non-nil means a rematch punished the last game's fool, and that
    /// seat's left-hand neighbour (the fool) is the first defender.
    public let opening: Int?
    /// A WAITING lobby's rematch carry: the roster key of the table this lobby
    /// was created for, and the fool's canonical index within it. Both nil on
    /// an ordinary lobby. They are the QUESTION the kernel answers at Start -
    /// Swift never reads them apart, only hands the pair back.
    public let carryKey: UInt32?
    public let carryFool: Int?

    /// THE TABLE'S RULES: may the defender transfer the attack on (perevodnoy,
    /// true - the default and what every game before this variant played), or
    /// is this the throw-in game with no transfer at all (podkidnoy, false)?
    ///
    /// Chosen in the LOBBY, carried by every bubble of the game that follows,
    /// and enforced in the kernel: a podkidnoy defender's legal moves simply do
    /// not include a transfer, so nothing on a board has to remember to hide
    /// one. Read it to draw the lobby's checkbox and the rules a player is
    /// shown - never to decide what is legal.
    public let passingAllowed: Bool

    /// Does this lobby owe someone a penalty? Purely for the lobby's own copy -
    /// whether the rule actually FIRES is decided at Start, in C, against the
    /// roster that is really starting.
    public var carriesPenalty: Bool { carryKey != nil && carryFool != nil }

    /// How many atoms sat on this chain BEFORE this bubble - the boundary
    /// `MessageKernel.lastMoveEvents` groups on, and -1 when the bubble does
    /// not say (the kernel then falls back to its own guess).
    ///
    /// A bubble that added NOTHING answers `turn`: the chain already ended
    /// where this body ends, so the suffix to animate is empty and the kernel
    /// says so rather than guessing (fio_replay_last_events_packed clamps the
    /// start to the atom count and returns no events). That is the whole of
    /// round 16's undo-then-send report - cancelling a staged move used to hand
    /// every recipient the PREVIOUS player's move to replay again.
    public var atomsBefore: Int {
        if newAtoms == Self.newAtomsNothing { return turn }
        return newAtoms > 0 ? turn - newAtoms : -1
    }

    /// This bubble carries NO move of its own - the re-seal a cancelled stage
    /// leaves behind (§10: Messages has no API to withdraw a bubble already in
    /// the input field, so an undo overwrites it with the board as it stands).
    ///
    /// The animation already reads this through `atomsBefore`. The CAPTION has
    /// to ask it separately, because "no events" is ambiguous on its own: a bare
    /// `good` produces an empty stream too, and telling the two apart by the
    /// stream alone is how a defender - who may never say good at all - was
    /// announced as having declared done.
    public var addedNothing: Bool { newAtoms == Self.newAtomsNothing }
    public let joins: [MessageJoin]

    enum CodingKeys: String, CodingKey {
        case phase, turn, round, joins, digest, parent8, passingAllowed
        case sentAt = "sent_at"
        case newAtoms = "n_new"
        case opening, carryKey, carryFool
        case nPlayers = "n_players"
        case lastActorSeat = "last_actor_seat"
        case gameId = "game_id"
    }

    public enum Failure: Error, Equatable {
        case notAFoolishLink        // wrong scheme/format version — not ours
        case damaged(code: Int)     // MSG_E* — the payload does not replay
        case rejected(reason: Int)  // a move the kernel refused; reason is an
                                    // ENGINE_REJECT_* code (fio_last_reject),
                                    // mapped to a human line by FStrings.rejectReason
        /// ROUND 20: the seal was about to describe a DIFFERENT GAME than the
        /// caller meant. The kernel keeps ONE resident game and `fio_msg_encode`
        /// seals whatever is in it, so a seal is only ever as correct as the
        /// caller's belief about what is resident - and that belief has been
        /// wrong in the field. See `seal(expectPlayers:)`.
        case sealMismatch(expected: Int, resident: Int)
    }

    /// The URL a bubble carries: https://foolish.cards/m/1<base32>. The leading
    /// '1' is the TEXT-level format version, so a link can be rejected before a
    /// single binary byte is decoded (§4.3).
    public static func payloadBytes(url: URL) throws -> Data {
        let seg = url.pathComponents.last ?? ""
        guard seg.first == "1" else { throw Failure.notAFoolishLink }
        guard let bytes = Base32.decode(String(seg.dropFirst())), !bytes.isEmpty else {
            throw Failure.notAFoolishLink
        }
        return bytes
    }

    /// The bubble's link for a payload: https://foolish.cards/m/1<base32>. The
    /// inverse of `payloadBytes` — the '1' is the §4.3 text-format version. Kept
    /// pure (no MSMessage) so the URL layer is testable without Apple's framework.
    public static func link(payload: Data) -> URL {
        URL(string: "https://foolish.cards/m/1" + Base32.encode(payload))!
    }

    /// The FINISHED bubble's link (§12): the shareable REPLAY code, not `/m/`, so
    /// a tap lands on the web replay page (Infinite Oracle) on any platform — the
    /// ecosystem funnel. `code` is `fio_replay_share_code_b32`'s output.
    public static func replayLink(code: String) -> URL {
        URL(string: "https://foolish.cards/" + code)!
    }

    /// …with the table's NICKNAMES attached, so the web replay shows "Sveta" and
    /// "Misha" instead of "P1" and "P2" (ReplayExtras). `names` is seat-ordered
    /// and must be exactly as long as the game has players - the reader counts
    /// seats from the decoded moves, not from this blob.
    ///
    /// It is one function and not a mutation of `replayLink` because the names
    /// are NOT the kernel's to give: the code comes out of
    /// fio_replay_share_code_b32, which knows the cards and not the people, and
    /// the roster comes off the FMSG chain's joins. Joining them is a URL
    /// concern, and the URL layer is the one thing here that is Swift's.
    public static func replayLink(code: String, names: [String]) -> URL {
        replayLink(code: ReplayExtras.code(moves: code, names: names))
    }

    /// Decode + validate + ADOPT: the chain is replayed through the kernel, so
    /// afterwards the engine's resident game IS this payload's game.
    public static func decode(url: URL, viewer: Int) async throws -> MessageEnvelope {
        try await decode(payload: try payloadBytes(url: url), viewer: viewer)
    }

    public static func decode(payload: Data, viewer: Int) async throws -> MessageEnvelope {
        try await MessageKernel.shared.decode(payload: payload, viewer: viewer)
    }

    /// READ a payload's header and adopt NOTHING - the engine is left exactly as
    /// it was, resident game and all.
    ///
    /// For a caller that only wants the fields: the composer describing the
    /// bubble it has just sealed (its joins, its summary line, its game id).
    /// That read used to be a `decode`, and a decode is an ADOPTION - it told
    /// the kernel the chain up to and including the staged move was history
    /// somebody else made, so the next action of the same turn measured its
    /// delta from the middle of its own bubble. See `atomsBefore`, and
    /// fio_msg_peek_packed for the whole of it.
    ///
    /// Nothing here replays, so nothing here validates: the fields are the
    /// sender's claims. Peek what you are about to SEND; decode what is about
    /// to be PLAYED.
    public static func peek(payload: Data) async throws -> MessageEnvelope {
        try await MessageKernel.shared.peek(payload: payload)
    }

    /// Parse the kernel's packed envelope-metadata blob (fio_msg_decode_packed).
    /// Fixed layout: phase(1) n_players(1) last_actor_seat(1) round(1) turn(u16
    /// LE) game_id(u64 LE) parent8(8) digest(32) sent_at(u16 LE) n_new(1)
    /// opening(1) carry_key(u32 LE) carry_fool(1) passing(1) n_joins(1) then
    /// joins of {seat(1) name_len(1) name[]}. Returns nil if a field runs past
    /// the end.
    ///
    /// ROUND 16 grew this by the two sent_at bytes and the n_new byte, then by
    /// the fool's-penalty trio; the rules byte followed. All of them land AFTER
    /// the digest, so every offset above is the one it always was.
    static func decode(packed d: Data) -> MessageEnvelope? {
        let b = [UInt8](d)
        let HDR = 65
        guard b.count >= HDR else { return nil }
        let phase = Int(b[0]); let nPlayers = Int(b[1]); let last = Int(b[2]); let round = Int(b[3])
        let turn = Int(b[4]) | (Int(b[5]) << 8)
        var gid: UInt64 = 0
        for i in 0..<8 { gid |= UInt64(b[6 + i]) << (8 * i) }
        let hex = { (r: Range<Int>) in b[r].map { String(format: "%02x", $0) }.joined() }
        let parent8 = hex(14..<22)
        let digest = hex(22..<54)
        let sentAt = Int(b[54]) | (Int(b[55]) << 8)
        let newAtoms = Int(b[56])
        // 0xFF is the wire's "no penalty here" on both of these (MSG_NO_OPENING
        // / MSG_NO_FOOL), and a 0 key is "no carry".
        let opening: Int? = b[57] == 0xFF ? nil : Int(b[57])
        var rawKey: UInt32 = 0
        for i in 0..<4 { rawKey |= UInt32(b[58 + i]) << (8 * i) }
        let carryKey: UInt32? = rawKey == 0 ? nil : rawKey
        let carryFool: Int? = b[62] == 0xFF ? nil : Int(b[62])
        // The rules, already resolved against the envelope's format by the
        // kernel (msg_pass_allowed): Swift never learns which formats carry a
        // variant byte.
        let passing = b[63] != 0
        let nJoins = Int(b[64])
        var joins: [MessageJoin] = []
        var q = HDR
        for _ in 0..<nJoins {
            guard q + 2 <= b.count else { return nil }
            let seat = Int(b[q]); let nl = Int(b[q + 1]); q += 2
            guard q + nl <= b.count else { return nil }
            let name = String(decoding: b[q..<q + nl], as: UTF8.self); q += nl
            joins.append(MessageJoin(seat: seat, name: name))
        }
        return MessageEnvelope(phase: phase, turn: turn, round: round, nPlayers: nPlayers,
                               lastActorSeat: last, gameId: String(gid),
                               parent8: parent8, digest: digest, sentAt: sentAt,
                               newAtoms: newAtoms, opening: opening,
                               carryKey: carryKey, carryFool: carryFool,
                               passingAllowed: passing, joins: joins)
    }
}

/// Serialized access to the kernel's single static Game, same discipline as
/// EngineC (ios_api.h: not reentrant).
public actor MessageKernel {
    public static let shared = MessageKernel()
    private init() {}

    /// Decode + validate + ADOPT through the PACKED envelope wire — the metadata
    /// crosses as a fixed-layout blob, no JSON. The view is read separately
    /// (residentView) in this same actor. `viewer` no longer rides the decode
    /// (metadata is viewer-independent); it stays in the signature for the
    /// call sites that pass a seat, and is applied on the residentView read.
    public func decode(payload: Data, viewer: Int) throws -> MessageEnvelope {
        var out = [UInt8](repeating: 0, count: 4 * 1024)
        let n: Int32 = payload.withUnsafeBytes { raw in
            fio_msg_decode_packed(raw.bindMemory(to: UInt8.self).baseAddress,
                                  Int32(payload.count), &out, Int32(out.count))
        }
        guard n > 0 else { throw MessageEnvelope.Failure.damaged(code: Int(fio_last_msg_error())) }
        guard let env = MessageEnvelope.decode(packed: Data(out.prefix(Int(n)))) else {
            throw MessageEnvelope.Failure.damaged(code: -1)
        }
        return env
    }


    /// The same packed blob as `decode`, WITHOUT the replay: nothing about the
    /// resident game moves, the bubble-delta base least of all. See
    /// `MessageEnvelope.peek`.
    public func peek(payload: Data) throws -> MessageEnvelope {
        var out = [UInt8](repeating: 0, count: 4 * 1024)
        let n: Int32 = payload.withUnsafeBytes { raw in
            fio_msg_peek_packed(raw.bindMemory(to: UInt8.self).baseAddress,
                                Int32(payload.count), &out, Int32(out.count))
        }
        guard n > 0 else { throw MessageEnvelope.Failure.damaged(code: Int(fio_last_msg_error())) }
        guard let env = MessageEnvelope.decode(packed: Data(out.prefix(Int(n)))) else {
            throw MessageEnvelope.Failure.damaged(code: -1)
        }
        return env
    }

    /// 1.0(6) DIAGNOSTIC: the replay codec version (5/6/7) of the body the last
    /// `decode` replayed, or -1 for an empty-body (lobby/handoff) message.
    public func lastBodyVersion() -> Int { Int(fio_msg_last_body_version()) }

    /// 1.0(6) DIAGNOSTIC: decode AND read the body version in ONE actor hop, so a
    /// concurrent decode (e.g. the board's begin()) can't clobber the shared
    /// version global between the two reads.
    public func decodeWithBodyVersion(payload: Data) throws -> (MessageEnvelope, Int) {
        let env = try decode(payload: payload, viewer: -1)
        return (env, Int(fio_msg_last_body_version()))
    }

    /// The masked board the last `decode` left resident, for `viewer` (or -1 for
    /// the public/spectator view the bubble snapshot needs). Same packed wire the
    /// app reads — decoded in this actor so it never races EngineC on the shared
    /// static Game. Returns nil if no game is resident or the buffer won't fit.
    public func residentView(viewer: Int) -> GameView? {
        guard let data = packedCall({ fio_state_packed(Int32(viewer), $0, $1) }) else { return nil }
        return MaskedView.decode(data, viewer: viewer)
    }

    /// The legal moves for `seat` on the resident game (kernel-computed), as the
    /// kernel's own bytes. The board's play rules take the menu as an input
    /// (PlayWire / fio_play_probe), so this is the form that travels.
    public func residentLegalPacked(seat: Int) -> Data {
        packedCall({ fio_legal_packed(Int32(seat), $0, $1) }) ?? MoveWire.emptyMenu
    }

    /// The same menu decoded. Empty on no game or none legal.
    public func residentLegal(seat: Int) -> [Move] {
        MoveWire.decode(residentLegalPacked(seat: seat))
    }

    /// The moves a HUMAN may make at `seat` on the resident game - the kernel
    /// menu narrowed by fio_play_human_menu (no `wait`, no `good` over an
    /// uncovered attack). ONE actor call, so the menu and the table it is
    /// narrowed against cannot come from two different chains.
    public func residentHumanMoves(seat: Int) -> [Move] {
        guard let view = residentView(viewer: seat) else { return [] }
        return PlayWire.humanMoves(menu: residentLegalPacked(seat: seat), battles: view.battles)
    }

    /// Call a packed-bytes-emitting C function into a growing buffer (mirrors
    /// EngineC's helper; FIO_ECAP == -3 doubles and retries).
    private func packedCall(_ call: (UnsafeMutablePointer<CChar>, Int32) -> Int32) -> Data? {
        var cap = 8 * 1024
        while true {
            var buf = [CChar](repeating: 0, count: cap)
            let n = call(&buf, Int32(cap))
            if n >= 0 { return Data(bytes: buf, count: Int(n)) }
            if n == -3 { cap *= 2; if cap > (1 << 20) { return nil }; continue }
            return nil
        }
    }

    /// Deal a fresh game as the resident one — the start of the send path (a new
    /// invite). `seed` MUST be 32 bytes (the wide ChaCha deal both devices
    /// reproduce); `seal` then reads that seed into the envelope header, so the
    /// recipient re-deals the identical stock. Throws on a bad seed / seat count.
    public func newGame(seed: Data, players: Int) throws {
        let rc = seed.withUnsafeBytes { raw -> Int32 in
            fio_new_game(raw.bindMemory(to: UInt8.self).baseAddress, Int32(seed.count), Int32(players))
        }
        guard rc == 0 else { throw MessageEnvelope.Failure.damaged(code: Int(rc)) }
    }

    /// Re-deal the RESIDENT game's own LOCKED seed at a different player count —
    /// the group lobby's Start action (docs/IMESSAGE_LOBBY_V2.md): a lobby is
    /// created OPEN (`newGame(seed:, players: 8)`, the wire's max capacity) so
    /// seats stay free; Start re-derives the SAME seed's deal at the ACTUAL
    /// joined count (never a new random seed — that is the "locked at create"
    /// guarantee). The seed itself never crosses back into Swift: the kernel
    /// already holds it resident from whichever call last dealt or decoded it
    /// (`newGame` or `decode`), the same "kernel keeps the seed" discipline
    /// `residentReplayCode` already relies on. Throws FIO_ENOSEED if nothing is
    /// resident to re-derive from (only reachable by calling this out of order —
    /// every real lobby is created wide-seeded) or a bad player count.
    public func reseatResidentGame(players: Int) throws {
        let rc = fio_reseat_game(Int32(players))
        guard rc == 0 else { throw MessageEnvelope.Failure.damaged(code: Int(rc)) }
    }

    // ---------- the table's rules ------------------------------------------

    /// Set whether the defender may TRANSFER (perevodnoy, the default) or not
    /// (podkidnoy) - the lobby's passing checkbox, and the only place this is
    /// ever chosen.
    ///
    /// Call it on the resident game and then seal: the WAITING bubble states
    /// the rules for everyone else (a lobby has no body to carry them in), and
    /// the Start that re-derives the locked seed carries them across. It is a
    /// term of the TABLE, not a display option - the kernel stops offering a
    /// transfer at all - so it belongs to a lobby and never to a live board.
    public func setPassing(_ allowed: Bool) {
        _ = fio_set_passing(allowed ? 1 : 0)
    }

    /// The resident game's rules, the same way round. True when nothing has
    /// said otherwise.
    public func passingAllowed() -> Bool { fio_passing_allowed() != 0 }

    // ---------- Rule F: the fool's penalty ---------------------------------
    //
    // A rematch among the SAME players, in the same cycle, opens on the seat to
    // the RIGHT of the last game's fool - so the fool is the first player
    // attacked. Both halves are C (msg_wire.c's msg_roster_key /
    // msg_rematch_opening); these three relay, and decide nothing. In
    // particular Swift never compares two rosters and never works out which
    // seat is "right of" anyone: get either wrong on one device and that device
    // has dealt a different game.

    /// CREATING the rematch lobby: turn its roster and the fool's seat WITHIN
    /// that roster into the carry a WAITING envelope hands forward, and arm the
    /// resident game with it so the next `seal` writes it. Returns false if the
    /// kernel would not take the pair (which simply means no penalty rides).
    @discardableResult
    public func armRematchCarry(joins: [MessageJoin], foolSeat: Int) -> Bool {
        let joinsJSON = (try? JSONEncoder().encode(joins))
            .flatMap { String(data: $0, encoding: .utf8) } ?? "[]"
        var key: UInt32 = 0
        var idx: Int32 = 0
        let rc = joinsJSON.withCString { jp in fio_msg_carry(jp, Int32(foolSeat), &key, &idx) }
        guard rc == 0, key != 0 else { fio_msg_set_carry(0, -1); return false }
        return fio_msg_set_carry(key, idx) == 0
    }

    /// SHOWING it: which seat a lobby's pending penalty would fall on - the
    /// fool, who becomes the new game's first defender - or nil if the rule
    /// would not apply to this roster. Read-only: it deals nothing, so a lobby
    /// may ask on every render.
    public func penaltyFoolSeat(joins: [MessageJoin],
                                carryKey: UInt32, carryFool: Int) -> Int? {
        let joinsJSON = (try? JSONEncoder().encode(joins))
            .flatMap { String(data: $0, encoding: .utf8) } ?? "[]"
        let s = joinsJSON.withCString { jp in
            fio_msg_penalty_fool_seat(jp, carryKey, Int32(carryFool))
        }
        return s >= 0 ? Int(s) : nil
    }

    /// STARTING it: deal the resident locked seed at `joins.count`, applying the
    /// penalty if - and only if - that roster still keys equal to the carry the
    /// lobby was created with. Returns the seat the game opens on, or nil when
    /// the rule did not apply and the deal derived its opener as usual.
    ///
    /// Replaces `reseatResidentGame` on the rematch path and does everything it
    /// does, so the two Start routes stay one primitive.
    @discardableResult
    public func startRematchDeal(joins: [MessageJoin],
                                 carryKey: UInt32, carryFool: Int) throws -> Int? {
        let joinsJSON = (try? JSONEncoder().encode(joins))
            .flatMap { String(data: $0, encoding: .utf8) } ?? "[]"
        var opening: Int32 = -1
        let rc = joinsJSON.withCString { jp in
            fio_msg_start_rematch(jp, carryKey, Int32(carryFool), &opening)
        }
        guard rc == 0 else { throw MessageEnvelope.Failure.damaged(code: Int(rc)) }
        return opening >= 0 ? Int(opening) : nil
    }

    /// Reseat the LOCKED seed at `joins.count` and seal a LIVE handoff — the
    /// one primitive lobby v3's two Start routes share (docs note 2): any
    /// already-joined player tapping Start after a plain Join, or a fresh
    /// joiner tapping "Join and start" in one step. Both call this with the
    /// same `lobbyPayload` (a chain carrying the locked seed) and the same
    /// FINAL `joins` list, so they are PROVABLY the same deal — it depends
    /// only on the seed already resident on `lobbyPayload` and `joins.count`,
    /// never on which UI path assembled `joins` or when it was sealed as its
    /// own WAITING bubble (MessageLobbyTests proves both routes agree).
    ///
    /// `sentAt` is round 16's send clock, defaulting to this device's; a fixture
    /// that needs REPRODUCIBLE BYTES passes a constant, because a clock in the
    /// envelope means the digest (Rule P's tiebreak) moves with the second the
    /// seal happened.
    public func startFromLobby(lobbyPayload: Data, gameId: UInt64, actingSeat: Int,
                               parent8: Data, joins: [MessageJoin],
                               sentAt: Int = MessageKernel.clockNow()) throws -> Data {
        let lobby = try decode(payload: lobbyPayload, viewer: -1)
        // THE FOOL'S PENALTY rides the lobby, so honouring it is part of Start
        // rather than a separate route: a lobby that carries one deals through
        // the kernel's rematch entry (which applies the rule or declines it,
        // against the roster REALLY starting), and one that does not deals
        // exactly as it always has. Both Start routes inherit this for free,
        // which is the point of them sharing this primitive.
        if let key = lobby.carryKey, let fool = lobby.carryFool {
            _ = try startRematchDeal(joins: joins, carryKey: key, carryFool: fool)
        } else {
            try reseatResidentGame(players: joins.count)
        }
        return try seal(phase: 2, lastActorSeat: actingSeat, gameId: gameId,
                        parent8: parent8, joins: joins, sentAt: sentAt)
    }

    /// ROUND 21: MOVE A WAITING LOBBY'S RULES AND RESEAL IT - IN ONE ACTOR CALL.
    ///
    /// The passing checkbox used to reach through this actor three separate
    /// times (`decode`, `setPassing`, `seal`) with the caller suspended between
    /// each pair. That is the same shape as the phantom-seal bug `resealFromBase`
    /// exists to close, and for the same reason: decoding IS adopting, so any
    /// bubble snapshot or Rule-P comparison landing in one of those gaps repoints
    /// the resident game and the seal below describes IT instead. Two taps on the
    /// box in quick succession could do it without any help from elsewhere - two
    /// of these sequences interleaving would decode, decode, set, set, seal,
    /// seal, and the second seal would carry the first tap's rule.
    ///
    /// One call, no suspension point, and the lobby that gets sealed is provably
    /// the one whose bytes were just decoded. No `expectPlayers` backstop here:
    /// a lobby seal is the case that guard documents itself as skipping, and
    /// there is no longer a window for it to catch.
    ///
    /// Phase 0 - a lobby stays a lobby; changing a rule is not starting a game.
    public func resealLobby(_ lobbyPayload: Data, passing: Bool, actingSeat: Int,
                            gameId: UInt64, parent8: Data, joins: [MessageJoin],
                            sentAt: Int = MessageKernel.clockNow()) throws -> Data {
        _ = try decode(payload: lobbyPayload, viewer: -1)
        setPassing(passing)
        return try seal(phase: 0, lastActorSeat: actingSeat, gameId: gameId,
                        parent8: parent8, joins: joins, sentAt: sentAt)
    }

    /// Apply one action by `seat` to the resident (adopted) game — the LOCAL half
    /// of a turn, before `seal`. Same packed awire frame the app and server apply
    /// through, and the kernel is the only judge of legality: an illegal move
    /// throws and the resident game is untouched. Kept in THIS actor so a message
    /// turn never races EngineC on the shared static Game.
    public func apply(seat: Int, move: Move) throws {
        let awire = MoveWire.encodeAction(move)
        guard !awire.isEmpty else { throw MessageEnvelope.Failure.damaged(code: -1) }
        let rc = awire.withUnsafeBytes { raw in
            fio_apply_awire(Int32(seat), raw.bindMemory(to: UInt8.self).baseAddress, Int32(awire.count))
        }
        guard rc == 0 else {
            // An illegal move (FIO_EREJECT) carries a specific ENGINE_REJECT_*
            // reason; surface it so the board can say WHY, not just "no". Every
            // other rc is a genuine decode/marshalling fault.
            if rc == FIO_EREJECT { throw MessageEnvelope.Failure.rejected(reason: Int(fio_last_reject())) }
            throw MessageEnvelope.Failure.damaged(code: Int(rc))
        }
    }

    /// Seal the resident game — the send path, after the local player moved.
    /// The kernel derives turn/round from the body it writes, so a device cannot
    /// emit a payload it would itself reject. Seals every phase: a 0-action game (a
    /// WAITING lobby, or the last-joiner LIVE handoff, §5.2) seals to an empty body
    /// - the deal alone is the state - which msg_seal handles, so lobby creation
    /// and joins use this same entry.
    ///
    /// ROUND 16: every seal STAMPS the send clock (`Self.clockNow`), which is
    /// what makes it a format-3 envelope and what the receiving defender's
    /// 15-second pickup hold measures from. Passing 0 would seal the old
    /// format; nothing does, because a bubble with no clock silently disables
    /// the hold for whoever receives it.
    /// ROUND 20 - `expectPlayers`: HOW MANY SEATS THE CALLER BELIEVES IT IS
    /// SEALING. Checked against the resident game before a byte is written, and
    /// a disagreement throws `sealMismatch` instead of emitting the bubble.
    ///
    /// This exists because a seal has no other way to be sure. `fio_msg_encode`
    /// takes `n_players`, the body, the seed and the carry key from the resident
    /// statics (`ios_api.c`: `e.n_players = g_game.num_players`), so it will
    /// faithfully describe whatever game happens to be loaded - and the resident
    /// game does not stay put. Every bubble snapshot, every Rule-P comparison
    /// and every tap DECODES into the same kernel and re-points it. A rematch is
    /// the moment that bites: a WAITING lobby dealt at capacity 8 sits in the
    /// thread beside a live board, and a stage that seals between the two emits
    /// the LOBBY'S untouched 8-player deal wearing the BOARD'S roster - eight
    /// hands of six, deck 4, and every unjoined seat reading "Seat N" on
    /// everybody's screen. Worse, it names the live chain as its parent, so Rule
    /// P's child rule makes the phantom outrank the real game on every device.
    ///
    /// A player count is a small check and it is the RIGHT one: it is fixed for
    /// the life of a chain, so it can never false-positive, and it is exactly
    /// what a foreign deal gets wrong. `nil` skips it, for the lobby seals that
    /// legitimately have no game of their own to compare against.
    ///
    /// It is a backstop, not the fix: `resealFromBase` is what removes the
    /// window rather than detecting it.
    public func seal(phase: Int, lastActorSeat: Int, gameId: UInt64,
                    parent8: Data, joins: [MessageJoin],
                    sentAt: Int = MessageKernel.clockNow(),
                    expectPlayers: Int? = nil) throws -> Data {
        if let expectPlayers {
            // Read through the ordinary view rather than a new C accessor: a
            // seal happens once per bubble, so the marshalling costs nothing
            // that matters, and this keeps the guard inside the Swift layer it
            // is guarding.
            let resident = residentView(viewer: -1)?.players.count ?? -1
            guard resident == expectPlayers else {
                throw MessageEnvelope.Failure.sealMismatch(expected: expectPlayers,
                                                           resident: resident)
            }
        }
        let joinsJSON = String(data: try JSONEncoder().encode(joins), encoding: .utf8) ?? "[]"
        var parent = [UInt8](repeating: 0, count: 8)
        parent.replaceSubrange(0..<min(8, parent8.count), with: parent8.prefix(8))
        var out = [UInt8](repeating: 0, count: 8 * 1024)
        let n = joinsJSON.withCString { jp in
            fio_msg_encode(Int32(phase), Int32(lastActorSeat), gameId, parent, jp,
                           Int32(sentAt & 0xffff), &out, Int32(out.count))
        }
        guard n > 0 else { throw MessageEnvelope.Failure.damaged(code: Int(fio_last_msg_error())) }
        return Data(bytes: out, count: Int(n))
    }

    /// WHAT A CHAIN IS BUILT ON - enough to rebuild it from bytes this device
    /// already holds, which is the whole reason undo is free (§10) and now also
    /// the reason a seal can be made honest.
    public enum SealBase: Sendable {
        case continuation(payload: Data)        // re-adopt this chain
        case genesis(seed: Data, players: Int)  // re-deal this game
    }

    /// One board, read in one breath - see `readBoard` for why that matters.
    public struct BoardRead: Sendable {
        public let view: GameView?
        /// The seat's menu as the KERNEL's bytes. Not a decode: a board hands
        /// these on to the play rules (PlayWire / fio_play_probe), and its own
        /// [Move] view of them is derived once, downstream.
        public let legalPacked: Data
        public let stagedAtomsBefore: Int
        /// Seconds the seat must still wait before it may pick up; 0 = now.
        public let hold: Int
        /// The §12 replay code, only once the game is over.
        public let replayCode: String?
    }

    /// ROUND 20: RE-ESTABLISH, REPLAY, AND SEAL - IN ONE ACTOR CALL.
    ///
    /// The bug this closes: `MessageTurnController.stagedPayload` used to call
    /// `seal` on its own, trusting that the resident game was still the one it
    /// had been playing. Between the move and the seal the board makes four or
    /// more separate hops through this actor (apply, two settlement captures, a
    /// publish), and ANY tap, reload or bubble snapshot in that gap decodes a
    /// different chain into the same kernel - decoding IS adopting. A rematch
    /// puts a WAITING lobby dealt at capacity 8 in the thread next to a live
    /// board, so the gap is not theoretical: it sealed the lobby's 8-player deal
    /// as a LIVE bubble wearing the board's roster, and Rule P's child rule then
    /// spread that phantom to every device in the chat.
    ///
    /// Doing it here makes the whole sequence ONE await from the caller's side,
    /// so there is no suspension point for a foreign decode to land in. The
    /// chain is rebuilt from `base` (bytes this device holds), `moves` are
    /// replayed onto it, and only then is the bubble written - from a game that
    /// is provably the one those moves were made on, because it was just built
    /// from them.
    ///
    /// The PHASE is decided here too, after the replay, for the same reason: it
    /// is a fact about the rebuilt game ("did my move end it"), and asking the
    /// caller to have computed it earlier is asking it to trust a reading taken
    /// before the rebuild.
    public func resealFromBase(_ base: SealBase, replaying moves: [Move], seat: Int,
                               gameId: UInt64, parent8: Data, joins: [MessageJoin],
                               sentAt: Int = MessageKernel.clockNow()) throws -> Data {
        let players = try rebuild(base, replaying: moves, seat: seat)
        let over = residentView(viewer: -1)?.isOver == true
        return try seal(phase: over ? 3 : 2, lastActorSeat: seat, gameId: gameId,
                        parent8: parent8, joins: joins, sentAt: sentAt,
                        // Belt to the brace. Nothing can have moved the resident
                        // game since the lines above put it there - but this is
                        // the assertion that says so, and it is what would catch
                        // a future caller that reintroduces the gap.
                        expectPlayers: players)
    }

    /// EVERYTHING THE BOARD IS PAINTED FROM, read in one breath.
    ///
    /// `MessageTurnController.publish` assigns its `@Published` properties back
    /// to back so SwiftUI can never paint half of a change (round 16). This is
    /// the other half of that promise: the values themselves must also come from
    /// ONE game. They used to be five separate trips into this actor, and the
    /// kernel holds ONE resident game that decoding ADOPTS - so a bubble
    /// snapshot, a Rule-P comparison or a surface reload landing between two of
    /// those trips re-pointed the game underneath the read. What came back was a
    /// board painted from one chain and a menu computed from another, or a whole
    /// read taken from a chain this board was not playing.
    ///
    /// Reported twice on 1.0(21): "as soon as I hit the send button, the 6 of
    /// diamonds somehow transformed into a 6 of hearts covered by the 8 of
    /// hearts, and the pickup button popped back up" (a whole read from the
    /// previous bout's chain), and "pickup button won't appear if attack arrives
    /// while board is open" (a menu from a chain where I was not the defender).
    ///
    /// So this rebuilds the caller's OWN chain from bytes it holds and reads
    /// everything off it, with no suspension point anywhere in between - exactly
    /// what `resealFromBase` above does for a seal, and for exactly the same
    /// reason. The game described is provably the caller's, because it was just
    /// built from the caller's base and the caller's moves.
    ///
    /// `sentAt` 0 means "no send clock" (a genesis, or a pre-round-16 chain) and
    /// reports no hold, matching what the controller asked for by hand before.
    public func readBoard(_ base: SealBase, replaying moves: [Move], seat: Int,
                          sentAt: Int, now: Int = MessageKernel.clockNow()) throws -> BoardRead {
        try rebuild(base, replaying: moves, seat: seat)
        let v = residentView(viewer: seat)
        return BoardRead(view: v,
                         legalPacked: residentLegalPacked(seat: seat),
                         stagedAtomsBefore: stagedAtomsBefore(),
                         hold: sentAt == 0 ? 0 : pickupHold(seat: seat, sentAt: sentAt, now: now),
                         replayCode: v?.isOver == true ? residentReplayCode() : nil)
    }

    /// The event stream for the moves STAGED on `base`, with the boundary they
    /// were cut at - one actor call, same argument as `readBoard`. The board
    /// withholds everything from `settlementCut` onward until Send
    /// (`MessageTurnController.captureSettlement`), and a stream that described
    /// somebody else's chain would withhold the wrong half of it.
    ///
    /// `settlementCut` is the kernel's, over these exact frames
    /// (evwire_frames_settlement_cut); nil means the turn ended no bout and
    /// there is nothing to hold.
    public func stagedTurn(_ base: SealBase, replaying moves: [Move], seat: Int)
        throws -> (atomsBefore: Int, events: [GameEvent], settlementCut: Int?) {
        try rebuild(base, replaying: moves, seat: seat)
        let before = stagedAtomsBefore()
        // The PACKED bytes, read twice: once into events, once for the cut
        // (EvWire.settlementCut -> evwire_frames_settlement_cut). Two reads of
        // one buffer, in one actor call - a second trip in here to fetch the cut
        // would be a suspension point between the stream and the boundary it is
        // cut at, and the note on `readBoard` says what lands in those.
        guard let packed = lastMovePacked(viewer: seat, atomsBefore: before)
        else { return (before, [], nil) }
        return (before, EvWire.decodeFrames(packed), EvWire.settlementCut(packed))
    }

    /// The same stream at a boundary the CALLER already knows (the board's
    /// `animAtomsBefore`), rebuilt from the caller's chain first. The board
    /// fetches this to decide what a bout end flies - discards, refills, whose
    /// card goes where - and it used to read it straight off the resident game.
    /// An arrival is exactly when that is least safe: Rule P compares two
    /// chains, the surface routes a third and a bubble bakes a fourth, all
    /// while this animation is being composed, and the stream that came back
    /// described whichever one won. The cards then flew the wrong way on a
    /// board that was otherwise correct.
    public func turnEvents(_ base: SealBase, replaying moves: [Move], seat: Int,
                           atomsBefore: Int) throws -> [GameEvent] {
        try rebuild(base, replaying: moves, seat: seat)
        return lastMoveEvents(viewer: seat, atomsBefore: atomsBefore)
    }

    /// ADOPT A CHAIN AND READ WHAT IT SHOULD ANIMATE, in one actor call.
    ///
    /// `MessageTurnController.begin` decoded the chain and then asked for its
    /// opening stream on a second hop, using the boundary the decode had put
    /// down. Two hops is one gap: an arrival landing on an open board runs this
    /// while the surface is also snapshotting bubbles, and the stream came back
    /// cut on whatever chain won the race. Same fix as `readBoard` - the decode
    /// and the reads it justifies happen together or not at all.
    ///
    /// `prior` is the board as it stood BEFORE the bubble's move, or nil when
    /// there is nothing before it (see `lastMoveEventsWithPrior`).
    /// `floor` is the number of atoms this board has ALREADY ANIMATED - never
    /// replay behind it. See the note on the clamp below.
    public func openChain(payload: Data, viewer: Int, floor: Int = -1)
        throws -> (env: MessageEnvelope, events: [GameEvent], prior: GameView?) {
        let env = try decode(payload: payload, viewer: viewer)
        // A BUBBLE'S OWN BOUNDARY IS THE SENDER'S CLAIM, NOT A FACT.
        //
        // `atomsBefore` is `turn - newAtoms`, and `newAtoms` is stamped by
        // whoever sealed the bubble - so a sender whose own rebase failed
        // (every early return in `markSent` used to leave the base a bubble
        // behind, and an older build still does) stamps a boundary one move too
        // early, and every recipient dutifully re-animates a move they have
        // already watched. Owner, on two SEPARATE single-cover bubbles: "when
        // they sent the J of spades cover, I saw the Q of hearts animate IN
        // PARALLEL with the J of spades! Multi card covers / attacks in a
        // SINGLE BUBBLE should be animated in parallel, but these were separate
        // bubbles!"
        //
        // The receiver has a fact the sender's claim cannot override: how much
        // of this chain it has already shown. Clamping to it makes the board
        // robust against any sender - a stale build, a failed rebase, a
        // hand-rolled bubble - instead of trusting a number computed on a phone
        // this one cannot see. It can only ever REMOVE re-animation, never add
        // any: `max` with a floor of -1 (the default, "no floor") is the exact
        // behaviour every existing caller had.
        //
        // Deliberately NOT applied to a cold open. There the floor is -1
        // because the controller has adopted nothing yet, which is what keeps
        // "close the bubble I just sent and open it again" animating my own
        // move (owner, round 22) - a clamp keyed on the chain's own turn would
        // have silently killed that.
        let opening = lastMoveEventsWithPrior(viewer: viewer,
                                              atomsBefore: max(env.atomsBefore, floor))
        return (env, opening.events, opening.prior)
    }

    /// EVERYTHING A BUBBLE SAYS ABOUT ITSELF - decoded once, read together.
    ///
    /// The picture (`BubbleSnapshot`) and the caption (`MessageSummary`) are the
    /// two things a bubble carries to people who are not looking at the board,
    /// and both are drawn from the chain it names. They used to take the
    /// envelope from a `peek` and then read the RESIDENT game, on the note that
    /// "the caller must have just sealed or decoded this payload" - true of
    /// every caller written so far, and exactly the kind of promise that quietly
    /// stops being kept. This picture goes out to the whole thread and shows on
    /// lock screens; it should be the last thing in the app reading a game some
    /// other task may have swapped in.
    ///
    /// One call, so the two can never disagree with each other either. The view
    /// is the PUBLIC one (viewer -1, no hand), which is what keeps a hand out of
    /// a notification by construction rather than by care.
    public func publicRead(payload: Data)
        throws -> (env: MessageEnvelope, view: GameView?, events: [GameEvent]) {
        let env = try decode(payload: payload, viewer: -1)
        return (env, residentView(viewer: -1),
                lastMoveEvents(viewer: -1, atomsBefore: env.atomsBefore))
    }

    /// Re-establish `base` and replay `moves` onto it. The shared first half of
    /// `resealFromBase`, `readBoard` and `stagedTurn`; private because rebuilding
    /// and THEN reading in a second call would reopen the very gap all three
    /// exist to close.
    /// Returns the rebuilt game's player count, which is what `resealFromBase`
    /// hands to `seal(expectPlayers:)` as its backstop.
    @discardableResult
    private func rebuild(_ base: SealBase, replaying moves: [Move], seat: Int) throws -> Int {
        let players: Int
        switch base {
        case .continuation(let payload): players = try decode(payload: payload, viewer: seat).nPlayers
        case .genesis(let seed, let n): try newGame(seed: seed, players: n); players = n
        }
        for m in moves { try apply(seat: seat, move: m) }
        return players
    }

    /// THE CLOCK, in the one unit the wire speaks: unix seconds mod 65536.
    ///
    /// Two bytes, and the wrap is deliberate — nothing ever needs the absolute
    /// time, only `now - sent_at` against 15, and unsigned subtraction gets that
    /// right across a rollover (see c/src/msg_wire.h for the full argument and
    /// what the wrap costs).
    ///
    /// nonisolated: a clock read touches no kernel state, and the send path
    /// stamps it as a default argument, which cannot await.
    public nonisolated static func clockNow() -> Int {
        Int(Date().timeIntervalSince1970.rounded(.down)) & 0xffff
    }

    /// ROUND 16 — how many seconds `seat` must still wait before it may pick up,
    /// against the game the last `decode` left resident. 0 = it may pick up now.
    ///
    /// The RULE is `msg_pickup_hold_remaining` in C, not here: the defender, a
    /// last action that is an attack or a pass, and spare capacity on the table
    /// are all questions about the game, and this file marshals. Deliberately
    /// separate from `residentLegal` — the v6 body codes each action as an index
    /// into the legal-move menu, so a hold that removed `pickup` from that menu
    /// would re-point every replay code ever written.
    public func pickupHold(seat: Int, sentAt: Int, now: Int = MessageKernel.clockNow()) -> Int {
        Int(fio_msg_pickup_hold(Int32(seat), Int32(sentAt & 0xffff), Int32(now & 0xffff)))
    }

    /// Where the moves THIS DEVICE has staged begin, as an atom count on the
    /// resident game (fio_msg_staged_atoms_before): the `atomsBefore` a board
    /// passes to `lastMoveEvents` to animate its OWN turn.
    ///
    /// Measured from the kernel's log mark, which is the only fold-proof answer
    /// — the atom count of the adopted chain can exceed what the same history
    /// re-encodes to, and a boundary past the end of the stream animates
    /// nothing at all. -1 when no chain has been adopted.
    public func stagedAtomsBefore() -> Int { Int(fio_msg_staged_atoms_before()) }

    /// The best shareable REPLAY code for the resident (finished) game — the §12
    /// funnel code behind `replayLink`. v6 when the deal is re-derivable, else v5;
    /// the kernel chooses, not app code. nil if no game or it cannot encode.
    public func residentReplayCode() -> String? {
        guard let data = packedCall({ fio_replay_share_code_b32($0, $1) }), !data.isEmpty
        else { return nil }
        return String(decoding: data, as: UTF8.self)
    }

    /// The resident message game decoded as a replay — the PACKED LOG_* step
    /// stream (fio_replay_decode_packed via the game's own v6 share code), for
    /// driving animations (e.g. replaying the last move when a bubble is opened).
    /// No JSON crosses the boundary. nil if there is no game or it cannot encode.
    public func residentReplay() -> DecodedReplay? {
        guard let code = residentReplayCode() else { return nil }
        let packed = code.withCString { (cstr: UnsafePointer<CChar>) -> Data? in
            packedCall { out, cap in
                out.withMemoryRebound(to: UInt8.self, capacity: Int(cap)) { u8 in
                    fio_replay_decode_packed(cstr, u8, cap)
                }
            }
        }
        guard let packed else { return nil }
        return DecodedReplay.decode(packed: packed)
    }

    /// The animations of the resident game's LAST BUBBLE, masked for `viewer` -
    /// the kernel's evwire event stream, the SAME one live play and the website
    /// emit. Each step bundles its move with its refill/discard/defender-change
    /// consequences, and the viewer's own drawn/picked-up cards come back with
    /// real identities while everyone else's are hidden - so a reopened bubble
    /// animates through the kernel, not a client-side GameView diff (which could
    /// never recover the viewer's own drawn card and so silently dropped it, the
    /// "my refill never animated on reopen" bug).
    ///
    /// `atomsBefore` is where this bubble starts: the number of atoms already
    /// on the chain when it was sealed, so the kernel replays what came after
    /// them and not the move before them as well. A receiver reads it off the
    /// envelope it opened (`MessageEnvelope.atomsBefore`); a board animating its
    /// OWN just-played move passes the turn of the chain it adopted, which is
    /// the same boundary from the other side - and, unlike a count of staged
    /// moves, is exact, because the codec is not 1:1 with actions (it folds a
    /// bout's closing goods into one atom and can expand a closing good into
    /// two). Pass -1 and it falls back to its pre-round-16 guess - the trailing
    /// run of steps by one acting seat - which is right for a bubble whose
    /// sender staged everything at once (a double cover replays both covers)
    /// and wrong for one who covered, sent, covered and sent again. What is
    /// NEVER an input is "where I last looked": the boundary is a property of
    /// the bubble, so a wiped store or a new phone must not change what
    /// animates.
    ///
    /// [] if there is no game, it is not v6-encodable, or the group produced
    /// nothing to animate.
    public func lastMoveEvents(viewer: Int, atomsBefore: Int = -1) -> [GameEvent] {
        guard let packed = lastMovePacked(viewer: viewer, atomsBefore: atomsBefore)
        else { return [] }
        return EvWire.decodeFrames(packed)
    }

    /// The same stream still PACKED - the frame bytes the kernel wrote, before
    /// anything decoded them. `stagedTurn` needs them twice (the events, and the
    /// settlement cut the kernel reads off the same frames), and a decode throws
    /// away what the second question is asked of.
    /// nil if there is no game, it is not v6-encodable, or the turn animates
    /// nothing.
    private func lastMovePacked(viewer: Int, atomsBefore: Int) -> Data? {
        guard let code = residentReplayCode() else { return nil }
        let packed = code.withCString { (cstr: UnsafePointer<CChar>) -> Data? in
            packedCall { out, cap in
                out.withMemoryRebound(to: UInt8.self, capacity: Int(cap)) { u8 in
                    fio_replay_last_events_packed(cstr, Int32(viewer), Int32(atomsBefore), u8, cap)
                }
            }
        }
        guard let packed, !packed.isEmpty else { return nil }
        return packed
    }

    /// THIS BUBBLE'S ANIMATIONS, AND THE BOARD THEY START FROM.
    ///
    /// `lastMoveEvents` answers what MOVED; a board opening cold also needs to
    /// know what the table looked like an instant BEFORE, because that is where
    /// its role marks have to start. Seeding them from the stream's own first
    /// frame is off by one move: an event's `state` is the board AS OF that step,
    /// so a bubble carrying a good opens with the check already printed and the
    /// sword-to-check flip has nowhere left to happen (round 21, the owner: "it
    /// started out already in GOOD").
    ///
    /// The prior board is the same stream asked for ONE STEP EARLIER. Step 0 is
    /// the deal and this bubble's own steps begin at `atomsBefore + 1`, so step
    /// `atomsBefore` is the last step of whatever came before it, and its
    /// committed state is exactly "the table as this bubble found it".
    ///
    /// BOTH READS IN ONE ACTOR CALL, which is the reason this exists as a method
    /// rather than two calls from the board: between two hops any other decode
    /// repoints the resident game, and the second read would describe a different
    /// chain than the first (see `resealFromBase`).
    ///
    /// SELF-CHECKING. The extra read must come back as exactly one more step than
    /// the plain one; anything else means the step/atom mapping is not what is
    /// assumed here and the prior board is reported as `nil` rather than guessed
    /// at. A nil prior is not a failure - it is what every caller did before this
    /// existed, and the board falls back to seeding from the first frame.
    ///
    /// `atomsBefore < 1` has no earlier step to ask for (the bubble is the first
    /// move on a fresh deal, or the deal itself), so it skips the second read.
    public func lastMoveEventsWithPrior(viewer: Int, atomsBefore: Int)
        -> (events: [GameEvent], prior: GameView?) {
        let events = lastMoveEvents(viewer: viewer, atomsBefore: atomsBefore)
        // An EMPTY stream is worth a prior board too, and is in fact the case
        // that needs one most: a `good` that does not close the bout emits no
        // step at all, so the only thing a board can be told about it is the
        // difference between two role states.
        guard atomsBefore >= 1 else { return (events, nil) }
        let withPrior = lastMoveEvents(viewer: viewer, atomsBefore: atomsBefore - 1)
        guard withPrior.count == events.count + 1 else { return (events, nil) }
        return (events, withPrior.first?.state)
    }

    /// Rule P (§7.2). <0 `a` wins, >0 `b`, 0 the same chain. Delivery order is
    /// never an input — two devices can transiently disagree about "newest".
    public func preferred(_ a: Data, _ b: Data) throws -> Int {
        let r: Int32 = a.withUnsafeBytes { ap in
            b.withUnsafeBytes { bp in
                fio_msg_rule_p(ap.bindMemory(to: UInt8.self).baseAddress, Int32(a.count),
                               bp.bindMemory(to: UInt8.self).baseAddress, Int32(b.count))
            }
        }
        if r < -1 { throw MessageEnvelope.Failure.damaged(code: Int(fio_last_msg_error())) }
        return Int(r)
    }

    // ROUND 9 (owner): the Swift Rule-R binding (`rebase(pendingRound:seat:
    // awire:)` over fio_msg_rebase_awire) is removed with the iOS pending
    // ledger - nothing on this platform rebases stored moves any more. The C
    // kernel entry itself stays (the wasm bridge and the FMSG e2e concurrency
    // suite still exercise Rule R as a kernel capability).
}

// Base32 moved to its own file (sdk/swift/Base32.swift). It is pure Foundation
// and this file is not: everything else here reaches into CFoolish, so the codec
// could not be compiled on its own - and the cross-language round-trip test for
// the replay names blob (e2e/imessage_replay_names.test.ts) has to compile the
// REAL codec, not a copy of it, or it proves nothing. Same type, same module,
// same callers.
