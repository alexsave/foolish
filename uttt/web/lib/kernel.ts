// The uttt kernel in the browser (../c/wasm/uttt_web.c). A thin wrapper and
// nothing more: every answer about the game, the drawing, the motion and the
// words is the kernel's, and this file knows only its function names. It holds
// no layout - a polygon is three questions (where its points start, how many,
// its ink), and the points are one span of floats.

interface Exports {
    memory: WebAssembly.Memory;
    uw_code_ptr(): number;
    uw_code_cap(): number;
    uw_load(): number;
    uw_seek(k: number): number;
    uw_plies(): number;
    uw_motion(animate: number): void;
    uw_rest_ms(): number;
    uw_frame(nowMs: number): number;
    uw_poly_count(): number;
    uw_poly_first(i: number): number;
    uw_poly_len(i: number): number;
    uw_poly_ink(i: number): number;
    uw_points(): number;
    uw_point_count(): number;
    uw_caption(): number;
    uw_paper(side: number): number;
}

export interface Frame {
    /** Still animating: ask again next display frame. */
    running: boolean;
    /** x, y pairs, 0..1 of the board. */
    points: Float32Array;
    polys: { first: number; n: number; fill: string }[];
}

export interface Replay {
    plies: number;
    /** Move the position to after `k` plies, at rest. */
    seek(k: number): void;
    /** Play the last ply of the current position from the start of its motion. */
    animate(): void;
    /** The board `nowMs` into the current motion. */
    frame(nowMs: number): Frame;
    /** The kernel's line for the current position. */
    caption(): string;
    /** How long a settled move rests before the next one may start. */
    restMs: number;
    /** The napkin, as an image the page can put under everything. */
    paper(side: number): ImageData;
}

let module: Promise<WebAssembly.Module> | null = null;

async function compile(): Promise<WebAssembly.Module> {
    const res = await fetch('/uttt.wasm');
    if (!res.ok) throw new Error(`uttt.wasm: ${res.status}`);
    return WebAssembly.compile(await res.arrayBuffer());
}

const hex = (rgba: number) => '#' + (rgba >>> 0).toString(16).padStart(8, '0');

/** A replay of `code`, or null when the code is not a game. */
export async function openReplay(code: string): Promise<Replay | null> {
    module ??= compile();
    const instance = await WebAssembly.instantiate(await module, {});
    const w = instance.exports as unknown as Exports;

    const bytes = new TextEncoder().encode(code);
    if (bytes.length + 1 > w.uw_code_cap()) return null;
    const mem = new Uint8Array(w.memory.buffer, w.uw_code_ptr(), bytes.length + 1);
    mem.set(bytes);
    mem[bytes.length] = 0;
    const plies = w.uw_load();
    if (!plies) return null;

    const text = (p: number) => {
        const all = new Uint8Array(w.memory.buffer);
        let e = p;
        while (all[e]) e++;
        return new TextDecoder().decode(all.subarray(p, e));
    };

    return {
        plies,
        restMs: w.uw_rest_ms(),
        seek: (k) => void w.uw_seek(k),
        animate: () => w.uw_motion(1),
        frame(nowMs) {
            const running = w.uw_frame(Math.max(0, Math.round(nowMs))) !== 0;
            const points = new Float32Array(w.memory.buffer, w.uw_points(), w.uw_point_count() * 2).slice();
            const polys = [];
            for (let i = 0, n = w.uw_poly_count(); i < n; i++)
                polys.push({ first: w.uw_poly_first(i), n: w.uw_poly_len(i), fill: hex(w.uw_poly_ink(i)) });
            return { running, points, polys };
        },
        caption: () => text(w.uw_caption()),
        paper(side) {
            const p = w.uw_paper(side);
            const px = new Uint8ClampedArray(w.memory.buffer, p, side * side * 4).slice();
            return new ImageData(px, side, side);
        },
    };
}
