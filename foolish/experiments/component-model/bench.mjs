// node bench.mjs [N] [runs]
// Round trip = import-state(game) + export-state() -> game, both variants
// starting from and ending at the same JS object shape.
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const N = +(process.argv[2] ?? 200000);
const RUNS = +(process.argv[3] ?? 7);
const here = new URL('./out/', import.meta.url);

// ---- a realistic 4-player mid-game (36-card deck, 8 left, 3 battles on table)
const mkState = (typed) => {
    const L = typed ? (a) => Uint8Array.from(a) : (a) => a;
    return {
        status: 'playing', numPlayers: 4, trumpSuit: 2, firstAttacker: 1, defender: 2,
        discard: 12, flipped: 33, goodMask: 0, hasGoodTs: false,
        deck: L([4, 17, 29, 8, 44, 12, 38, 25]),
        battles: [{ attack: 5, defense: 9 }, { attack: 18, defense: 22 }, { attack: 31, defense: undefined }],
        players: [
            { status: 'playing', awaiting: false, hand: L([0, 13, 27, 40, 7, 20]) },
            { status: 'playing', awaiting: false, hand: L([1, 14, 41, 34, 11]) },
            { status: 'playing', awaiting: true, hand: L([2, 15, 28, 42, 36, 49, 10]) },
            { status: 'playing', awaiting: false, hand: L([3, 16, 43, 23]) },
        ],
        elimination: L([]),
    };
};

// ---- baseline: hand-written marshal in the style of engine.ts marshalGame/parseState
const G_STATUS_TO_INT = { waiting: 0, playing: 1, finished: 2 };
const G_STATUS = ['waiting', 'playing', 'finished'];
const P_STATUS_TO_INT = { idle: 0, ready: 1, playing: 2, out: 3 };
const P_STATUS = ['idle', 'ready', 'playing', 'out'];
const WIRE_NONE = 0xff;
const i8 = (b) => (b << 24) >> 24;

const base = new WebAssembly.Instance(new WebAssembly.Module(readFileSync(new URL('baseline.wasm', here))), {}).exports;
let memBuf = null, memU8 = null;
const mem = () => (memBuf === base.memory.buffer ? memU8 : (memU8 = new Uint8Array(memBuf = base.memory.buffer)));

function marshal(game) {
    const buf = mem();
    let q = base.wasm_io_ptr();
    buf[q++] = G_STATUS_TO_INT[game.status] ?? 0;
    buf[q++] = game.players.length;
    buf[q++] = game.trumpSuit & 0xff;
    buf[q++] = game.firstAttacker & 0xff;
    buf[q++] = game.defender & 0xff;
    buf[q++] = game.discard & 0xff;
    buf[q++] = (game.discard >> 8) & 0xff;
    const hasF = game.flipped !== undefined && game.flipped !== null;
    buf[q++] = hasF ? 1 : 0;
    buf[q++] = hasF ? game.flipped : 0;
    const mask = game.goodMask;
    buf[q++] = mask & 0xff;
    buf[q++] = (mask >> 8) & 0xff;
    buf[q++] = (mask >> 16) & 0xff;
    buf[q++] = (mask >> 24) & 0xff;
    buf[q++] = game.hasGoodTs ? 1 : 0;
    buf[q++] = game.deck.length & 0xff;
    buf[q++] = (game.deck.length >> 8) & 0xff;
    for (const c of game.deck) buf[q++] = c;
    buf[q++] = game.battles.length;
    for (const b of game.battles) {
        buf[q++] = b.attack;
        buf[q++] = b.defense !== undefined && b.defense !== null ? b.defense : WIRE_NONE;
    }
    for (const p of game.players) {
        buf[q++] = P_STATUS_TO_INT[p.status] ?? 0;
        buf[q++] = p.awaiting ? 1 : 0;
        buf[q++] = p.hand.length;
        for (const c of p.hand) buf[q++] = c;
    }
    buf[q++] = game.elimination.length;
    for (const s of game.elimination) buf[q++] = s & 0xff;
    base.wasm_import_state();
}

function unmarshal() {
    base.wasm_export_state();
    const buf = mem();
    let q = base.wasm_io_ptr();
    const status = G_STATUS[buf[q++]];
    const numPlayers = buf[q++];
    const trumpSuit = buf[q++];
    const firstAttacker = i8(buf[q++]);
    const defender = i8(buf[q++]);
    const discard = buf[q] | (buf[q + 1] << 8); q += 2;
    const hasFlipped = buf[q++] !== 0;
    const flippedWire = buf[q++];
    const goodMask = (buf[q] | (buf[q + 1] << 8) | (buf[q + 2] << 16) | (buf[q + 3] << 24)) >>> 0; q += 4;
    const hasGoodTs = buf[q++] !== 0;
    const deckN = buf[q] | (buf[q + 1] << 8); q += 2;
    const deck = new Array(deckN);
    for (let i = 0; i < deckN; i++) deck[i] = buf[q++];
    const nBattles = buf[q++];
    const battles = [];
    for (let i = 0; i < nBattles; i++) {
        const attack = buf[q++];
        const dw = buf[q++];
        battles.push({ attack, defense: dw === WIRE_NONE ? undefined : dw });
    }
    const players = [];
    for (let i = 0; i < numPlayers; i++) {
        const st = P_STATUS[buf[q++]];
        const awaiting = buf[q++] !== 0;
        const handN = buf[q++];
        const hand = new Array(handN);
        for (let j = 0; j < handN; j++) hand[j] = buf[q++];
        players.push({ status: st, awaiting, hand });
    }
    const elimN = buf[q++];
    const elimination = [];
    for (let i = 0; i < elimN; i++) elimination.push(buf[q++]);
    return {
        status, numPlayers, trumpSuit, firstAttacker, defender, discard,
        flipped: hasFlipped ? flippedWire : undefined, goodMask, hasGoodTs,
        deck, battles, players, elimination,
    };
}

// ---- component via jco (sync instantiation: no await anywhere)
// JCO_DIRS=jco-sync,jco-1.10-sync picks which transpiled outputs under out/ to bench.
const jcoDirs = (process.env.JCO_DIRS ?? 'jco-sync').split(',');
const comps = [];
for (const d of jcoDirs) {
    const { instantiate } = await import(`./out/${d}/kernel.js`);
    const c = instantiate((path) => new WebAssembly.Module(readFileSync(new URL(`${d}/${path}`, here))), {});
    comps.push([d, c.state.importState, c.state.exportState]);
}

// ---- Emscripten embind, when build.sh found an emcc to build it with.
// Instantiation is asynchronous and there is no synchronous mode: the factory
// returns a promise. The bound functions take and return plain JS values.
let embind = null;
try {
    const { default: createKernel } = await import('./out/embind/kernel.js');
    const m = await createKernel();
    embind = [m.importState.bind(m), m.exportState.bind(m)];
} catch {
    console.log('embind: out/embind/kernel.js not built (emcc missing?), skipping');
}

// ---- correctness first: both round trips must reproduce the input
const norm = (g) => JSON.parse(JSON.stringify(g, (k, v) => (v instanceof Uint8Array ? Array.from(v) : v)));
const unsupported = new Set();
for (const typed of [false, true]) {
    const s = mkState(typed);
    marshal(s); assert.deepEqual(norm(unmarshal()), norm(s));
    for (const [d, importState, exportState] of comps) {
        try { importState(s); } catch (e) {
            console.log(`${d}: rejects ${typed ? 'Uint8Array' : 'number[]'} lists: ${e.message}`);
            unsupported.add(`${d}/${typed}`);
            continue;
        }
        assert.deepEqual(norm(exportState()), norm(s));
    }
    if (embind) {
        embind[0](s);
        assert.deepEqual(norm(embind[1]()), norm(s));
    }
}
console.log('round trips verified for every supported case');

const cases = [
    ['baseline hand marshal (number[])', mkState(false), (s) => { marshal(s); return unmarshal(); }],
    ['baseline hand marshal (Uint8Array)', mkState(true), (s) => { marshal(s); return unmarshal(); }],
    ...comps.flatMap(([d, importState, exportState]) => [false, true]
        .filter((typed) => !unsupported.has(`${d}/${typed}`))
        .map((typed) => [`${d} (${typed ? 'Uint8Array' : 'number[]'})`, mkState(typed), (s) => { importState(s); return exportState(); }])),
    ...(embind ? [false, true].map((typed) => [`embind (${typed ? 'Uint8Array' : 'number[]'})`, mkState(typed),
        (s) => { embind[0](s); return embind[1](); }]) : []),
];

let sink = 0;
for (const [, s, fn] of cases) for (let i = 0; i < 20000; i++) sink ^= fn(s).numPlayers; // warmup
const results = cases.map(() => []);
for (let r = 0; r < RUNS; r++) {
    for (let c = 0; c < cases.length; c++) { // interleave cases so drift hits all equally
        const [, s, fn] = cases[c];
        const t0 = process.hrtime.bigint();
        for (let i = 0; i < N; i++) sink ^= fn(s).numPlayers;
        results[c].push(Number(process.hrtime.bigint() - t0) / N);
    }
}
const median = (a) => [...a].sort((x, y) => x - y)[a.length >> 1];
console.log(`node ${process.version}, N=${N}, runs=${RUNS}, ns per round trip (import+export)`);
for (let c = 0; c < cases.length; c++) {
    const a = results[c];
    console.log(`${cases[c][0].padEnd(38)} median ${median(a).toFixed(0).padStart(6)}  min ${Math.min(...a).toFixed(0).padStart(6)}  max ${Math.max(...a).toFixed(0).padStart(6)}  [${a.map((x) => x.toFixed(0)).join(' ')}]`);
}
