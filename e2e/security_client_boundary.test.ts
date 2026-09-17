// SECURITY S2: the web client ships no reader of an UNMASKED kernel game.
//
// Hidden information is safe on the wire today because the server only ever
// hands a client a masked board (e2e/security_hidden_info.test.ts). This file
// pins the other half: code that turns an unmasked kernel game into bytes or a
// JS object - the durable-blob codec, the server's packed pipeline, and (after
// the C Game shape migration) the generated struct accessors - is not part of
// the browser bundle at all. A reader the client does not have is one nobody
// can wire to the wrong input.
//
// HOW. esbuild bundles every src/ entry the way a browser gets it (the Next app
// router's page/layout files plus the Web Worker entries the app spawns with
// `new Worker(new URL(...))`), with tree shaking on and names kept. Two rules:
//
//   - MODULE rule: no module matching a denied path is in the graph at all
//     (metafile inputs). This is where the structgen accessors are pinned.
//   - SYMBOL rule: no denied function survives tree shaking into the output,
//     and no denied wasm export is called by it. Module-level reachability is
//     too coarse for engine.ts, which the client legitimately uses for masked
//     marshaling and replay decode - so what is pinned is that the unmasked
//     readers inside it are dead to the browser.
//
// A red names the entry and the import chain that reaches the denied module,
// and whether a dynamic import (which keeps EVERY export of its target) is on it.
//
// The config below is the contract the migration extends: when it adds a module
// or a function that reads a full Game, it adds it here.
//
// Since Phase 5a the client reads the wire through the kernel too, so the
// boundary is pinned from the other side as well: the bundle DOES reach the
// client slot's generated snapshot readers (view_layout) and its wrapper, and
// the TypeScript wire readers they replaced are not in it.

import { test } from 'node:test';
import assert from 'node:assert/strict';
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
    assert.ok(opts.stdin || entries.length > 5, `found the client entries (${entries.length})`);
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

const explain = (r: BoundaryReport) =>
    `\n  denied modules: ${JSON.stringify(r.modules)}\n  denied symbols in the bundle: ${JSON.stringify(r.symbols)}` +
    `\n  denied wasm exports in the bundle: ${JSON.stringify(r.wasmExports)}` +
    `\n  required modules: ${JSON.stringify(r.required)}\n  retired wire readers in the bundle: ${JSON.stringify(r.retired)}` +
    `\n  import chains:\n    ${r.chains.join('\n    ')}`;

test('the web client bundle contains no reader of an unmasked kernel game', async () => {
    const r = await scanClientBoundary();
    assert.deepEqual({ modules: r.modules, symbols: r.symbols, wasmExports: r.wasmExports },
        { modules: [], symbols: [], wasmExports: [] }, `the client can read an unmasked game:${explain(r)}`);
});

test('the web client reads the wire through the kernel: it reaches view_layout and client_table, and no TS wire reader, client rule or second kernel', async () => {
    const r = await scanClientBoundary();
    assert.deepEqual(r.required.filter(q => !q.found), [], `the client does not reach the client slot's readers:${explain(r)}`);
    assert.deepEqual(r.retired, [], `a TypeScript wire reader is still in the client bundle:${explain(r)}`);
});

// The scanner itself must be able to see a violation, or a green above means
// nothing. Each canary is a client module that does exactly one forbidden thing.
test('canary: a client module importing the structgen Game accessors is caught by the module rule', async () => {
    // Wherever structgen writes it (tools/structgen/gen, or sdk/ts/gen once the migration moves it).
    const layout = globSync('{sdk,tools}/**/game_layout.bots.ts')[0];
    assert.ok(layout, 'the generated Game accessors exist');
    const r = await scanClientBoundary({ stdin: `import { Game_get_status } from './${layout}'; console.log(Game_get_status);` });
    assert.ok(r.modules.includes(layout), `module rule saw ${layout}:${explain(r)}`);
});

test('canary: a client module importing the server table wrapper is caught by the module rule', async () => {
    // sdk/ts/table/server_table.ts hands out the unmasked state blob (a commit's
    // products). It must be denied by name, not only because it happens to import
    // the Game accessors today.
    const r = await scanClientBoundary({ stdin: `import { createServerTable } from './sdk/ts/table/server_table.ts'; console.log(createServerTable);` });
    assert.ok(r.modules.includes('sdk/ts/table/server_table.ts'), `module rule saw server_table.ts:${explain(r)}`);
    assert.ok(r.wasmExports.some(s => s.name === 'wasm_table_commit_products'), `and the unmasked commit export:${explain(r)}`);
});

test('canary: a client module importing the C-backed test fixtures is caught by the module rule', async () => {
    const r = await scanClientBoundary({ stdin: `import { fixture } from './e2e/helpers/table_fixture.ts'; console.log(fixture);` });
    assert.ok(r.modules.includes('e2e/helpers/table_fixture.ts'), `module rule saw table_fixture.ts:${explain(r)}`);
    assert.ok(r.wasmExports.some(s => s.name === 'wasm_fixture_seal'), `and the seal export:${explain(r)}`);
});

test('canary: a client module calling the durable blob reader is caught by the symbol rule', async () => {
    // The TS durable blob reader is deleted (Phase 8); the canary brings its own, as
    // a resurrected copy would, calling the kernel's unmasked importer.
    const reader = 'export function deserializeGameState(ex: { wasm_state_deserialize(n: number): number }, n: number) { return ex.wasm_state_deserialize(n); }';
    const r = await scanClientBoundary({ stdin: `${reader} console.log(deserializeGameState);` });
    assert.ok(r.symbols.some(s => s.name === 'deserializeGameState'), `symbol rule saw deserializeGameState:${explain(r)}`);
    assert.ok(r.wasmExports.some(s => s.name === 'wasm_state_deserialize'), `and the wasm export it calls:${explain(r)}`);
});

test('canary: a client module using only masked kernel helpers is NOT flagged', async () => {
    const r = await scanClientBoundary({ stdin: `import { readTableView } from './sdk/ts/gen/view_layout.bots.ts'; console.log(readTableView);` });
    assert.deepEqual({ modules: r.modules, symbols: r.symbols, wasmExports: r.wasmExports },
        { modules: [], symbols: [], wasmExports: [] }, `a masked reader is allowed:${explain(r)}`);
});
