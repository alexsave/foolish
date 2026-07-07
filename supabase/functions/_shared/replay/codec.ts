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
 * The game-rules projection lives in the C kernel (cnitro/src/replay.c),
 * reached through encode.ts/decode.ts + _shared/wasm/engine.ts. The Coder
 * class below remains the TS-side arithmetic-coding primitive for the
 * frozen oracle (e2e/replay_ts_oracle.ts) and any rules-free side channels.
 * ========================================================================== */

/* ----------------------------------------------------------------------------
 * 1. rANS KERNEL  (range Asymmetric Numeral System over a single BigInt)
 * ----------------------------------------------------------------------------
 * A "symbol" is a choice of option k out of n, where option i has integer
 * weight w[i] > 0, total M = sum(w), and cumulative cum[k] = sum(w[0..k-1]).
 * The realized symbol costs ~log2(M / w[k]) bits.
 *
 * push (encode) is LIFO; pop (decode) is FIFO. So to encode a forward sequence
 * of choices we push them in REVERSE, and the decoder pops them FORWARD.
 * A conforming decoder ends with x === 0n.
 * -------------------------------------------------------------------------- */

function ransPush(x: bigint, cum: number, w: number, M: number): bigint {
  const W = BigInt(w);
  const r = x % W; // x mod w
  const q = x / W; // x div w
  return q * BigInt(M) + BigInt(cum) + r;
}

function ransPop(
  x: bigint,
  weights: number[],
): { index: number; x: bigint } {
  let M = 0;
  for (const w of weights) M += w;
  const r = Number(x % BigInt(M)); // r < M (safe: keep M < 2^53)
  let acc = 0;
  let k = 0;
  // find the interval [cum[k], cum[k]+w[k]) that contains r
  for (; k < weights.length; k++) {
    if (r < acc + weights[k]) break;
    acc += weights[k];
  }
  if (k >= weights.length) k = weights.length - 1; // defensive
  const nx = (x / BigInt(M)) * BigInt(weights[k]) + BigInt(r - acc);
  return { index: k, x: nx };
}

/* ----------------------------------------------------------------------------
 * 2. THE CODER  (drives the engine in either direction)
 * ----------------------------------------------------------------------------
 * The engine calls exactly one method at every point where information enters
 * the game — a player's decision OR a hidden card becoming known (a "reveal"):
 *
 *     const index = coder.code(weights, chosenIndexInEncodeMode);
 *
 *   ENCODE: you are replaying a KNOWN game. The engine computes the legal-move
 *           menu, finds the index of the move that was actually played, and
 *           passes it as `chosen`. code() records it and returns it unchanged.
 *   DECODE: you are reconstructing the game from the integer. The engine
 *           computes the SAME menu, calls code(weights) with no `chosen`, gets
 *           back the index, and applies that move.
 *
 * Because the menu (options + weights + order) is a pure function of the public
 * game state, both directions see identical menus and stay in lockstep.
 * -------------------------------------------------------------------------- */

interface RecordedChoice {
  weights: number[];
  chosen: number;
}

export class Coder {
  readonly mode: "encode" | "decode";
  private recorded: RecordedChoice[] = []; // encode only
  private x: bigint = 0n; // decode only

  private constructor(mode: "encode" | "decode", x: bigint) {
    this.mode = mode;
    this.x = x;
  }

  static forEncode(): Coder {
    return new Coder("encode", 0n);
  }
  static forDecode(x: bigint): Coder {
    return new Coder("decode", x);
  }

  /** The single primitive. weights: positive integers. */
  code(weights: number[], chosen?: number): number {
    if (weights.length === 0) throw new Error("empty menu");
    if (weights.length === 1) {
      // forced move: 0 bits, nothing to code, but keep both sides symmetric
      return 0;
    }
    if (this.mode === "encode") {
      if (chosen === undefined || chosen < 0 || chosen >= weights.length)
        throw new Error("encode: chosen index out of range");
      this.recorded.push({ weights: weights.slice(), chosen });
      return chosen;
    } else {
      const { index, x } = ransPop(this.x, weights);
      this.x = x;
      return index;
    }
  }

  /** Uniform menu of n options. */
  codeUniform(n: number, chosen?: number): number {
    if (n === 1) return 0;
    return this.code(new Array(n).fill(1), chosen);
  }

  /** ENCODE: collapse the recorded choices into the final integer. */
  finishEncode(): bigint {
    if (this.mode !== "encode") throw new Error("not in encode mode");
    let x = 0n;
    for (let i = this.recorded.length - 1; i >= 0; i--) {
      const c = this.recorded[i];
      let M = 0,
        cum = 0;
      for (let j = 0; j < c.weights.length; j++) {
        if (j < c.chosen) cum += c.weights[j];
        M += c.weights[j];
      }
      x = ransPush(x, cum, c.weights[c.chosen], M);
    }
    return x;
  }

  /** DECODE: a conforming game consumes the integer exactly. Check AFTER the
   *  engine has run to its own terminal state (the integer does NOT tell the
   *  engine when to stop — the rules do). */
  residueIsZero(): boolean {
    return this.x === 0n;
  }

  /** Diagnostics: ideal (information-theoretic) size of the encoded choices. */
  idealBits(): number {
    let bits = 0;
    for (const c of this.recorded) {
      let M = 0;
      for (const w of c.weights) M += w;
      bits += Math.log2(M / c.weights[c.chosen]);
    }
    return bits;
  }
}


/* ----------------------------------------------------------------------------
 * 3. INTEGER <-> BYTES <-> BASE32 / BASE64 <-> URL
 * ----------------------------------------------------------------------------
 * base32 = RFC 4648 uppercase alphabet (A-Z, 2-7), NO padding. Every character
 * is in the QR "alphanumeric" set, as are '.' and '/' and uppercase letters in
 * the prefix — so the whole URL encodes in QR alphanumeric mode (5.5 bits/char,
 * much denser than byte mode). Use QR error-correction level L for smallest QR.
 *
 * base64 = RFC 4648 standard alphabet, no padding — denser as on-screen text
 * and the natural form for a future DB column. Both wrap the same bytes.
 * -------------------------------------------------------------------------- */

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
export function urlToGame(url: string): bigint {
  const i = url.toUpperCase().indexOf(URL_PREFIX);
  let code = i >= 0 ? url.slice(i + URL_PREFIX.length) : url;
  // an optional extras section (player names + move times, see extras.ts)
  // follows the moves after a dash — the move integer is the prefix
  const dash = code.indexOf("-");
  if (dash >= 0) code = code.slice(0, dash);
  return codeToGame(code.replace(/[^A-Za-z2-7]/g, ""));
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

/* ----------------------------------------------------------------------------
 * 5. CNS — combinatorial number system (reserved for v2 set-coded reveal)
 * ----------------------------------------------------------------------------
 * Colexicographic rank/unrank of a k-subset of [0, m). Lets you reveal a
 * player's acquired hand as an UNORDERED set in exactly log2(C(m,k)) bits.
 * C(52,26) < 2^53, so plain numbers are safe up to a 52-card deck.
 * -------------------------------------------------------------------------- */

const COMB: number[][] = (() => {
  const N = 64;
  const c: number[][] = Array.from({ length: N }, () => new Array(N).fill(0));
  for (let n = 0; n < N; n++) {
    c[n][0] = 1;
    for (let k = 1; k <= n; k++)
      c[n][k] = c[n - 1][k - 1] + (k <= n - 1 ? c[n - 1][k] : 0);
  }
  return c;
})();

export function comb(n: number, k: number): number {
  if (k < 0 || k > n) return 0;
  return COMB[n][k];
}


