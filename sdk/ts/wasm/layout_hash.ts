// The layout handshake between a kernel module and the generated TS that reads it.
//
// Every wasm module built by c/Makefile exports wasm_layout_hash(): the
// tools/structgen hash of the Game layout under that build's flags. The module
// generated from the same headers and flags (sdk/ts/gen/layout_hash.<build>.ts)
// carries the same number as LAYOUT_HASH. It is its own module, never the
// accessors in game_layout.<build>.ts: the browser runs this check too, and the
// client bundle must not reach a reader of the unmasked Game
// (e2e/security_client_boundary.test.ts). A host checks the two once per
// instance, before the first kernel call, so a module built from different C
// than the generated accessors fails at load with a message, instead of reading
// every field from the wrong offset. One export call per instance; it costs
// nothing a request would notice.

let expectedOverride: number | null = null;

/** Test-only: pretend the generated module says `hash` (null restores it). */
export function __overrideLayoutHash(hash: number | null): void { expectedOverride = hash; }

const hex = (h: number) => `0x${(h >>> 0).toString(16).padStart(8, '0')}`;

/** Throws unless the instance was built for the layout `generatedPath` describes. */
export function assertLayoutHash(
    module: string,
    ex: { wasm_layout_hash?: () => number },
    generated: number,
    generatedPath: string,
): void {
    const want = (expectedOverride ?? generated) >>> 0;
    if (typeof ex.wasm_layout_hash !== 'function') {
        throw new Error(`${module} exports no wasm_layout_hash: it was built before the layout handshake. ` +
            `Rebuild it (make -C c wasm wasm-bots with WASM_CC=/opt/homebrew/opt/llvm/bin/clang on a Mac).`);
    }
    const got = ex.wasm_layout_hash() >>> 0;
    if (got !== want) {
        throw new Error(`${module} was built for Game layout ${hex(got)}, but ${generatedPath} ` +
            `describes ${hex(want)}: the module and the generated accessors come from different C headers. ` +
            `Rebuild the module (make -C c wasm wasm-bots, which also rewrites sdk/ts/gen/{game_layout,layout_hash}.*.ts) ` +
            `and commit both, or run tools/structgen/gen.sh if only the generated module is stale.`);
    }
}
