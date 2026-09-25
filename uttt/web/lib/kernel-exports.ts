// What uttt.wasm exports (../c/wasm/uttt_web.c), declared ONCE for both of
// its hosts: the browser (lib/kernel.ts, the replay) and the build (lib/
// kernel-build.ts, the about page and the napkin). Function names and nothing
// else - no layout, no offsets.

export interface UtttExports {
    memory: WebAssembly.Memory;
    // the replay
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
    // the napkin
    uw_paper(side: number): number;
}

/** A NUL-terminated string the kernel handed back, by its address. */
export function cString(memory: WebAssembly.Memory, p: number): string {
    const all = new Uint8Array(memory.buffer);
    let e = p;
    while (all[e]) e++;
    return new TextDecoder().decode(all.subarray(p, e));
}
