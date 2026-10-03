// The 243 page's module, loaded as the page loads it, against the kernel it
// drives (public/uttt243.wasm and lib/gen/, both written by `npm run wasm`).
// lib/arena.ts and the generated readers it imports are TypeScript and node 20
// runs JavaScript, so each is transpiled here with the site's own compiler -
// the modules under test are the page's files, not copies.
//
//   npm test
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, writeFileSync, mkdtempSync, mkdirSync } from 'node:fs';
import { join, dirname, relative } from 'node:path';
import { tmpdir } from 'node:os';
import { pathToFileURL } from 'node:url';
import ts from 'typescript';

const web = join(import.meta.dirname, '..');
const out = mkdtempSync(join(tmpdir(), 'arena-'));

/** Transpile `file` and every relative module it imports, as .mjs under out/. */
function load(file, seen = new Set()) {
    if (seen.has(file)) return;
    seen.add(file);
    let js = ts.transpileModule(readFileSync(file, 'utf8'), {
        compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 },
    }).outputText;
    js = js.replace(/(from\s+['"])(\.{1,2}\/[^'"]+)(['"])/g, (_, a, spec, b) => {
        load(join(dirname(file), spec + '.ts'), seen);
        return a + spec + '.mjs' + b;
    });
    const to = join(out, relative(web, file)).replace(/\.ts$/, '.mjs');
    mkdirSync(dirname(to), { recursive: true });
    writeFileSync(to, js);
}
load(join(web, 'lib', 'arena.ts'));
const arena = await import(pathToFileURL(join(out, 'lib', 'arena.mjs')).href);
const wasm = readFileSync(join(web, 'public', 'uttt243.wasm'));

test('the module loads, its layout matches the kernel, and it states the bots', async () => {
    const a = await arena.instantiateArena(wasm);
    assert.equal(typeof arena.loadArena, 'function');
    a.start('0000000000000001');
    const c = a.config();
    assert.equal(c.depth, 5);
    assert.equal(c.side, 243);
    assert.equal(c.leaves, 59049);
    assert.equal(c.nodes, 7381);
    assert.ok(c.plies >= 2 && c.plies <= 6, `plies ${c.plies}`);
    assert.ok(c.budget > 0);
    assert.deepEqual(c.weight.slice(0, 4), [1, 9, 81, 729]);
    assert.ok(c.weight[4] > 26244, 'the game outweighs the whole board');
    assert.deepEqual([arena.UA_OPEN, arena.UA_X, arena.UA_O, arena.UA_DRAW], [0, 1, 2, 3]);
});

test('a module built for another layout is refused', async () => {
    const mod = await WebAssembly.compile(wasm);
    const inst = await WebAssembly.instantiate(mod, {});
    assert.ok(inst.exports.ua_layout_hash() !== 0, 'the module is stamped');
    // the same module, its ua_layout_hash answering another layout's hash
    const real = WebAssembly.instantiate;
    WebAssembly.instantiate = async (m, i) => {
        const r = await real(m, i);
        return { exports: { ...r.exports, ua_layout_hash: () => r.exports.ua_layout_hash() ^ 1 } };
    };
    try {
        await assert.rejects(arena.instantiateArena(mod), /different headers/);
    } finally {
        WebAssembly.instantiate = real;
    }
});

test('seeds: 16 hex digits, the secure random spelled the same way', () => {
    assert.equal(arena.parseSeed('00ff'), '00000000000000ff');
    assert.equal(arena.parseSeed('  DEADbeef01234567 '), 'deadbeef01234567');
    assert.equal(arena.parseSeed('xyz'), null);
    assert.equal(arena.parseSeed('12345678901234567'), null);
    const s = arena.randomSeed();
    assert.match(s, /^[0-9a-f]{16}$/);
    assert.notEqual(s, arena.randomSeed());
});

test('the bot plays a legal move, and the board and the status show it', async () => {
    const a = await arena.instantiateArena(wasm);
    a.start('0123456789abcdef', 5);
    for (let i = 0; i < 200; i++) {
        const mv = a.w.ua_think();
        assert.ok(mv >= 0 && mv < 59049, `move ${mv}`);
        assert.equal(a.w.ua_legal_at(mv), 1, `ply ${i}: ${mv} is not legal`);
        const mover = a.status().turn;
        assert.equal(a.w.ua_play(mv), 1);
        const cells = new Uint8Array(a.w.memory.buffer, a.w.ua_cells(), 59049);
        assert.equal(cells[mv], mover);
        const s = a.status();
        assert.equal(s.last, mv);
        assert.equal(s.plies, i + 1);
        assert.notEqual(s.turn, mover);
        assert.equal(s.lastBox.size, 1);
        assert.equal(a.grid()[s.lastBox.y * 243 + s.lastBox.x], mover, `ply ${i}: the picture has the mark where the box says`);
        assert.ok(s.regionBox.size >= 3, 'the region is at least a 3 x 3');
    }
    assert.equal(a.grid().reduce((n, c) => n + (c ? 1 : 0), 0), 200);
    assert.equal(a.w.ua_play(a.status().last), 0, 'a filled cell is refused');
    assert.deepEqual(a.box(0), { x: 0, y: 0, size: 243 }, 'the root is the whole board');
    assert.equal(a.level(0), 0);
});

test('one seed, one game; a whole small game ends', async () => {
    const play = async (seed) => {
        const a = await arena.instantiateArena(wasm);
        a.start(seed, 3);
        const moves = [];
        while (!a.status().over) { assert.equal(a.step(1), 1); moves.push(a.status().last); }
        return { moves, over: a.status().over };
    };
    const one = await play('00000000c0ffee00');
    const two = await play('00000000c0ffee00');
    const other = await play('00000000c0ffee01');
    assert.deepEqual(two, one);
    assert.notDeepEqual(other.moves, one.moves);
    assert.ok([arena.UA_X, arena.UA_O, arena.UA_DRAW].includes(one.over));
});
