// The uttt kernel at BUILD time, in Node: the same wasm the replay page runs
// in the browser (public/uttt.wasm), opened from disk to draw the napkin once,
// as a static PNG, for every page to sit on.
//
// Like lib/kernel.ts it knows function names and nothing else
// (lib/kernel-exports.ts). This is the ONLY place the napkin becomes an image:
// every page shows /napkin.png (app/globals.css).

import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { deflateSync } from 'node:zlib';
import type { UtttExports } from './kernel-exports';

let kernel: UtttExports | null = null;

/** PNG's CRC-32, table-driven (node:zlib only has one from Node 20.15/22.2). */
const CRC = Array.from({ length: 256 }, (_, n) => {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
});
function crc32(b: Uint8Array): number {
    let c = 0xffffffff;
    for (const x of b) c = CRC[(c ^ x) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
}

function open(): UtttExports {
    if (kernel) return kernel;
    const bytes = readFileSync(join(process.cwd(), 'public', 'uttt.wasm'));
    const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), {});
    kernel = instance.exports as unknown as UtttExports;
    return kernel;
}


/** The napkin (uttt_paper) at side x side, as a PNG. */
export function napkinPng(side: number): Buffer {
    const w = open();
    const px = new Uint8Array(w.memory.buffer, w.uw_paper(side), side * side * 4);
    const raw = Buffer.alloc(side * (side * 4 + 1));
    for (let y = 0; y < side; y++) {
        raw[y * (side * 4 + 1)] = 0; // filter: none
        raw.set(px.subarray(y * side * 4, (y + 1) * side * 4), y * (side * 4 + 1) + 1);
    }
    const chunk = (type: string, data: Buffer) => {
        const len = Buffer.alloc(4);
        len.writeUInt32BE(data.length);
        const td = Buffer.concat([Buffer.from(type, 'latin1'), data]);
        const crc = Buffer.alloc(4);
        crc.writeUInt32BE(crc32(td));
        return Buffer.concat([len, td, crc]);
    };
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(side, 0);
    ihdr.writeUInt32BE(side, 4);
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // RGBA
    return Buffer.concat([
        Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
        chunk('IHDR', ihdr),
        chunk('IDAT', deflateSync(raw, { level: 9 })),
        chunk('IEND', Buffer.alloc(0)),
    ]);
}
