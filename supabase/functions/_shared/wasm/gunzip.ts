// Synchronous gunzip for the gzip+base64 wasm embeds (rules/guards). Uses the
// vendored tiny-inflate — pure JS, sync, and a relative import, so it works
// identically in the browser, Node and the Deno edge with zero npm/import-map
// plumbing (unlike fflate) and no async DecompressionStream.
// @ts-ignore — vendored plain-JS module, typed as (src, dest) => Uint8Array
import tinf_uncompress from './tiny_inflate.mjs';

export function gunzip(gz: Uint8Array): Uint8Array {
    if (gz[0] !== 0x1f || gz[1] !== 0x8b) throw new Error('not a gzip stream');
    const flg = gz[3];
    let p = 10;                                        // fixed header
    if (flg & 0x04) p += 2 + (gz[p] | (gz[p + 1] << 8)); // FEXTRA
    if (flg & 0x08) while (gz[p++] !== 0) { /* FNAME */ }
    if (flg & 0x10) while (gz[p++] !== 0) { /* FCOMMENT */ }
    if (flg & 0x02) p += 2;                            // FHCRC
    const n = gz.length;
    // gzip trailer: CRC32 (4) + ISIZE (4, uncompressed length mod 2^32, LE).
    const isize = (gz[n - 4] | (gz[n - 3] << 8) | (gz[n - 2] << 16) | (gz[n - 1] << 24)) >>> 0;
    const out = new Uint8Array(isize);
    tinf_uncompress(gz.subarray(p, n - 8), out);
    return out;
}
