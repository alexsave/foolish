// ReplayExtras - the seat NICKNAMES that ride along a §12 replay link.
//
// A replay link is a prefix, a game, and optionally who played it:
//
//     https://foolish.cards/<base32 moves>                 (the game)
//     https://foolish.cards/<base32 moves>-<base32 extras> (…and the table)
//
// All of that string is the kernel's. `fio_replay_share_link`
// (c/src/replay_extras.c) owns the version byte, the name budget, the trimming
// rule, the dash, the prefix and the decision to stay quiet about an anonymous
// table; this file is the marshaling in front of it, and MessageEnvelope turns
// what comes back into a `URL`. It used to be a second implementation of that
// format, kept in step with the web's TypeScript third by a parity test - #113.
import Foundation
import CFoolish

public enum ReplayExtras {
    /// Seat -> name, as the dense seat-ordered array the wire wants. The reader
    /// gets no count of its own - it takes the seat count from the DECODED
    /// MOVES and then reads exactly that many names - so the roster has to be
    /// as wide as the table. Unnamed seats are "", which the frame builder
    /// renders as "P<n>": exactly the nothing-known fallback a seat that never
    /// introduced itself should get.
    public static func seatNames(_ names: [Int: String], count: Int) -> [String] {
        guard count > 0 else { return [] }
        return (0..<count).map { names[$0] ?? "" }
    }

    /// The whole link for this game and this table, as the kernel writes it.
    /// An empty or all-anonymous roster gets the bare link, which is what every
    /// build before names emitted and what every old link still is.
    public static func link(moves: String, names: [String]) -> String {
        var w = PackedWriter()
        for name in names { w.blob(Array(name.utf8)) }
        let roster = w.bytes
        // Sized from the arguments rather than grown on FIO_ECAP: the answer is
        // the prefix, the moves code (a long v6 game runs to tens of KB) and at
        // most 8/5 of the roster in base32, so one call always suffices and
        // there is no retry loop to get wrong.
        let cap = moves.utf8.count + roster.count * 2 + 256
        var out = [CChar](repeating: 0, count: cap)
        let n = roster.withUnsafeBufferPointer { rp in
            fio_replay_share_link(moves, rp.baseAddress, Int32(roster.count),
                                  Int32(names.count), &out, Int32(cap))
        }
        guard n >= 0 else { return moves }   // unreachable; a code beats nothing
        return String(decoding: out.prefix(Int(n)).map { UInt8(bitPattern: $0) },
                      as: UTF8.self)
    }

    /// The link with no names on it. Kept separate from `link(moves:names:)`
    /// only so a caller with no roster does not have to invent an empty one.
    public static func bare(_ moves: String) -> String {
        link(moves: moves, names: [])
    }
}
