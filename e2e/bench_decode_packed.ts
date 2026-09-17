// Microbench: the web's read of one packed game envelope.
//
//   decodePackedGame   the retired TS reader (sdk/ts/wire/view.ts over
//                      packed_read.ts): header, roster trailer, masked board, and
//                      the viewToGame materialization into a PersonalGame
//   adopt + snapshot   Phase 5a: the kernel's client slot reads the envelope
//                      (c/src/client_table.c) and the generated reader copies the
//                      TableView out (sdk/ts/gen/view_layout.bots.ts)
//   decodeEnvelope     the same plus the transitional PersonalGame mapping
//                      (src/state/snapshotToGame.ts) - what the web runs today
//
// It is the "marshal / decode" gate in docs/C_GAME_SHAPE_MIGRATION.md 4.0: the C
// read plus snapshot must be no slower than decodePackedGame.
//
// The envelopes are real ones, written by the C Table: a table of one human and
// handwritten bots is dealt from a pinned seed and played (bots by their cycle,
// the human by its first legal move) until a battle is on the table after a few
// actions. Cases: 2p, 4p and 8p seat-0 views, and the 4p spectator view.
//
//   TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/bench_decode_packed.ts
//   BENCH_ITERS=20000 BENCH_RUNS=9 BENCH_JSON=1 ...

import { decodePackedGame } from '@sdk/ts/wire/view.ts';
import { clientTable } from '@sdk/ts/table/client_table.ts';
import { decodeEnvelope } from '../src/state/snapshotToGame.ts';
import { deserializeGameState, kernelLegalMoves } from '@sdk/ts/wasm/engine.ts';
import { wasmBotEligibleMask } from '@sdk/ts/wasm/bots.ts';
import { encodeAction, AWIRE_KIND } from '@sdk/ts/wire/awire.ts';
import { GAME_STATUS } from '@api/core/types.ts';
import { createServerTable } from '@sdk/ts/table/server_table.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }
const out = (s: string) => process.stdout.write(`${s}\n`);

const ITERS = Number(process.env.BENCH_ITERS || 20000);
const RUNS = Number(process.env.BENCH_RUNS || 9);
const NOW = () => 1_760_000_000_000;

const table = createServerTable();
const sx = (table as unknown as { ex: { memory: WebAssembly.Memory; wasm_io_ptr(): number; wasm_table_set_deal_seed(n: number): number; wasm_table_bot_drive(p: number, m: number): number } }).ex;

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
        const seats = table.seats();
        const game = deserializeGameState(p.state, {
            id: `bench${np}`, name: '', deck_length: 0, good_players: seats.map((s) => s.id), good_timestamp: 1,
            players: seats.map((s) => ({ player_id: s.id, name: s.name, is_ai: s.brain !== '', strategy_key: s.brain || 'human' })),
        });
        if (game.status !== GAME_STATUS.PLAYING) throw new Error(`${np}p: the game ended before a mid-bout board`);
        if (actions >= minActions && game.table_battles.length > 0) break;
        table.load(p.state, p.roster);
        if (wasmBotEligibleMask(game) !== 0) {
            const hex = new TextEncoder().encode('00'.repeat(32));
            new Uint8Array(sx.memory.buffer).set(hex, sx.wasm_io_ptr());
            sx.wasm_table_set_deal_seed(hex.length);
            if (sx.wasm_table_bot_drive(0, 0) <= 0) throw new Error('a bot cycle applied nothing');
        } else {
            const moves = kernelLegalMoves(game, human).filter((m) => m.type !== 'wait' && m.type !== 'pickup');
            const m = moves[0] ?? kernelLegalMoves(game, human)[0];
            table.act(human, encodeAction({ kind: m.type as keyof typeof AWIRE_KIND, cards: m.cards, attack_cards: m.attack_cards }), null, 0);
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
        decodePackedGame: (buf) => decodePackedGame(buf, NOW)!.game.players.length,
        'adopt + snapshot': (buf) => client.adoptEnvelope(buf)!.seats.length,
        decodeEnvelope: (buf) => decodeEnvelope(buf, NOW)!.game.players.length,
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
