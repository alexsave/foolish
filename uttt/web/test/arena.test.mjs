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

test('the control: settings go to the kernel, are clamped there, and the next move searches by it', async () => {
    const a = await arena.instantiateArena(wasm);
    a.start('00000000000000aa', 5);
    a.settings(2, arena.UA_BUDGET_SMALL);
    let c = a.config();
    assert.equal(c.plies, 2);
    assert.equal(c.budget, 4000);
    assert.ok(c.capNode >= 3 && c.capRoot >= c.capNode, `caps ${c.capNode}/${c.capRoot}`);
    const wideAt2 = c.capNode;
    a.settings(8, arena.UA_BUDGET_SMALL);
    assert.ok(a.config().capNode < wideAt2, 'eight plies search narrower than two on one budget');
    assert.equal(arena.UA_PLIES_MAX, 32, 'the stepper goes to 32');
    a.settings(99, -5);
    c = a.config();
    assert.equal(c.plies, arena.UA_PLIES_MAX);
    assert.equal(c.budget, 0);
    a.settings(0, 2 ** 31 - 1);
    assert.equal(a.config().plies, arena.UA_PLIES_MIN);
    assert.equal(a.config().budget, arena.UA_BUDGET_HUGE);

    // the next move is searched as set
    a.settings(3, arena.UA_BUDGET_LARGE);
    assert.equal(a.step(1), 1);
    assert.equal(a.status().x.searched, 3, 'X searched three plies');
    assert.equal(a.step(1), 1);
    assert.equal(a.status().o.searched, 3, 'and O');
    // the deepest setting is taken, and a move comes back on the smallest
    // budget: the iterations it cannot finish are dropped
    a.settings(arena.UA_PLIES_MAX, arena.UA_BUDGET_SMALL);
    assert.equal(a.config().plies, arena.UA_PLIES_MAX);
    assert.equal(a.step(1), 1);
    const x = a.status().x;
    assert.ok(x.searched >= 1 && x.searched < arena.UA_PLIES_MAX, `searched ${x.searched}`);
    assert.ok(x.work > 0);
});

/** A whole depth-3 game from the kernel's own loop: the moves, and how it ended. */
const playOut = (a) => {
    const moves = [];
    while (!a.status().over) { assert.equal(a.step(1), 1); moves.push(a.status().last); }
    return { moves, over: a.status().over };
};

test('a change of settings starts the game again: move 0, the same seed, the new settings', async () => {
    const seed = '00000000feed0001';
    // the reference: a fresh game set to 2 plies on Small before its first move
    const ref = await arena.instantiateArena(wasm);
    ref.start(seed, 3);
    ref.settings(2, arena.UA_BUDGET_SMALL);
    const two = playOut(ref);

    // a game 40 moves in at the defaults, then the control
    const a = await arena.instantiateArena(wasm);
    a.start(seed, 3);
    assert.equal(a.step(40), 40);
    a.settings(2, arena.UA_BUDGET_SMALL);
    const s = a.status();
    assert.equal(s.plies, 0, 'back to move 0');
    assert.equal(s.last, -1, 'no last move');
    assert.equal(s.turn, arena.UA_X, 'X to play');
    assert.equal(s.over, 0);
    assert.equal(a.grid().reduce((n, c) => n + (c ? 1 : 0), 0), 0, 'the picture is empty');
    assert.equal(a.nodes().reduce((n, c) => n + (c ? 1 : 0), 0), 0, 'every node open');
    assert.deepEqual([a.config().plies, a.config().budget], [2, arena.UA_BUDGET_SMALL]);
    assert.deepEqual(playOut(a), two, 'the same seed and settings play the same game, whenever they are set');

    // rapid changes land on the last one: the game is the seed and THOSE
    // settings, with nothing of the ones passed through
    a.settings(5, arena.UA_BUDGET_LARGE);
    assert.equal(a.step(7), 7);
    a.settings(4, arena.UA_BUDGET_MED);
    a.settings(3, arena.UA_BUDGET_MED);
    a.settings(2, arena.UA_BUDGET_SMALL);
    assert.deepEqual(playOut(a), two, 'three changes in a row, then the reference settings: the reference game');

    // and other settings on the same seed play another game
    a.settings(1, arena.UA_BUDGET_SMALL);
    assert.notDeepEqual(playOut(a).moves, two.moves, 'one ply plays another game');
});

test('a change of send rule starts the game again, and seed, settings and rule are the game', async () => {
    const seed = '00000000feed0002';
    const games = {};
    for (const rule of [arena.UA_RULE_SHIFT, arena.UA_RULE_CLIMB, arena.UA_RULE_CLIMB_FREE]) {
        const ref = await arena.instantiateArena(wasm);
        ref.start(seed, 3);
        ref.settings(2, arena.UA_BUDGET_SMALL, rule);
        games[rule] = playOut(ref);
    }
    assert.notDeepEqual(games[arena.UA_RULE_CLIMB].moves, games[arena.UA_RULE_SHIFT].moves, "B' plays another game than A");
    assert.notDeepEqual(games[arena.UA_RULE_CLIMB_FREE].moves, games[arena.UA_RULE_CLIMB].moves, "B plays another game than B'");

    // a game 30 moves in under A, then only the rule changes
    const a = await arena.instantiateArena(wasm);
    a.start(seed, 3);
    a.settings(2, arena.UA_BUDGET_SMALL);
    assert.equal(a.step(30), 30);
    a.settings(2, arena.UA_BUDGET_SMALL, arena.UA_RULE_CLIMB);
    const s = a.status();
    assert.deepEqual([s.plies, s.last, s.turn, s.over], [0, -1, arena.UA_X, 0], 'back to move 0, X to play');
    assert.equal(a.grid().reduce((n, c) => n + (c ? 1 : 0), 0), 0, 'the picture is empty');
    assert.deepEqual(playOut(a), games[arena.UA_RULE_CLIMB], "the same seed, settings and rule: the reference B' game");
    // and back: the A game again, on the same seed
    a.settings(2, arena.UA_BUDGET_SMALL, arena.UA_RULE_SHIFT);
    assert.deepEqual(playOut(a), games[arena.UA_RULE_SHIFT], 'back to A: the reference A game');
    a.settings(2, arena.UA_BUDGET_SMALL, arena.UA_RULE_CLIMB_FREE);
    assert.deepEqual(playOut(a), games[arena.UA_RULE_CLIMB_FREE], 'B: the reference B game');
});

test('the send rule: it reaches the kernel, a game opens at A, and each rule sends where it says', async () => {
    const a = await arena.instantiateArena(wasm);
    a.start('0000000000005e4d', 5);
    assert.equal(a.config().rule, arena.UA_RULE_SHIFT, 'a page opens at rule A');
    assert.equal(arena.UA_DEFAULT_RULE, arena.UA_RULE_SHIFT);
    assert.deepEqual([arena.UA_RULE_SHIFT, arena.UA_RULE_CLIMB, arena.UA_RULE_CLIMB_FREE], [0, 1, 2]);
    a.settings(2, arena.UA_BUDGET_SMALL, arena.UA_RULE_CLIMB);
    assert.equal(a.config().rule, arena.UA_RULE_CLIMB);
    a.settings(2, arena.UA_BUDGET_SMALL, 7);
    assert.equal(a.config().rule, arena.UA_RULE_SHIFT, 'a rule that is not one is A');
    a.settings(2, arena.UA_BUDGET_SMALL, arena.UA_RULE_CLIMB);
    a.settings(3, arena.UA_BUDGET_SMALL);
    assert.equal(a.config().rule, arena.UA_RULE_CLIMB, 'settings without a rule keep the one it plays');

    // where play goes: the share of plies whose forced 3 x 3 lies in the 9 x 9
    // the last move was in, and the widest region seen, over 300 plies
    const watch = (rule) => {
        a.settings(2, arena.UA_BUDGET_SMALL, rule);
        let local = 0, threes = 0, nine = 0;
        for (let i = 0; i < 300; i++) {
            assert.equal(a.step(1), 1);
            const s = a.status(), l = s.lastBox, r = s.regionBox;
            if (r.size === 3) {
                threes++;
                if (Math.floor(r.x / 9) === Math.floor(l.x / 9) && Math.floor(r.y / 9) === Math.floor(l.y / 9)) local++;
            }
            if (r.size === 9) nine++;
        }
        return { local: local / threes, nine };
    };
    const A = watch(arena.UA_RULE_SHIFT), B1 = watch(arena.UA_RULE_CLIMB), B = watch(arena.UA_RULE_CLIMB_FREE);
    assert.ok(A.local < 0.1, `rule A scatters: ${A.local} of its 3 x 3s in the last move's 9 x 9`);
    assert.ok(B1.local > 0.8, `rule B' stays local: ${B1.local}`);
    assert.ok(B.local > 0.8, `rule B stays local: ${B.local}`);
    assert.ok(B.nine > 0, 'rule B opens a whole 9 x 9 after a 3 x 3 is completed');
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
