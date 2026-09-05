// Base32 - the URL layer's alphabet, shared by the /m/ bubble payload (§4.3),
// the §12 replay code and the replay names blob (ReplayExtras).
//
// Lifted out of MessageEnvelope.swift unchanged. It lives alone because it is
// the one piece of that file with no CFoolish in it, and a Foundation-only file
// can be compiled by itself - which is what lets the cross-language round trip
// (e2e/imessage_replay_names.test.ts) feed the REAL encoder's output to the
// REAL web decoder instead of pinning a hand-copied fixture.
import Foundation

/// RFC 4648 base32, uppercase, no padding - the same alphabet the replay codec
/// and the /m/ route use (codec.ts). QR-alphanumeric-safe and URL-safe, which is
/// why the payload is base32 and not base64.
public enum Base32 {
    private static let A = Array("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567")

    public static func encode(_ data: Data) -> String {
        var out = "", bits = 0, value = 0
        for b in data {
            value = (value << 8) | Int(b); bits += 8
            while bits >= 5 { out.append(A[(value >> (bits - 5)) & 31]); bits -= 5 }
        }
        if bits > 0 { out.append(A[(value << (5 - bits)) & 31]) }
        return out
    }

    public static func decode(_ s: String) -> Data? {
        var bits = 0, value = 0
        var out = Data()
        for ch in s.uppercased() {
            guard let idx = A.firstIndex(of: ch) else { continue }  // ignore stray chars
            value = (value << 5) | idx; bits += 5
            if bits >= 8 { out.append(UInt8((value >> (bits - 8)) & 0xff)); bits -= 8 }
        }
        return out
    }
}
