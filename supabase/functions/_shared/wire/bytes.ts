// Byte helpers for the packed wire formats (docs/PACKED_WIRE_CUTOVER.md).
// Pure TS, no wasm imports — shared by edge functions, the web client and
// the e2e harness. Base64 is the transport encoding for packed buffers that
// must ride inside a JSON envelope (the realtime broadcast API is JSON);
// the bytes themselves are kernel-produced.

export function bytesToBase64(bytes: Uint8Array): string {
    // btoa exists in Deno edge, browsers and Node >= 16. Chunk the
    // fromCharCode spread — a whole event buffer in one call can blow the
    // argument-count limit.
    let bin = '';
    const CHUNK = 0x2000;
    for (let i = 0; i < bytes.length; i += CHUNK) {
        bin += String.fromCharCode(...bytes.subarray(i, i + CHUNK));
    }
    return btoa(bin);
}

export function base64ToBytes(b64: string): Uint8Array {
    const bin = atob(b64);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
}

// Bare hex (NO \x prefix) — the encoding of append-only hex columns
// (games.logs_packed): appends must be plain string concatenation, which a
// prefixed representation would corrupt. hexToBytes accepts both forms.
export function bytesToBareHex(bytes: Uint8Array): string {
    let out = '';
    for (const b of bytes) out += b.toString(16).padStart(2, '0');
    return out;
}

