/* =============================================================================
 * foolish.cards — game replay codec kernel (rules-independent)
 * =============================================================================
 * Encodes an ENTIRE Durak game as one big integer -> bytes -> base32/base64 ->
 * a short string / URL you can put in a QR code. The same deterministic engine
 * drives encode and decode (see core.ts), so the round trip is correct by
 * construction.
 *
 * This file contains everything that is INDEPENDENT of the game rules:
 *   - the exact rANS coder kernel (native BigInt — no bignum library needed)
 *   - the Coder driver (one primitive: code(weights, chosen?))
 *   - integer <-> bytes <-> base32/base64 <-> URL
 *   - URL routing (legacy short code vs. self-contained replay)
 *   - CNS subset ranking (reserved for the v2 "set-coded reveal" optimization)
 *
 * The game-rules projection lives in the C kernel (c/src/replay.c),
 * reached through encode.ts/decode.ts + sdk/ts/wasm/engine.ts. The Coder
 * class below remains the TS-side arithmetic-coding primitive for the
 * frozen oracle (e2e/replay_ts_oracle.ts) and any rules-free side channels.
 * ========================================================================== */

/* ----------------------------------------------------------------------------
 * BYTES, BASE32/BASE64, HEX, URL ROUTING
 * ----------------------------------------------------------------------------
 * The rANS arithmetic coder used to sit above this line - a full second
 * implementation of the one in c/src/replay.c. It was kept for a frozen
 * differential oracle, e2e/replay_ts_oracle.ts, which no longer exists; the
 * class had no caller anywhere, in production or in a test. The kernel codes
 * replays (wasm_replay_encode / _decode / _encode_v6), and there is no second
 * coder now.
 *
 * What is left is genuinely host-shaped and has no C twin: turning bytes into
 * a hex column value, a base32 URL segment, or a base64 blob.
 * ------------------------------------------------------------------------- */

const B32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
const B32_INV: Record<string, number> = (() => {
  const m: Record<string, number> = {};
  for (let i = 0; i < B32.length; i++) m[B32[i]] = i;
  return m;
})();
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
  out.reverse(); // big-endian, minimal
  return Uint8Array.from(out);
}

export function bytesToBigint(b: Uint8Array): bigint {
  let x = 0n;
  for (const byte of b) x = (x << 8n) | BigInt(byte);
  return x;
}

export function base32Encode(bytes: Uint8Array): string {
  let bits = 0,
    value = 0,
    out = "";
  for (const b of bytes) {
    value = (value << 8) | b;
    bits += 8;
    while (bits >= 5) {
      out += B32[(value >>> (bits - 5)) & 31];
      bits -= 5;
    }
  }
  if (bits > 0) out += B32[(value << (5 - bits)) & 31];
  return out;
}

export function base32Decode(s: string): Uint8Array {
  let bits = 0,
    value = 0;
  const out: number[] = [];
  for (const ch of s.toUpperCase()) {
    const idx = B32_INV[ch];
    if (idx === undefined) continue; // ignore stray chars
    value = (value << 5) | idx;
    bits += 5;
    if (bits >= 8) {
      out.push((value >>> (bits - 8)) & 0xff);
      bits -= 8;
    }
  }
  return Uint8Array.from(out);
}

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

export const URL_PREFIX = "WWW.FOOLISH.CARDS/";

function gameToCode(x: bigint): string {
  return base32Encode(bigintToBytes(x));
}
export function codeToGame(code: string): bigint {
  return bytesToBigint(base32Decode(code));
}
export function gameToUrl(x: bigint): string {
  return URL_PREFIX + gameToCode(x);
}
/** The public host, without the `www.` a browser hides and a person omits. */
const URL_HOST = "FOOLISH.CARDS/";
const IS_BASE32 = /^[A-Za-z2-7]+$/;

/**
 * The replay code out of whatever a person pasted.
 *
 * Accepts the bare code, the printed form `WWW.FOOLISH.CARDS/<code>`, and the
 * links a browser actually hands out - `https://foolish.cards/<code>`,
 * `https://www.foolish.cards/<code>`, scheme or no scheme, trailing slash,
 * query or fragment. Whitespace anywhere is dropped: a code that survived a
 * line wrap is still that code.
 *
 * Does NOT decide whether the result is a code; see urlToGame.
 */
export function urlToCode(url: string): string {
  // A query or a fragment is never part of the code.
  let s = url.trim().replace(/\s+/g, "").split(/[?#]/)[0].replace(/\/+$/, "");
  const host = s.toUpperCase().lastIndexOf(URL_HOST);
  if (host >= 0) s = s.slice(host + URL_HOST.length);
  // Some other host, or a bare path: the code is the last path segment. A
  // scheme's own letters are in the base32 alphabet, so leaving `https:/` on
  // the front does not fail - it silently decodes a DIFFERENT game.
  else if (s.includes("/")) s = s.slice(s.lastIndexOf("/") + 1);
  // an optional extras section (player names + move times, see extras.ts)
  // follows the moves after a dash - the move integer is the prefix
  const dash = s.indexOf("-");
  if (dash >= 0) s = s.slice(0, dash);
  return s;
}

export function urlToGame(url: string): bigint {
  const code = urlToCode(url);
  // Name the fault by what it IS. base32Decode ignores stray characters, so
  // input that is not a code used to decode to some other game and fail deep
  // in the kernel as "unsupported replay format version 11" - sending the
  // reader after a codec bug that was never there. Same principle as
  // REPLAY_EHEADER and REPLAY_ETOOLONG in c/src/replay.h.
  if (!IS_BASE32.test(code)) {
    throw new Error(
      `not a replay code: ${JSON.stringify(url)} - expected a foolish.cards link or the code out of one`,
    );
  }
  return codeToGame(code);
}

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
