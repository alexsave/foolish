// PackedBytes.swift - the one byte reader/writer this repo's Swift packs with.
//
// Everything that crosses a boundary here is fixed-layout bytes rather than
// JSON (§zero-JSON), and before this file every such layout was its own hand
// rolled loop of shifts and bounds checks. This is that loop, written once:
// little-endian scalars, length-prefixed blobs, and a reader that returns nil
// the moment a field would run past the end.
//
// STRICTNESS IS THE POINT. Every read is bounded before it happens and a short
// buffer stops the walk dead, so a caller's only two outcomes are "the whole
// record" and "nothing" - never half a record read as a whole one. That is the
// same discipline evwire.h states for the kernel's own readers, and the reason
// the on-disk stores can treat an unreadable container as "no rows" cleanly.
//
// Two length prefixes, deliberately: `text8`/`blob8` for a field the KERNEL
// already length-prefixes with a byte (a roster name, MSG_MAX_NAME 64), and
// `text`/`blob` with a u16 for everything this side invents. Picking the
// kernel's width where the kernel has one is what keeps RosterWire a reader and
// writer of the same bytes rather than a translation.

import Foundation

public struct PackedWriter {
    public private(set) var bytes: [UInt8] = []
    public init() {}

    public mutating func u8(_ v: Int) { bytes.append(UInt8(truncatingIfNeeded: v)) }
    public mutating func u16(_ v: Int) {
        bytes.append(UInt8(v & 0xFF)); bytes.append(UInt8((v >> 8) & 0xFF))
    }
    public mutating func u32(_ v: UInt32) {
        for i in 0..<4 { bytes.append(UInt8((v >> (8 * UInt32(i))) & 0xFF)) }
    }
    /// A time, as its IEEE-754 bit pattern. Times here order evictions and
    /// nothing else, so the only requirement is that the round trip is exact.
    public mutating func f64(_ v: Double) {
        let b = v.bitPattern
        for i in 0..<8 { bytes.append(UInt8((b >> (8 * UInt64(i))) & 0xFF)) }
    }
    public mutating func u8s(_ v: [UInt8]) { bytes.append(contentsOf: v) }

    /// A byte string with a u16 length. Returns false, and writes NOTHING, for
    /// a blob that does not fit the prefix: a truncated row is a lie.
    @discardableResult
    public mutating func blob(_ v: [UInt8]) -> Bool {
        guard v.count <= 0xFFFF else { return false }
        u16(v.count); bytes.append(contentsOf: v)
        return true
    }
    @discardableResult
    public mutating func text(_ s: String) -> Bool { blob(Array(s.utf8)) }

    /// The same, with the byte-wide prefix the kernel's own records use.
    @discardableResult
    public mutating func blob8(_ v: [UInt8]) -> Bool {
        guard v.count <= 0xFF else { return false }
        u8(v.count); bytes.append(contentsOf: v)
        return true
    }

    public var data: Data { Data(bytes) }
}

public struct PackedReader {
    private let b: [UInt8]
    public private(set) var at: Int
    public init(_ d: Data, at: Int = 0) { self.b = [UInt8](d); self.at = at }
    public init(_ b: [UInt8], at: Int = 0) { self.b = b; self.at = at }

    public var isAtEnd: Bool { at >= b.count }

    public mutating func u8() -> Int? {
        guard at < b.count else { return nil }
        defer { at += 1 }
        return Int(b[at])
    }
    public mutating func u16() -> Int? {
        guard at + 2 <= b.count else { return nil }
        defer { at += 2 }
        return Int(b[at]) | (Int(b[at + 1]) << 8)
    }
    public mutating func u32() -> UInt32? {
        guard at + 4 <= b.count else { return nil }
        defer { at += 4 }
        var v: UInt32 = 0
        for i in 0..<4 { v |= UInt32(b[at + i]) << (8 * UInt32(i)) }
        return v
    }
    public mutating func f64() -> Double? {
        guard at + 8 <= b.count else { return nil }
        defer { at += 8 }
        var v: UInt64 = 0
        for i in 0..<8 { v |= UInt64(b[at + i]) << (8 * UInt64(i)) }
        return Double(bitPattern: v)
    }
    public mutating func u8s(_ n: Int) -> [UInt8]? {
        guard n >= 0, at + n <= b.count else { return nil }
        defer { at += n }
        return Array(b[at..<at + n])
    }
    public mutating func blob() -> [UInt8]? {
        guard let n = u16() else { return nil }
        return u8s(n)
    }
    public mutating func text() -> String? {
        guard let v = blob() else { return nil }
        return String(decoding: v, as: UTF8.self)
    }
    public mutating func blob8() -> [UInt8]? {
        guard let n = u8() else { return nil }
        return u8s(n)
    }
}
