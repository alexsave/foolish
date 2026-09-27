/* =============================================================================
 * The replay extras format, version 2, AS SHIPPED - frozen
 * =============================================================================
 * This is the TypeScript that encoded and decoded the extras blob from the day
 * the channel was invented until #113 moved the codec into the kernel
 * (c/src/replay_extras.c). It is kept here, and ONLY here, as the oracle the
 * kernel is measured against: replay links are shared and stored, so a code
 * written by any build before this one must still decode, byte for byte.
 *
 * THIS FILE IS NOT AN IMPLEMENTATION AND MUST NEVER BECOME ONE.
 *
 *   - Nothing in `server/`, `src/`, `sdk/` or `offlinefun/` may import it. It
 *     exists to disagree with the kernel, which is worth nothing if it is the
 *     same code.
 *   - It is FROZEN. If the kernel and this file ever disagree, the kernel is
 *     what changed and the kernel is what gets fixed - do not "correct" this
 *     file to match. The only edit it may ever take is a deliberate,
 *     version-bumped format change, and then only alongside the old bytes it
 *     keeps decoding.
 *   - It is deliberately not derived from the kernel by any tooling. A
 *     generated oracle proves the generator ran, not that the format held.
 *
 * The layout it describes is in c/src/replay_extras.h.
 * ========================================================================== */

const EXTRAS_VERSION = 2;
const FLAG_NAMES = 1;
const FLAG_TIMES = 2;

const TIME_B = 1.072;
const TIME_RANGE = Math.pow(TIME_B, 255) - 1; // the curve's reach in units
const MAX_NAME_BYTES = 48;

export interface FrozenExtras {
    names: string[] | null;
    startTime: number | null;
    moveGaps: number[] | null;
}

function unitFor(scaleExp: number): number {
    return Math.pow(2, scaleExp - 64);
}

function pickScaleExp(maxGapSeconds: number): number {
    if (!(maxGapSeconds > 0)) return 64; // degenerate: all-zero gaps, unit = 1 s
    const e = Math.ceil(Math.log2(maxGapSeconds / TIME_RANGE)) + 64;
    return Math.max(0, Math.min(255, e));
}

function quantizeGap(seconds: number, unit: number): number {
    const s = Math.max(0, seconds);
    const v = Math.round(Math.log(1 + s / unit) / Math.log(TIME_B));
    return Math.max(0, Math.min(255, v));
}

function dequantizeGap(v: number, unit: number): number {
    return unit * (Math.pow(TIME_B, v) - 1);
}

function utf8Encode(s: string): number[] {
    return Array.from(new TextEncoder().encode(s));
}

/** The blob the shipped format writes for this roster and these gaps. */
export function frozenEncodeExtras(
    names: string[] | null,
    startTime: number | null,
    gaps: number[] | null,
): Uint8Array {
    const out: number[] = [EXTRAS_VERSION, 0];
    let flags = 0;

    if (names && names.length > 0) {
        flags |= FLAG_NAMES;
        for (const name of names) {
            // Cap by trimming whole code points (Array.from splits on code
            // points, never mid-surrogate-pair).
            let points = Array.from(name.replace(/\0/g, ""));
            let bytes = utf8Encode(points.join(""));
            while (bytes.length > MAX_NAME_BYTES) {
                points = points.slice(0, -1);
                bytes = utf8Encode(points.join(""));
            }
            out.push(...bytes, 0);
        }
    }

    if (startTime !== null && gaps !== null) {
        flags |= FLAG_TIMES;
        const maxGap = gaps.reduce((m, g) => Math.max(m, g), 0);
        const scaleExp = pickScaleExp(maxGap);
        const unit = unitFor(scaleExp);
        out.push(scaleExp);
        const start = Math.max(0, Math.floor(startTime));
        for (let i = 4; i >= 0; i--) out.push(Math.floor(start / 256 ** i) % 256);
        for (const g of gaps) out.push(quantizeGap(g, unit));
    }

    out[1] = flags;
    return Uint8Array.from(out);
}

/** What the shipped format reads back out of `b`. Throws on malformed data. */
export function frozenDecodeExtras(
    b: Uint8Array,
    playerCount: number,
    moveCount: number,
): FrozenExtras {
    if (b.length < 2) throw new Error("extras: truncated header");
    const version = b[0];
    if (version !== EXTRAS_VERSION)
        throw new Error(`extras: unsupported version ${version}`);
    const flags = b[1];
    let pos = 2;

    let names: string[] | null = null;
    if (flags & FLAG_NAMES) {
        names = [];
        for (let p = 0; p < playerCount; p++) {
            const start = pos;
            while (pos < b.length && b[pos] !== 0) pos++;
            if (pos >= b.length) throw new Error("extras: unterminated name");
            names.push(new TextDecoder().decode(b.slice(start, pos)));
            pos++; // skip the NUL
        }
    }

    let startTime: number | null = null;
    let moveGaps: number[] | null = null;
    if (flags & FLAG_TIMES) {
        if (pos + 6 > b.length) throw new Error("extras: truncated time header");
        const unit = unitFor(b[pos++]);
        startTime = 0;
        for (let i = 0; i < 5; i++) startTime = startTime * 256 + b[pos++];
        moveGaps = [];
        for (let i = 0; i < moveCount && pos < b.length; i++) {
            moveGaps.push(dequantizeGap(b[pos++], unit));
        }
        // base32 padding can leave a stray trailing byte; a count mismatch
        // beyond that means a corrupt blob
        if (moveGaps.length < moveCount && moveGaps.length > 0) {
            throw new Error(
                `extras: ${moveGaps.length} gaps for ${moveCount} moves`,
            );
        }
    }

    return { names, startTime, moveGaps };
}
