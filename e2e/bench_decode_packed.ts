// Microbench: the web's read of one packed game envelope.
//
//   adopt + snapshot   the kernel's client slot reads the envelope
//                      (c/src/client_table.c) and the generated reader copies the
//                      TableView out (sdk/ts/gen/view_layout.bots.ts) - what the
//                      web runs since Phase 6a
//
// It is the "marshal / decode" gate in docs/C_GAME_SHAPE_MIGRATION.md 4.0: the C
// read plus snapshot must be no slower than the retired TS reader, decodePackedGame
// (measured side by side before it was deleted; the numbers are in that table).
// The decodeEnvelope column (the Phase 5a PersonalGame mapping) is gone with the
// TS game shape in Phase 8.
//
// The envelopes are real ones, written by the C Table: a table of one human and
// handwritten bots is dealt from a pinned seed and played (bots by their cycle,
// the human by its first legal move) until a battle is on the table after a few
// actions. Cases: 2p, 4p and 8p seat-0 views, and the 4p spectator view.
//
//   TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/bench_decode_packed.ts
//   BENCH_ITERS=20000 BENCH_RUNS=9 BENCH_JSON=1 ...

import { clientTable } from '@sdk/ts/table/client_table.ts';
import * as L from '@sdk/ts/gen/game_layout.bots.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { residentBoard, residentMoves } from './helpers/table_mem.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }
const out = (s: string) => process.stdout.write(`${s}\n`);

const ITERS = Number(process.env.BENCH_ITERS || 20000);
const RUNS = Number(process.env.BENCH_RUNS || 9);

const table = fixtureTable();

// A mid-bout board of `np` seats: seat 0 human, the rest handwritten bots.
function midBout(np: number, minActions: number): { seat: Uint8Array; spectator: Uint8Array } {
    const human = '00000000-0000-4000-8000-000000000000';
    const seed = Uint8Array.from({ length: 32 }, (_, i) => (i * 7 + np) & 0xff);
    table.create(human, 'Player 1');
    let p = table.commit(`bench${np}`, 1, 0);
    if (typeof p === 'number') throw new Error(`create: ${p}`);
    for (let b = 1; b < np; b++) {
        table.load(p.state, p.roster);
        table.addBot(human, `b0000000-0000-4000-8000-00000000000${b}`, `Player ${b + 1}`, 'handwritten', seed);
        p = table.commit(`bench${np}`, 1, 0) as Exclude<typeof p, number>;
    }
    table.load(p.state, p.roster);
    table.ready(human, seed);
    p = table.commit(`bench${np}`, 42, 0) as Exclude<typeof p, number>;
    for (let actions = 0; actions < 5000; actions++) {
        table.load(p.state, p.roster);
        const board = residentBoard();
        if (board.status !== L.GAME_STATUS_PLAYING) throw new Error(`${np}p: the game ended before a mid-bout board`);
        if (actions >= minActions && board.battles.length > 0) break;
        // A bot seat still in may have no move yet: the drive applies nothing, and the human moves.
        let drove = false;
        if (table.needsBots()) {
            table.setDealSeed('00'.repeat(32));
            const d = table.botDrive(null);
            if (typeof d === 'number') throw new Error(`a bot cycle was refused (${d})`);
            drove = d.n > 0;
        }
        if (!drove) {
            table.load(p.state, p.roster);
            const all = residentMoves(0);
            const m = all.find((x) => x.kind !== 'pickup') ?? all[0];
            if (!m) throw new Error(`${np}p: neither a bot nor the human has a move`);
            table.act(human, m.wire, null, 0);
        }
        p = table.commit(`bench${np}`, 42, 0) as Exclude<typeof p, number>;
    }
    return { seat: p.views[0]!, spectator: p.spectator };
}

const median = (a: number[]) => { const s = [...a].sort((x, y) => x - y); const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };

function time(read: () => number): { median: number; min: number; max: number } {
    let sink = 0;
    const once = () => { for (let i = 0; i < ITERS; i++) sink += read(); };
    for (let w = 0; w < 3; w++) once();
    const perOp: number[] = [];
    for (let r = 0; r < RUNS; r++) {
        const t0 = process.hrtime.bigint();
        once();
        perOp.push(Number(process.hrtime.bigint() - t0) / ITERS);
    }
    if (sink === 0) throw new Error('unreachable');
    return { median: median(perOp), min: Math.min(...perOp), max: Math.max(...perOp) };
}

function main(): void {
    const b2 = midBout(2, 6), b4 = midBout(4, 10), b8 = midBout(8, 14);
    const cases = [
        { name: '2p player', buf: b2.seat }, { name: '4p player', buf: b4.seat },
        { name: '8p player', buf: b8.seat }, { name: '4p spectator', buf: b4.spectator },
    ];
    const client = clientTable();
    const readers: Record<string, (buf: Uint8Array) => number> = {
        'adopt + snapshot': (buf) => client.adoptEnvelope(buf)!.seats.length,
    };
    const results = cases.map(({ name, buf }) => ({
        name, bytes: buf.length,
        ...Object.fromEntries(Object.entries(readers).map(([reader, read]) => [reader, time(() => read(buf))])),
    }));
    if (process.env.BENCH_JSON) {
        out(JSON.stringify({ iters: ITERS, runs: RUNS, results }));
        return;
    }
    out(`envelope read bench: ${ITERS} reads x ${RUNS} runs per case (ns/op, median [min..max])`);
    for (const r of results) {
        const cols = Object.keys(readers).map((k) => {
            const t = (r as unknown as Record<string, { median: number; min: number; max: number }>)[k];
            return `${k} ${t.median.toFixed(0)} [${t.min.toFixed(0)}..${t.max.toFixed(0)}]`;
        });
        out(`  ${r.name.padEnd(13)} ${String(r.bytes).padStart(4)} B  ${cols.join('   ')}`);
    }
}

main();
