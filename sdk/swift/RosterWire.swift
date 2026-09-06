// RosterWire.swift - the roster, packed, in the one layout the kernel already
// writes.
//
//     n_joins(1), then n_joins x { seat(1) name_len(1) name[name_len] }
//
// That is the tail of `fio_msg_decode_packed`'s blob, so this file is a reader
// and a writer of the SAME bytes rather than a second format. It was the last
// JSON on any path that matters: the four `fio_msg_*` entries took the roster as
// `[{"seat":0,"name":"Sveta"},...]` and parsed it in C, and the client-server
// envelope carried a JSON island inside an otherwise packed payload
// (FoolishNet/PackedGame.swift), a dozen lines under a comment saying the wire
// was packed.
//
// NOTHING ABOUT THE WIRE MOVED. The FMSG envelope's own join layout is
// untouched; what changed is that a host now hands the roster over in the shape
// the kernel hands it back, instead of in a shape only a parser could read.

import Foundation

public enum RosterWire {
    /// MSG_MAX_NAME (c/src/msg_wire.h). A name is <=64 UTF-8 BYTES, not
    /// characters, and the kernel refuses a longer one outright.
    public static let maxNameBytes = 64
    /// MSG_MAX_JOINS == MAX_PLAYERS.
    public static let maxJoins = 8

    /// The roster as bytes. Over-long names are trimmed by whole UNICODE
    /// SCALARS so the result is always valid UTF-8 - the same discipline
    /// ReplayExtras.nameBytes keeps, and for the same reason: a severed
    /// multi-byte sequence is worse than a shorter name.
    ///
    /// Deliberately not throwing on a long name. The kernel's cap is a refusal
    /// and a refused seal is a bubble that never goes out; the display cap is 12
    /// bytes anyway (docs/APP_REVIEW_NOTES.md), so 64 is already generous.
    public static func encode(_ joins: [MessageJoin]) -> Data {
        var w = PackedWriter()
        w.u8(min(joins.count, maxJoins))
        for j in joins.prefix(maxJoins) {
            w.u8(j.seat)
            w.blob8(nameBytes(j.name))   // <=64 bytes, so the u8 prefix always fits
        }
        return w.data
    }

    /// The roster back out of a blob, starting at `at`. Returns the joins and
    /// the offset just past them, or nil if any record runs off the end - the
    /// same all-or-nothing the C reader keeps, since a roster read short is a
    /// different table.
    public static func decode(_ b: [UInt8], at: Int) -> (joins: [MessageJoin], next: Int)? {
        var r = PackedReader(b, at: at)
        guard let n = r.u8() else { return nil }
        var joins: [MessageJoin] = []
        joins.reserveCapacity(n)
        for _ in 0..<n {
            guard let seat = r.u8(), let name = r.blob8() else { return nil }
            joins.append(MessageJoin(seat: seat, name: String(decoding: name, as: UTF8.self)))
        }
        return (joins, r.at)
    }

    /// CALL A GATE THAT TAKES A ROSTER AND A NAME. Both cross as bytes with a
    /// length - a nickname is arbitrary Unicode and NUL is not its terminator,
    /// so a C string was never the right shape for one.
    static func call<T>(_ joins: [MessageJoin], _ name: String?,
                        _ body: (UnsafePointer<UInt8>?, Int32,
                                 UnsafePointer<UInt8>?, Int32) -> T) -> T {
        let packed = encode(joins)
        let n = Array((name ?? "").utf8)
        return packed.withUnsafeBytes { p in
            n.withUnsafeBufferPointer { np in
                body(p.bindMemory(to: UInt8.self).baseAddress, Int32(packed.count),
                     np.baseAddress, Int32(n.count))
            }
        }
    }

    /// One name's UTF-8 bytes, trimmed to the budget on a scalar boundary.
    private static func nameBytes(_ name: String) -> [UInt8] {
        var scalars = Array(name.unicodeScalars)
        var bytes = Array(String(String.UnicodeScalarView(scalars)).utf8)
        while bytes.count > maxNameBytes && !scalars.isEmpty {
            scalars.removeLast()
            bytes = Array(String(String.UnicodeScalarView(scalars)).utf8)
        }
        return bytes
    }
}
