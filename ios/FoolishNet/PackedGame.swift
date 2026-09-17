// PackedGame.swift - the server's enveloped packed-game buffer, as a hex column.
//
// WHAT IS LEFT OF THIS FILE IS THE HEX. The envelope itself - its header, the
// masked board inside it, the packed roster trailer behind it, and the rule
// that the roster's status outranks the blob's copy of it - is read by the
// kernel now (c/src/client_table.h client_adopt_envelope, through
// EngineC.adoptEnvelope). This file used to state that layout a second time:
//
//   [0] magic  [1] flags  [2] seat  [3..6] version  [7..8] roster_len
//   [q] view_len  [q+2] format  [q+3] viewer  [q+4..] the masked state
//   [q+viewLen..] the packed roster
//
// Every one of those offsets is in c/src/client_table.c, where the WEB reads
// them too, and the seats come back with their names and their is_ai already on
// them instead of being merged on afterwards.
//
// `player_views.view` arrives as bare hex over realtime, and hex is a transport
// detail of that column rather than anything the kernel has an opinion about -
// so this is where it stays.

import Foundation
import FoolishKit

public enum PackedGame {

    /// Decode an enveloped packed-game buffer. nil for a payload the kernel
    /// refuses (the caller treats it as unreadable, like the web).
    public static func decode(_ buf: Data, engine: EngineC) async -> AdoptedEnvelope? {
        await engine.adoptEnvelope(buf)
    }

    /// The same, from the bare-hex `player_views.view` column string.
    public static func decodeHex(_ hex: String, engine: EngineC) async -> AdoptedEnvelope? {
        guard let data = hexToData(hex) else { return nil }
        return await decode(data, engine: engine)
    }

    /// Bare-hex (optional `\x` / `0x` prefix) to bytes. Nonisolated so the async
    /// decoders can call it off the main actor.
    public static func hexToData(_ hex: String) -> Data? {
        var s = hex
        if s.hasPrefix("\\x") { s = String(s.dropFirst(2)) }
        if s.hasPrefix("0x") { s = String(s.dropFirst(2)) }
        guard s.count % 2 == 0 else { return nil }
        var out = Data(capacity: s.count / 2)
        var idx = s.startIndex
        while idx < s.endIndex {
            let next = s.index(idx, offsetBy: 2)
            guard let b = UInt8(s[idx..<next], radix: 16) else { return nil }
            out.append(b)
            idx = next
        }
        return out
    }
}
