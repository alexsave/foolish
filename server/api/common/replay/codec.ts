/* =============================================================================
 * foolish.cards — game replay codec kernel (rules-independent)
 * =============================================================================
 * Encodes an ENTIRE Durak game as one big integer -> bytes -> base32/base64 ->
 * a short string / URL you can put in a QR code. The same deterministic engine
 * drives encode and decode (see core.ts), so the round trip is correct by
 * construction.
 *
 * What is left here is what the kernel does NOT do:
 *   - integer <-> bytes, and base64
 *   - hex (a Postgres column format C has no reason to know)
 *   - READING a pasted link back (urlToCode / urlToGame) and deciding a path
 *     segment's type - replay_extras.h keeps the URL *type* platform-side
 *
 * Everything the kernel does do has gone to it: the rANS coder (c/src/replay.c,
 * which always had its own), base32 (replay_b32_encode / _decode) and the link
 * builders (replay_extras_link_styled).
 * ========================================================================== */

/* ----------------------------------------------------------------------------
 * BYTES, BASE32/BASE64, HEX, URL ROUTING
 * ----------------------------------------------------------------------------
 * The rANS arithmetic coder used to sit above this line - a full second
 * implementation of the one in c/src/replay.c. It was kept for a frozen
 * differential oracle, e2e/replay_ts_oracle.ts, which no longer exists; the
 * class had no caller anywhere, in production or in a test. The kernel codes
 * replays (wasm_replay_encode_v6 / _decode), and there is no second
 * coder now.
 *
 * What is left is genuinely host-shaped and has no C twin: turning bytes into
 * a hex column value, a base32 URL segment, or a base64 blob.
 * ------------------------------------------------------------------------- */


const B64 =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const B64_INV: Record<string, number> = (() => {
  const m: Record<string, number> = {};
  for (let i = 0; i < B64.length; i++) m[B64[i]] = i;
  return m;
})();

export function bigintToBytes(x: bigint): Uint8Array {
  if (x < 0n) throw new Error("negative");
  if (x === 0n) return new Uint8Array([0]);
  const out: number[] = [];
  while (x > 0n) {
    out.push(Number(x & 0xffn));
    x >>= 8n;
  }
  return new Uint8Array(out.reverse());
}

export function bytesToBigint(b: Uint8Array): bigint {
  let x = 0n;
  for (const byte of b) x = (x << 8n) | BigInt(byte);
  return x;
}

// base32 lived here, with its alphabet. It is the kernel's
// (replay.c replay_b32_encode/decode, reached through bots.ts kernelB32Encode /
// kernelB32Decode) - replay.h had always described the C one as "the web's
// codec.ts alphabet", which is a mirror naming its original.
//
export function base64Encode(bytes: Uint8Array): string {
  let out = "";
  for (let i = 0; i < bytes.length; i += 3) {
    const b0 = bytes[i];
    const b1 = i + 1 < bytes.length ? bytes[i + 1] : 0;
    const b2 = i + 2 < bytes.length ? bytes[i + 2] : 0;
    out += B64[b0 >> 2];
    out += B64[((b0 & 3) << 4) | (b1 >> 4)];
    if (i + 1 < bytes.length) out += B64[((b1 & 15) << 2) | (b2 >> 6)];
    if (i + 2 < bytes.length) out += B64[b2 & 63];
  }
  return out;
}

export function base64Decode(s: string): Uint8Array {
  const out: number[] = [];
  let value = 0,
    bits = 0;
  for (const ch of s) {
    const idx = B64_INV[ch];
    if (idx === undefined) continue; // ignore padding / stray chars
    value = (value << 6) | idx;
    bits += 6;
    if (bits >= 8) {
      out.push((value >>> (bits - 8)) & 0xff);
      bits -= 8;
    }
  }
  return Uint8Array.from(out);
}

/* Keep the whole URL uppercase: lower-casing the prefix would force the QR out
 * of alphanumeric mode into byte mode and grow the symbol. The HTTP route can
 * still be case-insensitive for typed/clicked links. */
/* Postgres bytea travels through supabase-js as hex strings ("\\x48ab...").
 * These helpers convert both directions for the binary snapshot columns. */
export function bytesToHex(bytes: Uint8Array): string {
  let out = "\\x";
  for (const b of bytes) out += b.toString(16).padStart(2, "0");
  return out;
}

export function hexToBytes(hex: string): Uint8Array {
  const h = hex.startsWith("\\x") ? hex.slice(2) : hex;
  const out = new Uint8Array(h.length >> 1);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(h.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

// The link builders lived here: URL_PREFIX, gameToCode, gameToUrl, codeToGame,
// urlToCode, urlToGame. They are the kernel's now (replay_extras.c
// replay_extras_link_styled, through bots.ts kernelReplayLink), which already
// assembled the same string for the /m/ route while these built it by
// concatenation for everything else - and built it DIFFERENTLY, uppercase and
// scheme-less, so the same replay copied from two screens gave two links.
//
// Both forms survive because both are wanted, and the kernel owns the choice:
// REPLAY_LINK.url is the https link a person copies, REPLAY_LINK.qr is the
// uppercase scheme-less one, which stays in QR alphanumeric mode and so fits a
// smaller QR version.
//
// urlToCode and urlToGame lived here: strip the prefix, refuse anything that
// is not base32, then decode. Both are the kernel's now (replay_extras.c
// replay_link_parse, through bots.ts kernelReplayLinkParse) - building a link
// and reading one back are two halves of one format, and the refusal is the
// half a tolerant decoder cannot do.
//
// classifyPathSegment stays: replay_extras.h keeps the URL *type* platform-side.

/* ----------------------------------------------------------------------------
 * 4. ROUTING  (legacy short code vs. self-contained replay)
 * ----------------------------------------------------------------------------
 * Legacy: WWW.FOOLISH.CARDS/abc123  -> short DB code (<= ~6-7 chars), loads a
 *         specific saved game by id.
 * New:    WWW.FOOLISH.CARDS/<base32>  -> the path IS the entire game; no DB.
 *
 * Length-based dispatch works but is fragile (see files/TODO.md decision 1 —
 * consider a /r/ namespace before replay URLs are shared publicly).
 * -------------------------------------------------------------------------- */

const LEGACY_MAX_LEN = 7;

export function classifyPathSegment(seg: string): "shortcode" | "replay" {
  return seg.length > LEGACY_MAX_LEN ? "replay" : "shortcode";
}
