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

// Little-endian u32 (the version field of the packed HTTP envelopes).
export function putU32(out: number[], v: number): void {
    out.push(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff);
}

export function readU32(buf: Uint8Array, off: number): number {
    // >>> 0 keeps versions above 2^31 unsigned.
    return (buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16) | (buf[off + 3] << 24)) >>> 0;
}
