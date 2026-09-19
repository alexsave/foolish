// Base32 - the URL layer's alphabet, shared by the /m/ bubble payload (§4.3)
// and the §12 replay code.
//
// Lifted out of MessageEnvelope.swift unchanged. The file's old note said it
// lived alone because it was the one piece with no CFoolish in it; that was
// true of a version that WROTE the characters itself, which is exactly what it
// no longer does.
import Foundation
import CFoolish

/// RFC 4648 base32, uppercase, no padding - the same alphabet the replay codec
/// and the /m/ route use (codec.ts). QR-alphanumeric-safe and URL-safe, which is
/// why the payload is base32 and not base64.
public enum Base32 {
    private static let A = Array("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567")

    /// Bytes as base32 text. THE KERNEL WRITES IT (`fio_b32_encode` ->
    /// replay.c), which is where the alphabet and the 5-bit packing already
    /// lived: the server reaches the same function through the wasm, so a Swift
    /// copy made this the one codec with three implementations and no test that
    /// compared any two of them.
    ///
    /// Empty for an empty input, and for a refusal the kernel has no reason to
    /// give here - the buffer is sized from the input, five bits at a time.
    public static func encode(_ data: Data) -> String {
        // ceil(8n/5) characters, plus the NUL the kernel writes after them.
        let cap = (data.count * 8 + 4) / 5 + 1
        var out = [CChar](repeating: 0, count: cap)
        let n: Int32 = data.withUnsafeBytes { raw in
            fio_b32_encode(raw.bindMemory(to: UInt8.self).baseAddress, Int32(data.count),
                           &out, Int32(cap))
        }
        guard n >= 0 else { return "" }
        // Read by the COUNT it returned rather than to its NUL: the alphabet is
        // ASCII, so the two agree, and a length cannot be walked off the end.
        return String(decoding: out.prefix(Int(n)).map { UInt8(bitPattern: $0) }, as: UTF8.self)
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
