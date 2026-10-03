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
    assert.equal(c.plies, arena.UA_DEFAULT_PLIES);
    assert.equal(c.budget, arena.UA_DEFAULT_BUDGET);
    assert.deepEqual(c.weight.slice(0, 4), [9, 81, 729, 6561]);
    assert.ok(c.weight[4] > 4 * 59049 + 5 * 104976, 'the game outweighs the whole board and every threat');
    // a threat is two ninths of the grid it threatens: between that win and
    // the win one size down
    assert.deepEqual(c.threat, [2, 18, 162, 1458, 13122]);
    for (let i = 1; i < 5; i++) assert.ok(c.threat[i] > c.weight[i - 1] && c.threat[i] < c.weight[i], `threat ${i}`);
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

test('the control: setBots goes to the kernel, is clamped there, and the next move searches by it', async () => {
    const a = await arena.instantiateArena(wasm);
    a.start('00000000000000aa', 5);
    a.setBots(2, arena.UA_BUDGET_SMALL);
    let c = a.config();
    assert.equal(c.plies, 2);
    assert.equal(c.budget, 4000);
    assert.ok(c.capNode >= 3 && c.capRoot >= c.capNode, `caps ${c.capNode}/${c.capRoot}`);
    const wideAt2 = c.capNode;
    a.setBots(8, arena.UA_BUDGET_SMALL);
    assert.ok(a.config().capNode < wideAt2, 'eight plies search narrower than two on one budget');
    a.setBots(99, -5);
    c = a.config();
    assert.equal(c.plies, arena.UA_PLIES_MAX);
    assert.equal(c.budget, 0);
    a.setBots(0, 2 ** 31 - 1);
    assert.equal(a.config().plies, arena.UA_PLIES_MIN);
    assert.equal(a.config().budget, arena.UA_BUDGET_HUGE);

    // the next move is searched as set, and the game goes on through changes
    a.setBots(3, arena.UA_BUDGET_LARGE);
    assert.equal(a.step(1), 1);
    assert.equal(a.status().x.searched, 3, 'X searched three plies');
    a.setBots(1, arena.UA_BUDGET_LARGE);
    assert.equal(a.step(1), 1);
    assert.equal(a.status().o.searched, 1, 'O, after the change, one');
    assert.equal(a.status().plies, 2, 'the game was not restarted');
    a.setBots(5, arena.UA_BUDGET_MED);
    assert.equal(a.step(1), 1);
    assert.ok(a.status().x.searched >= 1 && a.status().x.searched <= 5);
    assert.ok(a.status().x.work > 0);
});

test('a game is its seed and its settings: the same changes at the same moves play the same game', async () => {
    const play = async (schedule) => {
        const a = await arena.instantiateArena(wasm);
        a.start('00000000feed0001', 3);
        const moves = [];
        while (!a.status().over) {
            const at = schedule[a.status().plies];
            if (at) a.setBots(at[0], at[1]);
            assert.equal(a.step(1), 1);
            moves.push(a.status().last);
        }
        return moves;
    };
    const s1 = { 0: [1, 4000], 30: [4, 30000], 90: [2, 4000] };
    const one = await play(s1), two = await play(s1);
    assert.deepEqual(two, one);
    const other = await play({ 0: [1, 4000], 30: [3, 30000], 90: [2, 4000] });
    assert.notDeepEqual(other, one, 'another N from move 30 plays another game');
    assert.deepEqual(other.slice(0, 30), one.slice(0, 30), '...but the same first 30 moves');
});

test('the clock: a finished game is not timed again, and the rate is never divided by nothing', () => {
    const c = new arena.GameClock();
    assert.equal(c.rate(), null);
    c.frame(1000, 0, false);
    c.frame(1000.4, 300, false);
    assert.equal(c.rate(), null, 'under 20 ms of play: no rate');
    for (let t = 1016; t <= 2700; t += 16) c.frame(t, Math.round((t - 1000) * 19.12), false);
    c.frame(2716, 32502, true);
    const ms = c.ms, rate = c.rate();
    assert.ok(Math.abs(ms - 1716) < 1e-9, `ms ${ms}`);
    assert.ok(Math.abs(rate - 32502 / 1.716) < 1e-6, `rate ${rate}`);
    // the page shown again: a new loop over the finished game times nothing
    c.pause();
    c.frame(9000, 32502, true);
    c.frame(9000.2, 32502, true);
    assert.equal(c.ms, ms);
    assert.equal(c.rate(), rate);
    // a hidden tab: one long gap counts as MAX_GAP_MS
    const h = new arena.GameClock();
    h.frame(0, 0, false);
    h.frame(60000, 1000, false);
    assert.equal(h.ms, arena.GameClock.MAX_GAP_MS);
    // a pause, then the loop again: the pause is not counted
    h.pause();
    h.frame(70000, 1000, false);
    h.frame(70016, 1100, false);
    assert.equal(h.ms, arena.GameClock.MAX_GAP_MS + 16);
});

test('the game time: tenths while it runs, milliseconds once it is over', () => {
    assert.equal(arena.gameTime(42500, false), '0:42.5');
    assert.equal(arena.gameTime(0, false), '0:00.0');
    assert.equal(arena.gameTime(2143, true), '2.143 s');
    assert.equal(arena.gameTime(67250, true), '1:07.250');
    assert.equal(arena.gameTime(125, true), '0.125 s');
});

test('a start that fails says why: the one sentence without WebAssembly, the error\'s own words otherwise', async () => {
    assert.deepEqual(arena.startError(new Error('x'), false),
        { lead: 'This browser could not start the game (it needs WebAssembly).', detail: null });
    assert.deepEqual(arena.startError(new Error('uttt243.wasm: 404')), { lead: 'This browser could not start the game.', detail: 'uttt243.wasm: 404' });
    assert.equal(arena.startError(new WebAssembly.CompileError('invalid opcode 0xfd')).detail, 'CompileError: invalid opcode 0xfd');
    assert.equal(arena.startError('plain').detail, 'plain');
    // the real refusals, through the real paths
    const a = await arena.instantiateArena(wasm);
    try { a.start('not a seed'); assert.fail('started'); } catch (e) { assert.equal(arena.startError(e).detail, 'not a seed: not a seed'); }
    await assert.rejects(arena.instantiateArena(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1])), (e) => {
        assert.match(arena.startError(e).detail, /^CompileError: /);
        return true;
    });
});
