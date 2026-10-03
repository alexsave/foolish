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

export {
    UA_OPEN, UA_X, UA_O, UA_DRAW, UA_PLIES_MIN, UA_PLIES_MAX,
    UA_BUDGET_SMALL, UA_BUDGET_MED, UA_BUDGET_LARGE, UA_BUDGET_HUGE, UA_DEFAULT_PLIES, UA_DEFAULT_BUDGET,
} from './gen/arena_layout';
export type Box = UaBox_Snap;
export type Status = UaStatus_Snap;
export type Config = UaConfig_Snap;

/** Function names, and nothing about memory but where things start. */
export interface ArenaExports {
    memory: WebAssembly.Memory;
    ua_layout_hash(): number;
    ua_start(depth: number, hi: number, lo: number): number;
    ua_set_bots(plies: number, budget: number): void;
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
    /** Both bots look `plies` ahead with `budget` work units a move, and the
     *  game starts again from move 0 on the same seed (the kernel clamps both
     *  and derives the candidate caps). */
    setBots(plies: number, budget: number): void;
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

/** THE GAME'S CLOCK: wall time while the bots are playing, from the first
 *  frame that plays to the frame that sees the game end, and the moves those
 *  frames played. It belongs to the game, not to the loop that drives it, so
 *  a loop started again over a finished game (React runs an effect again
 *  when a hidden page is shown) adds nothing - the first page divided a
 *  whole game's moves by the new loop's first millisecond and read
 *  32,502,000 moves a second. A gap between frames longer than MAX_GAP_MS
 *  (a hidden tab, a stalled machine) counts as MAX_GAP_MS. */
export class GameClock {
    static readonly MAX_GAP_MS = 250;
    /** Below this much play the rate is not stated: a game the fastest
     *  setting finishes in a few frames still has a few frames of time. */
    static readonly MIN_RATE_MS = 20;
    ms = 0;
    plies = 0;
    over = false;
    private prev = -1;

    /** A frame at `now` (performance.now()) that left the game at `plies`. */
    frame(now: number, plies: number, over: boolean) {
        if (this.over) return;
        if (this.prev >= 0) this.ms += Math.min(Math.max(now - this.prev, 0), GameClock.MAX_GAP_MS);
        this.prev = now;
        this.plies = plies;
        this.over = over;
    }

    /** The loop stopped (an effect's cleanup): the next frame starts a new
     *  span, so the pause is not counted. */
    pause() { this.prev = -1; }

    /** Moves a second over the whole game so far, or null before there is
     *  enough time to divide by. */
    rate(): number | null {
        return this.ms >= GameClock.MIN_RATE_MS ? (this.plies * 1000) / this.ms : null;
    }
}

/** A game's time: "0:42.5" while it runs, to the millisecond once it is over
 *  ("2.143 s", "1:07.250"). */
export function gameTime(ms: number, over: boolean): string {
    const s = Math.max(ms, 0) / 1000, m = Math.floor(s / 60);
    if (!over) return `${m}:${(s - m * 60).toFixed(1).padStart(4, '0')}`;
    return m ? `${m}:${(s - m * 60).toFixed(3).padStart(6, '0')}` : `${s.toFixed(3)} s`;
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
        setBots: (plies, budget) => w.ua_set_bots(plies, budget),
        step: (n) => w.ua_step(n),
        grid: () => new Uint8Array(w.memory.buffer, w.ua_grid(), leaves),
        nodes: () => new Uint8Array(w.memory.buffer, w.ua_nodes(), nodes),
        status: () => readUaStatus(mem(), w.ua_status()),
        config: () => readUaConfig(mem(), w.ua_config()),
        box: (id) => readUaBox(mem(), w.ua_box(id)),
        level: (id) => w.ua_level(id),
    };
}

/** WHY THE PAGE COULD NOT START, for the page to say: no WebAssembly at all
 *  is the one sentence; anything else is that sentence and the error's own
 *  words (a fetch status, the layout refusal, an instantiate error such as an
 *  unsupported opcode), so a report from a visitor names what failed. */
export function startError(e: unknown, hasWasm = typeof WebAssembly !== 'undefined'): { lead: string; detail: string | null } {
    const lead = 'This browser could not start the game';
    if (!hasWasm) return { lead: `${lead} (it needs WebAssembly).`, detail: null };
    const msg = e instanceof Error ? `${e.name === 'Error' ? '' : `${e.name}: `}${e.message}` : String(e);
    return { lead: `${lead}.`, detail: msg.trim() || 'an unknown error' };
}

/** The page's arena: /uttt243.wasm, revalidated on every load (a 304 when it
 *  has not changed): a cached module from an older build beside this build's
 *  readers is refused by the layout check, and the page would not start. */
export async function loadArena(): Promise<Arena> {
    if (typeof WebAssembly === 'undefined') throw new Error('no WebAssembly');
    const res = await fetch('/uttt243.wasm', { cache: 'no-cache' });
    if (!res.ok) throw new Error(`uttt243.wasm: ${res.status}`);
    return instantiateArena(await res.arrayBuffer());
}
