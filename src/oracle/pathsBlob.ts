/* =============================================================================
 * Infinite Oracle — binary paths-sidecar decoder
 * Decodes the packed little-endian blob og_orc_write_blob (octogen_strategy.c)
 * writes next to each JSONL record. Binary by design: this rides the hot
 * per-batch path (up to fleet-size × ~25 Hz), and a DataView walk is ~free
 * where a JSON encode/parse round-trip is not.
 *
 * Layout (bytes, LE):
 *   header:  u32 magic "OGP1" (0x3150474F); u16 nentries; u8 stride (244); u8 0
 *   entry:   +0 u8 k (index into the record's candidates array)
 *            +1 u8 npaths (≤12)  +2 u8 nreplies (≤3)  +3 u8 pad
 *            +4 u32 n
 *            +8 f32 mepk, oppk, metr, opptr, rnds
 *            +28 reply[3]: u8 type; u8 card; u16 pad; u32 n        (8 B each)
 *            +52 path[12]: u8 len; u8 sym[4]; u8 pad[3]; u32 n; f32 fin (16 B)
 * ========================================================================== */

import { OracleCandPaths, OraclePathStat, OracleReplyStat } from './types';

const OGP1_MAGIC = 0x3150474f;
const HEADER_BYTES = 8;

/** Decode one sidecar blob into candidate-index → path data. Returns an empty
 *  map on any structural mismatch (wrong magic/stride/short buffer) — a
 *  malformed sidecar must never take the analysis down with it. */
export function decodePathsBlob(buf: ArrayBuffer): Map<number, OracleCandPaths> {
    const out = new Map<number, OracleCandPaths>();
    if (!buf || buf.byteLength < HEADER_BYTES) return out;
    const dv = new DataView(buf);
    if (dv.getUint32(0, true) !== OGP1_MAGIC) return out;
    const nentries = dv.getUint16(4, true);
    const stride = dv.getUint8(6);
    if (stride < 52 || HEADER_BYTES + nentries * stride > buf.byteLength) return out;

    for (let i = 0; i < nentries; i++) {
        const e = HEADER_BYTES + i * stride;
        const k = dv.getUint8(e);
        const npaths = Math.min(dv.getUint8(e + 1), 12);
        const nreplies = Math.min(dv.getUint8(e + 2), 3);
        const agg = {
            n: dv.getUint32(e + 4, true),
            mepk: dv.getFloat32(e + 8, true),
            oppk: dv.getFloat32(e + 12, true),
            metr: dv.getFloat32(e + 16, true),
            opptr: dv.getFloat32(e + 20, true),
            rnds: dv.getFloat32(e + 24, true),
        };
        const replies: OracleReplyStat[] = [];
        for (let r = 0; r < nreplies; r++) {
            const re = e + 28 + r * 8;
            replies.push({
                type: dv.getUint8(re),
                card: dv.getUint8(re + 1),
                n: dv.getUint32(re + 4, true),
            });
        }
        const paths: OraclePathStat[] = [];
        for (let p = 0; p < npaths; p++) {
            const pe = e + 52 + p * 16;
            const len = Math.min(dv.getUint8(pe), 4);
            const seq: number[] = [];
            for (let s = 0; s < len; s++) seq.push(dv.getUint8(pe + 1 + s));
            paths.push({ seq, n: dv.getUint32(pe + 8, true), fin: dv.getFloat32(pe + 12, true) });
        }
        out.set(k, { agg, replies, paths });
    }
    return out;
}
