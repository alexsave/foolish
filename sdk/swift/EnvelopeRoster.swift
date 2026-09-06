// EnvelopeRoster.swift - the client-server envelope's roster, read as bytes.
//
// The server's packed game envelope (sdk/ts/wire/view.ts, encodeGameResponse)
// used to carry its roster as a JSON island: a dozen lines under a comment in
// PackedGame.swift saying the wire was packed, and the last JSON on any path
// that matters. It is packed now, and this file reads it.
//
// ONE CODEC, NOT TWO. The seats and their names are the KERNEL's block -
// `n(1)`, then `n x { seat(1) name_len(1) name[] }`, exactly what
// `fio_msg_decode_packed` hands back and exactly what RosterWire writes - so
// this reads them THROUGH RosterWire rather than beside it. What the envelope
// needs and the kernel's roster does not (a player id, an is_ai flag, the
// game's own id/name/status, the good-players order and its timestamp) rides
// alongside that block, so the kernel's shape stays the kernel's shape.
//
// The writer is TypeScript, on the server, and there is no way to run it here;
// e2e/packed_roster_wire.test.ts compiles this file and feeds it the real
// encoder's bytes so the two cannot drift.
//
// Foundation only, deliberately - see the note in RosterWire.swift.

import Foundation

/// The envelope's roster, carrying every field its JSON predecessor carried.
public struct EnvelopeRoster: Sendable, Equatable {
    public struct Player: Sendable, Equatable {
        public let playerId: String
        public let name: String
        public let isAI: Bool
        public init(playerId: String, name: String, isAI: Bool) {
            self.playerId = playerId; self.name = name; self.isAI = isAI
        }
    }

    /// The game's id (`games.id`) - the client's handle on the game.
    public let id: String
    /// The game's display name.
    public let name: String
    /// `games.status`, which is COLUMN-AUTHORITATIVE over the blob's copy: 0
    /// waiting, 1 playing, 2 game_over. Without it a WAITING lobby's blob
    /// decodes as a playing board.
    public let status: Int
    /// Seat-indexed. `players[seat]`, always.
    public let players: [Player]
    /// The good-players list, in INSERTION order, by player id. An order and
    /// not a set, and it may name someone who has since left the table, which
    /// is why it is ids here and not a seat mask.
    public let goodPlayers: [String]
    /// The good-players timestamp, or nil when there is none.
    public let goodTimestamp: Double?

    public init(id: String, name: String, status: Int, players: [Player],
                goodPlayers: [String], goodTimestamp: Double?) {
        self.id = id; self.name = name; self.status = status
        self.players = players; self.goodPlayers = goodPlayers
        self.goodTimestamp = goodTimestamp
    }

    /// The trailer's own version byte. A reader that does not know the value
    /// stops rather than reading a field that moved.
    public static let wireFormat = 1
    /// Status codes, in view.c's G_STATUS order.
    public static let statusWaiting = 0, statusPlaying = 1, statusGameOver = 2

    /// Read a packed roster starting at `at`. Returns the roster and the offset
    /// just past it, or nil if any field runs off the end - the same
    /// all-or-nothing RosterWire keeps, since a roster read short is a
    /// different table.
    public static func decode(_ b: [UInt8], at: Int) -> (roster: EnvelopeRoster, next: Int)? {
        var r = PackedReader(b, at: at)
        guard r.u8() == wireFormat,
              let idBytes = r.blob(), let nameBytes = r.blob(),
              let status = r.u8(), status <= statusGameOver else { return nil }

        // The kernel's names block, read by the kernel's reader.
        guard let names = RosterWire.decode(b, at: r.at) else { return nil }
        r = PackedReader(b, at: names.next)

        var players: [Player] = []
        players.reserveCapacity(names.joins.count)
        for (i, join) in names.joins.enumerated() {
            // The block is seat-indexed by construction; a gap would silently
            // rename somebody, so refuse it rather than guess.
            // A u16 length on the id, not the kernel's u8: the id is a field
            // THIS side invents and has no trim rule to fall back on.
            guard join.seat == i, let pid = r.blob(), let isAI = r.u8() else { return nil }
            players.append(Player(playerId: String(decoding: pid, as: UTF8.self),
                                  name: join.name, isAI: isAI != 0))
        }

        guard let nGood = r.u8() else { return nil }
        var good: [String] = []
        good.reserveCapacity(nGood)
        for _ in 0..<nGood {
            guard let id = r.blob() else { return nil }
            good.append(String(decoding: id, as: UTF8.self))
        }

        guard let hasTs = r.u8() else { return nil }
        var ts: Double? = nil
        if hasTs != 0 {
            guard let v = r.f64() else { return nil }
            ts = v
        }

        return (EnvelopeRoster(id: String(decoding: idBytes, as: UTF8.self),
                               name: String(decoding: nameBytes, as: UTF8.self),
                               status: status, players: players,
                               goodPlayers: good, goodTimestamp: ts), r.at)
    }
}
