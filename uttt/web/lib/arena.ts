// The 243 page's kernel (../c/wasm/uttt_243_web.c): two greedy bots playing
// the 243 x 243 game. A thin wrapper like lib/kernel.ts - every move, every
// score and every node's place on the board is the kernel's. This file knows
// the kernel's function names and the seed's spelling, and NO BYTE LAYOUT:
// the structs it reads (uttt_243_web.h) come through readers structgen writes
// from that header (./gen, a build output), and the module's layout hash is
// held against theirs before the first call. It runs anywhere a WebAssembly
// module does (the page, and node in test/arena.test.mjs).

import { LAYOUT_HASH } from './gen/arena_layout_hash';
import {
    memOf, readUaBox, readUaConfig, readUaStatus,
    type UaBox_Snap, type UaConfig_Snap, type UaStatus_Snap,
} from './gen/arena_layout';

export { UA_OPEN, UA_X, UA_O, UA_DRAW } from './gen/arena_layout';
export type Box = UaBox_Snap;
export type Status = UaStatus_Snap;
export type Config = UaConfig_Snap;

/** Function names, and nothing about memory but where things start. */
export interface ArenaExports {
    memory: WebAssembly.Memory;
    ua_layout_hash(): number;
    ua_start(depth: number, hi: number, lo: number): number;
    ua_think(): number;
    ua_play(mv: number): number;
    ua_step(n: number): number;
    ua_legal_at(mv: number): number;
    ua_grid(): number;
    ua_nodes(): number;
    ua_cells(): number;
    ua_status(): number;
    ua_config(): number;
    ua_box(id: number): number;
    ua_level(id: number): number;
}

export interface Arena {
    /** The raw exports, for a test that asks the kernel directly. */
    w: ArenaExports;
    /** A fresh game at `depth` (5 on the page) from a 16-hex-digit seed. */
    start(seed: string, depth?: number): void;
    /** Up to `n` bot moves; how many were played (fewer when the game ends). */
    step(n: number): number;
    /** The board as a picture: side x side bytes, row-major, UA_OPEN / UA_X /
     *  UA_O. A view into the kernel's memory, valid until the next call. */
    grid(): Uint8Array;
    /** Every internal node's status (UA_OPEN, UA_X, UA_O, UA_DRAW), by id. */
    nodes(): Uint8Array;
    /** The game now. */
    status(): Status;
    /** The game's shape and the bots' settings. */
    config(): Config;
    /** Where node `id` sits, in cells, and its level (0 the root). */
    box(id: number): Box;
    level(id: number): number;
}

/** A seed is 64 bits, written as 16 lowercase hex digits. */
export function parseSeed(s: string | null | undefined): string | null {
    if (!s) return null;
    const t = s.trim().toLowerCase();
    return /^[0-9a-f]{1,16}$/.test(t) ? t.padStart(16, '0') : null;
}

/** A fresh seed from the browser's (or node's) secure random. */
export function randomSeed(): string {
    const v = new Uint32Array(2);
    crypto.getRandomValues(v);
    return Array.from(v, (x) => x.toString(16).padStart(8, '0')).join('');
}

const hex = (h: number) => `0x${(h >>> 0).toString(16).padStart(8, '0')}`;

export async function instantiateArena(source: BufferSource | WebAssembly.Module): Promise<Arena> {
    const mod = source instanceof WebAssembly.Module ? source : await WebAssembly.compile(source);
    const instance = await WebAssembly.instantiate(mod, {});
    const w = instance.exports as unknown as ArenaExports;
    const got = w.ua_layout_hash() >>> 0;
    if (got !== LAYOUT_HASH >>> 0)
        throw new Error(`uttt243.wasm was built for layout ${hex(got)}, but lib/gen/arena_layout.ts reads ` +
            `${hex(LAYOUT_HASH)}: the two came from different headers. Rebuild both: npm run wasm.`);
    // the memory never grows (nothing in the kernel allocates), but a view is
    // cheap, so each read takes a fresh one rather than trusting that
    const mem = () => memOf(w.memory.buffer);
    let leaves = 0, nodes = 0;
    return {
        w,
        start(seed, depth = 5) {
            const s = parseSeed(seed);
            if (!s) throw new Error(`not a seed: ${seed}`);
            if (!w.ua_start(depth, parseInt(s.slice(0, 8), 16), parseInt(s.slice(8), 16)))
                throw new Error(`not a depth: ${depth}`);
            const c = readUaConfig(mem(), w.ua_config());
            leaves = c.leaves;
            nodes = c.nodes;
        },
        step: (n) => w.ua_step(n),
        grid: () => new Uint8Array(w.memory.buffer, w.ua_grid(), leaves),
        nodes: () => new Uint8Array(w.memory.buffer, w.ua_nodes(), nodes),
        status: () => readUaStatus(mem(), w.ua_status()),
        config: () => readUaConfig(mem(), w.ua_config()),
        box: (id) => readUaBox(mem(), w.ua_box(id)),
        level: (id) => w.ua_level(id),
    };
}

/** The page's arena: /uttt243.wasm. */
export async function loadArena(): Promise<Arena> {
    const res = await fetch('/uttt243.wasm');
    if (!res.ok) throw new Error(`uttt243.wasm: ${res.status}`);
    return instantiateArena(await res.arrayBuffer());
}
