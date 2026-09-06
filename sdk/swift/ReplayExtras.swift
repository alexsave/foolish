// ReplayExtras - the seat NICKNAMES that ride along a §12 replay code.
//
// A replay code is two blobs joined by a dash:
//
//     foolish.cards/<base32 moves>                 (the game)
//     foolish.cards/<base32 moves>-<base32 extras> (the game + who played it)
//
// Both halves are the kernel's. `fio_replay_extras_link` (c/src/replay_extras.c)
// owns the version byte, the name budget, the trimming rule and the decision to
// stay quiet about an anonymous table; this file is the marshaling in front of
// it. It used to be a second implementation of that format, kept in step with
// the web's TypeScript third by a parity test - #113.
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

    /// The full URL code: the kernel's moves code, plus the names segment when
    /// the roster says anything. Degrades to the bare moves code, which is what
    /// every build before this one emitted and what every old link still is.
    public static func code(moves: String, names: [String]) -> String {
        var w = PackedWriter()
        for name in names { w.blob(Array(name.utf8)) }
        let roster = w.bytes
        var cap = 8 * 1024
        while true {
            var out = [CChar](repeating: 0, count: cap)
            let n = roster.withUnsafeBufferPointer { rp in
                fio_replay_extras_link(moves, rp.baseAddress, Int32(roster.count),
                                       Int32(names.count), &out, Int32(cap))
            }
            if n >= 0 { return String(decoding: out.prefix(Int(n)).map { UInt8(bitPattern: $0) }, as: UTF8.self) }
            if n == -3 { cap *= 2; if cap > (1 << 20) { return moves }; continue }
            return moves     // decoration: a link without names still plays
        }
    }
}
