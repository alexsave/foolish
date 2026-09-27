// THE BROWSER'S BUNDLE, built once and asked two different questions.
//
// esbuild bundles every entry the browser really gets - the Next app router's
// page/layout files plus the Web Worker entries the app spawns with
// `new Worker(new URL(...))` - with tree shaking on and names kept. Two gates
// read the result, and they share this file rather than each configuring its
// own bundle, because two descriptions of "what the browser downloads" would
// drift and the whole point of both gates is that they do not:
//
//   * e2e/security_client_boundary.test.ts asks what must NOT be in it - the
//     readers of an unmasked kernel game.
//   * e2e/wasm_web_link.test.ts asks what IS in it - the set of kernel entry
//     points the browser's own wasm link has to export, and no more.
//
// The config below (CLIENT_BOUNDARY) is the security gate's contract and stays
// with it; browserWasmReach() is the second question.
//
// Pure - no Postgres, no network, no compiler.
import { existsSync, globSync, readFileSync } from 'node:fs';
import * as esbuild from 'esbuild';

export const CLIENT_BOUNDARY = {
    /** The browser's entry points: app router files, plus worker entries found below. */
    entries: ['src/app/**/{page,layout,template,not-found,error,loading,providers}.tsx'],
    /** Modules that must not be reachable from any entry. */
    deniedModules: [
        // tools/structgen output: raw accessors over the kernel's Game struct.
        /(^|\/)game_layout\.[^/]*$/,
        // The server's C Table wrapper: loads a durable blob and hands out a
        // commit's products, the unmasked state blob among them.
        /(^|\/)sdk\/ts\/table\/server_table\.ts$/,
        // C-backed test fixtures: compose and seal unmasked boards, write rows.
        /(^|\/)e2e\/helpers\/table_(fixture|db)\.ts$/,
    ],
    /** Functions that read or write an unmasked kernel game, by name. */
    deniedSymbols: [
        // the durable (unmasked) state blob codec (the TS one, sdk/ts/wasm/engine.ts, is deleted)
        'serializeGameState', 'deserializeGameState', 'loadStateBlob',
        // unmasked kernel state -> JS Game
        'parseState', 'stateToGame', 'materializeKernelGame',
        // the server's packed pipeline over a durable blob
        'runPackedAction', 'runPackedGameAction', 'exportPackedProducts', 'exportPackedDriveProducts',
        'runPackedStart', 'runPackedRearrange', 'serializeViewBlob', 'serializeViewBlobs',
        // the server bot drive (sdk/ts/wasm/bots.ts), which exports post-state blobs
        'wasmBotDrive',
    ],
    /** Modules the client must reach: it reads the wire through the kernel's client slot, and the Oracle's
     *  Mode B candidate table through its generated reader (Phase 7). */
    requiredModules: [
        /(^|\/)sdk\/ts\/gen\/view_layout\.bots\.ts$/,
        /(^|\/)sdk\/ts\/table\/client_table\.ts$/,
        /(^|\/)sdk\/ts\/gen\/oracle_layout\.oracle_mt\.ts$/,
    ],
    /** The TypeScript wire readers the client slot replaced (Phase 5a), and the client rules and boards it replaced (Phase 6b), by name. */
    retiredReaders: ['decodePackedGame', 'viewToGame', 'decodeEventWire', 'kernelViewFromPacked', 'kernelEventsFromPacked', 'decodePackedRoster',
        'initClientGuards', 'guardsReady', 'resetToLobby', 'applyOverlayEntries', 'isHandPermutation',
        // the Oracle's TS position and memory builders: the kernel writes both (Phase 7)
        'encodeLogsWire', 'moveLogIndices'],
    /** Modules the client slot replaced: the second kernel the web used to load for its move gates (guards.wasm) (Phase 6b). */
    retiredModules: [/(^|\/)sdk\/ts\/wasm\/guards_wasm\.ts$/, /(^|\/)src\/wasm\/clientGuards\.ts$/,
        // the Oracle's TS log wire encoder: the kernel writes the Oracle's memory (Phase 7)
        /(^|\/)src\/oracle\/logsWire\.ts$/,
        // the TS replay decoder over rules.wasm: the web reads a code through bots.wasm's summary and frames (Phase 7)
        /(^|\/)server\/api\/common\/replay\/decode(_kernel)?\.ts$/],
    /** Kernel exports that serialize the resident game unmasked. */
    deniedWasmExports: ['wasm_export_state', 'wasm_state_serialize', 'wasm_state_deserialize',
        // the C Table: loads a durable blob, and writes it back out in a commit
        'wasm_table_load', 'wasm_table_commit_products',
        // the test fixtures: a composed board sealed into an unmasked state blob
        'wasm_fixture_seal'],
};

// Node built-ins the shared sdk modules reach only behind a runtime check.
const EXTERNAL = ['next', 'next/*', 'react', 'react/*', 'react-dom', 'react-dom/*', '@supabase/*', '@vercel/*',
    'qrcode.react', 'fs', 'path', 'zlib', 'url', 'module', 'crypto', 'worker_threads', 'node:*'];

function workerEntries(): string[] {
    const out: string[] = [];
    for (const f of globSync('src/**/*.{ts,tsx}')) {
        const src = readFileSync(f, 'utf8');
        for (const m of src.matchAll(/new\s+Worker\(\s*new\s+URL\(\s*['"]([^'"]+)['"]/g)) {
            out.push(new URL(m[1], `file:///${f}`).pathname.slice(1));
        }
    }
    return out;
}

export interface BoundaryReport {
    modules: string[];
    required: { pattern: string; found: boolean }[];
    retired: { name: string; count: number }[];
    symbols: { name: string; count: number }[];
    wasmExports: { name: string; count: number }[];
    chains: string[];
}

async function bundle(entryPoints: string[] | null, stdin?: string): Promise<esbuild.BuildResult & { metafile: esbuild.Metafile; outputFiles: esbuild.OutputFile[] }> {
    return esbuild.build({
        ...(stdin ? { stdin: { contents: stdin, resolveDir: process.cwd(), loader: 'ts', sourcefile: 'canary.ts' } } : { entryPoints: entryPoints! }),
        bundle: true, write: false, metafile: true, outdir: 'e2e-client-boundary-out',
        format: 'esm', platform: 'browser', splitting: !stdin, treeShaking: true, minify: false,
        legalComments: 'none', jsx: 'automatic', tsconfig: 'tsconfig.json', logLevel: 'silent',
        loader: { '.wasm': 'file', '.gz': 'file', '.css': 'empty', '.svg': 'file', '.png': 'file', '.jpg': 'file' },
        external: EXTERNAL,
    }) as never;
}

// The shortest import path from any entry to `target`, marking dynamic imports.
function chainTo(meta: esbuild.Metafile, entries: string[], target: string): string {
    const prev = new Map<string, { from: string; dynamic: boolean } | null>();
    const queue: string[] = [];
    for (const e of entries) if (meta.inputs[e]) { prev.set(e, null); queue.push(e); }
    while (queue.length) {
        const cur = queue.shift()!;
        if (cur === target) break;
        for (const imp of meta.inputs[cur]?.imports ?? []) {
            if (imp.external || prev.has(imp.path)) continue;
            prev.set(imp.path, { from: cur, dynamic: imp.kind === 'dynamic-import' });
            queue.push(imp.path);
        }
    }
    if (!prev.has(target)) return `${target} (no chain found)`;
    const hops: string[] = [target];
    for (let at = prev.get(target); at; at = prev.get(at.from)) hops.unshift(`${at.from} ${at.dynamic ? '=(dynamic import)=>' : '->'}`);
    return hops.join(' ');
}

export async function scanClientBoundary(opts: { stdin?: string } = {}): Promise<BoundaryReport> {
    const entries = opts.stdin ? ['canary.ts'] : [...CLIENT_BOUNDARY.entries.flatMap(g => globSync(g)), ...workerEntries()];
    // A glob that stopped matching would make every rule below pass over an
    // empty graph, which is the failure mode this repo has shipped three times.
    if (!opts.stdin && entries.length <= 5) throw new Error(`the client entries did not glob (${entries.length})`);
    const r = await bundle(opts.stdin ? null : entries, opts.stdin);
    const inputs = Object.keys(r.metafile.inputs);
    const code = r.outputFiles.filter(f => f.path.endsWith('.js')).map(f => f.text).join('\n');
    const count = (re: RegExp) => (code.match(re) ?? []).length;
    const modules = inputs.filter(p => CLIENT_BOUNDARY.deniedModules.some(re => re.test(p)));
    const required = CLIENT_BOUNDARY.requiredModules.map(re => ({ pattern: String(re), found: inputs.some(p => re.test(p)) }));
    // esbuild suffixes a renamed binding with digits (name2) on a collision.
    const symbols = CLIENT_BOUNDARY.deniedSymbols.map(name => ({ name, count: count(new RegExp(`\\b${name}\\d*\\b`, 'g')) })).filter(s => s.count > 0);
    const wasmExports = CLIENT_BOUNDARY.deniedWasmExports.map(name => ({ name, count: count(new RegExp(`\\b${name}\\b`, 'g')) })).filter(s => s.count > 0);
    const retired = [
        ...CLIENT_BOUNDARY.retiredReaders.map(name => ({ name, count: count(new RegExp(`\\b${name}\\d*\\b`, 'g')) })).filter(s => s.count > 0),
        ...inputs.filter(p => CLIENT_BOUNDARY.retiredModules.some(re => re.test(p))).map(name => ({ name, count: 1 })),
    ];
    // Chains: to every denied module, and to every module defining a surviving denied symbol.
    const definers = new Set<string>(modules);
    for (const s of [...symbols, ...wasmExports]) {
        for (const p of inputs) {
            if (p.includes('node_modules') || !existsSync(p)) continue;
            if (new RegExp(`(function|const|let)\\s+${s.name}\\b|\\b${s.name}\\s*\\(`).test(readFileSync(p, 'utf8'))) definers.add(p);
        }
    }
    const chains = [...definers].map(d => chainTo(r.metafile, opts.stdin ? inputs.filter(i => i.includes('canary')) : entries, d));
    // A dynamic import keeps every export of its target alive, whatever the
    // importer uses - the usual reason an unused reader survives tree shaking.
    for (const [from, info] of Object.entries(r.metafile.inputs)) {
        for (const imp of info.imports) {
            if (imp.kind === 'dynamic-import' && !imp.external) chains.push(`dynamic import: ${from} =(keeps every export of)=> ${imp.path}`);
        }
    }
    return { modules, required, retired, symbols, wasmExports, chains };
}

/**
 * Every `wasm_*` name that survives tree shaking into the browser bundle.
 *
 * This is the measurement c/Makefile's WASM_WEB_NAMES is written from: the
 * browser's link of the kernel must export exactly these (minus the names that
 * belong to the Oracle's own modules, which this cannot tell apart and does not
 * try to - the caller intersects with a module's export table).
 *
 * It counts a name as reached when it appears in the bundled output, which is
 * deliberately generous in one direction: a wrapper esbuild could not prove dead
 * keeps its export alive here, and shipping an entry point the browser turns out
 * not to call costs bytes, while missing one costs a TypeError in a tab.
 */
export async function browserWasmReach(): Promise<Set<string>> {
    const entries = [...CLIENT_BOUNDARY.entries.flatMap((g) => globSync(g)), ...workerEntries()];
    if (entries.length <= 5) throw new Error(`the client entries did not glob (${entries.length})`);
    const r = await bundle(entries);
    const code = r.outputFiles.filter((f) => f.path.endsWith('.js')).map((f) => f.text).join('\n');
    const out = new Set<string>();
    for (const m of code.matchAll(/\bwasm_[A-Za-z0-9_]+/g)) out.add(m[0]);
    return out;
}
