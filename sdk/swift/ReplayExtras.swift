// ReplayExtras - the seat NICKNAMES that ride along a §12 replay code.
//
// A replay code is two blobs joined by a dash:
//
//     foolish.cards/<base32 moves>                 (the game)
//     foolish.cards/<base32 moves>-<base32 extras> (the game + who played it)
//
// The moves half is the kernel's (fio_replay_share_code_b32) and is NOT touched
// here. The extras half is a side channel the web replay screen already reads -
// server/api/common/replay/extras.ts, consumed by ReplayScreen's
// buildReplayData - and until now the iMessage extension was the one producer
// that never wrote it, so a game played with friends replayed on the web as
// "P1" vs "P2". The website's own finished games have carried names since the
// channel was invented (finalizeEndedGame); this is the phone catching up, not
// a new format.
//
// WHY THIS IS SWIFT AND NOT C. Everything about a Durak game belongs in the
// kernel, and the moves half of the code is built there. This blob is not a
// game: it is decoration bolted onto a URL, carrying no rule, no card and no
// state the engine could ever disagree with a browser about. It also has no
// second caller - the server encodes it in TypeScript, the web decodes it in
// TypeScript, and the phone is the only other producer there will be. A C entry
// point would mean a third implementation of the same twelve lines plus a
// rebuilt xcframework, to move code that cannot fork the game. If a fourth
// producer ever appears, lift it then, and lift the TS one with it.
//
// '-' is deliberate: it is in the QR alphanumeric charset and NOT in the base32
// alphabet, so a code with names still QR-encodes densely and the moves-only
// code is always exactly the prefix before the dash.
//
// A MALFORMED BLOB MUST NEVER BREAK THE REPLAY. The reader treats extras as
// best-effort (ReplayScreen wraps decodeExtras in a try/catch and falls back to
// P1/P2), and this writer holds up the other end of that bargain: it either
// emits a blob the reader can parse or emits no segment at all.
import Foundation

public enum ReplayExtras {
    /// extras.ts EXTRAS_VERSION. Bumping it here without bumping it there makes
    /// every code we emit unreadable, silently - the reader throws
    /// "unsupported version" and the replay falls back to P1/P2.
    private static let version: UInt8 = 2
    /// extras.ts FLAG_NAMES (bit0). Bit1 is the timing section, which an
    /// iMessage chain cannot fill: its only clock is the per-bubble `sentAt`
    /// (unix seconds mod 65536, one reading per bubble, not one per move), so
    /// there are no per-move gaps to quantize. Names-only blobs are a shape the
    /// format was built for - the flags byte exists precisely so a producer can
    /// answer one question and stay quiet about the other.
    private static let flagNames: UInt8 = 1

    /// extras.ts MAX_NAME_BYTES. NOT the same cap as FMSG's MSG_MAX_NAME (64
    /// bytes, c/src/msg_wire.h - raised from the 12 that App Store review's B1
    /// found too tight for "Владимир"), and not the same as the 16-character
    /// limit the nickname field enforces in the UI. Three different layers, three
    /// different budgets, and this is the smallest of them: 16 characters of
    /// emoji is 64 bytes on the FMSG wire and would not fit here. So the trim
    /// below is reachable in practice, not theatre.
    public static let maxNameBytes = 48

    /// Seat -> name, as the dense seat-ordered array the wire wants. The reader
    /// gets no count of its own: `decodeExtras(code, playerCount, moveCount)`
    /// takes playerCount from the DECODED MOVES and then reads exactly that many
    /// NUL-terminated strings, so a short array desynchronizes the parse and a
    /// long one leaves the time section unreachable. Unnamed seats are "" - the
    /// frame builder renders an empty name as "P<n>", which is exactly the
    /// nothing-known fallback we want for a seat that never introduced itself.
    public static func seatNames(_ names: [Int: String], count: Int) -> [String] {
        guard count > 0 else { return [] }
        return (0..<count).map { names[$0] ?? "" }
    }

    /// The names section as its own base32 blob, or nil when there is nothing
    /// worth saying (no seats, or every seat anonymous - an all-empty roster
    /// decodes to the same P1/P2 the reader already shows, so the bytes would
    /// buy nothing).
    public static func encodeNames(_ names: [String]) -> String? {
        guard names.contains(where: { !$0.isEmpty }) else { return nil }
        var out: [UInt8] = [version, flagNames]
        for name in names {
            out.append(contentsOf: nameBytes(name))
            out.append(0)   // NUL terminator
        }
        return Base32.encode(Data(out))
    }

    /// One name's UTF-8 bytes, NUL-free and trimmed to the budget.
    ///
    /// NUL is stripped rather than escaped because it is the terminator; it can
    /// never appear INSIDE a UTF-8 multi-byte sequence, which is the whole
    /// reason a NUL-terminated list is safe for arbitrary Unicode at all.
    ///
    /// The trim drops whole UNICODE SCALARS (never bytes), so the result is
    /// always valid UTF-8 and the reader's TextDecoder never sees a severed
    /// sequence. Scalars and not Characters, byte-for-byte with the TS encoder's
    /// `Array.from(name)` - a grapheme cluster may be split (a flag losing half
    /// its regional indicator, an emoji losing its skin tone) but a code point
    /// never is. Both sides trimming the same way matters more than either
    /// trimming prettily: it is what makes the cross-language round-trip test
    /// (e2e/imessage_replay_names.test.ts) an equality and not an approximation.
    private static func nameBytes(_ name: String) -> [UInt8] {
        var scalars = Array(name.unicodeScalars.filter { $0.value != 0 })
        var bytes = Array(String(String.UnicodeScalarView(scalars)).utf8)
        while bytes.count > maxNameBytes {
            scalars.removeLast()
            bytes = Array(String(String.UnicodeScalarView(scalars)).utf8)
        }
        return bytes
    }

    /// The full URL code: the kernel's moves code, plus the names segment when
    /// there is one. Degrades to the bare moves code, which is what every
    /// build before this one emitted and what every old link still is.
    public static func code(moves: String, names: [String]) -> String {
        guard let extras = encodeNames(names) else { return moves }
        return moves + "-" + extras
    }
}
