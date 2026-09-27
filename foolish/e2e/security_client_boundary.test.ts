// SECURITY S2: the web client ships no reader of an UNMASKED kernel game.
//
// Hidden information is safe on the wire today because the server only ever
// hands a client a masked board (e2e/helpers/hidden_info.ts, run by the security_hidden_info_{2p,4p,6p} files). This file
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
import { globSync } from 'node:fs';

// The bundle and this gate's contract live in e2e/helpers/client_bundle.ts,
// beside the second gate that reads the same graph (e2e/wasm_web_link.test.ts,
// which pins what the browser's own wasm link must export). One description of
// "what the browser downloads", asked two questions.
import { CLIENT_BOUNDARY, scanClientBoundary, type BoundaryReport } from './helpers/client_bundle.ts';


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
