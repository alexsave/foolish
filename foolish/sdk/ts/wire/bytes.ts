// Base64 for the web: the session blob it keeps in localStorage
// (src/common/backend.ts). Pure TS, no wasm imports; the bytes are the kernel's.

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

