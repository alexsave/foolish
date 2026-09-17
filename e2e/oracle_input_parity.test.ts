/* =============================================================================
 * The Oracle's input, built in C, is the input the TS path built
 * (docs/C_GAME_SHAPE_MIGRATION.md Phase 7)
 * =============================================================================
 * The Infinite Oracle deliberates a replay's decision from two byte strings it
 * imports: the board the acting seat decided on (wasm_import_state, masked) and
 * the public log before the move (wasm_import_logs). They used to be assembled in
 * TS - a Game-shaped object out of the replay's frames, marshalled by
 * engine.ts __marshalGame, and a log wire encoded from the decoded log stream
 * (src/oracle/logsWire.ts). The kernel now writes both
 * (c/src/replay_steps.h replay_steps_board_v6, replay_steps_memory_v6), and this
 * holds them to what the TS path wrote, for every decision of the played games
 * the Oracle suites use (e2e/oracle_replay.test.ts, e2e/oracle_mode_b.test.ts).
 *
 * THE LOG is byte-identical.
 *
 * THE BOARD is identical except where the kernel's masking rule and the TS
 * placeholder differ, and those bytes are named here rather than hidden:
 *   - a card the acting seat cannot see (the stock, the other hands) is the
 *     kernel's hidden byte (view.h state_put), where the TS path wrote a five of
 *     spades as a placeholder card;
 *   - with no trump face up, the flipped byte is the hidden byte (state_put's
 *     canonical no-flip byte), where the TS path wrote 0 (an import ignores it);
 *   - the acting seat's turn flag and the good timestamp flag are the replayed
 *     game's own, where the TS path wrote false (no strategy reads either).
 * The expected board is the TS board with exactly those rules applied, the turn
 * and timestamp flags read off the acting seat's own replay frame (evwire, a
 * different kernel path). Before the TS path was deleted, the oracle's dump was
 * also compared on both boards, decision by decision, memory on and off.
 *
 * The TS path's products are recorded in fixtures/oracle_input/parity.json
 * (sha-256 per decision), so the check outlives the code it replaced.
 * ORACLE_INPUT_RECORD=1 rewrites them from the TS path while it exists.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';

import { decodeReplay } from '../server/api/common/replay/decode.ts';
import { bytesToBigint } from '../server/api/common/replay/codec.ts';
import { gunzip } from '../sdk/ts/wasm/gunzip.ts';
import { __marshalGame, __setResident } from '../sdk/ts/wasm/engine.ts';
import { replayEventFrames, replayStepMaskedState, replayStepLogs } from '../sdk/ts/wasm/bots.ts';
import { clientTable } from '../sdk/ts/table/client_table.ts';
import { buildReplayFrames } from '../src/replay/frames.ts';
import { buildOracleJob, findDecisionIndex } from '../src/oracle/replayOracleInput.ts';
import { OracleInstance } from '../src/oracle/oracleBridge.ts';
import { playSeededV6 } from './helpers/seeded_game.ts';

const GOLDEN = 'e2e/fixtures/oracle_input/parity.json';
const RECORD = process.env.ORACLE_INPUT_RECORD === '1';

// The Oracle suites' games: e2e/oracle_replay.test.ts's three and Mode B's one.
const SHAPES: [string, number, number][] = [['3p', 3, 41], ['4p', 4, 42], ['8p', 8, 43], ['2p', 2, 7]];

const HIDDEN = 0xfe;
const sha = (b: Uint8Array | null) => (b ? createHash('sha256').update(b).digest('hex') : null);

/** One decision: its step, the acting seat, and the sha-256 of each input (null: no memory). */
type Row = [step: number, seat: number, state: string, logs: string | null];
type Golden = Record<string, Row[]>;

interface Played {
    code: Uint8Array;
    decoded: Awaited<ReturnType<typeof decodeReplay>>;
    frames: ReturnType<typeof buildReplayFrames>;
    decisions: number[];
}
const played = new Map<string, Played>();

async function game(label: string, np: number, seed: number): Promise<Played> {
    const hit = played.get(label);
    if (hit) return hit;
    const g = await playSeededV6(np, seed);
    assert.ok(g, `${label}: the seeded game finished`);
    const decoded = await decodeReplay(bytesToBigint(g!.code));
    const frames = buildReplayFrames(g!.code, 'g', null, { fool: decoded.fool });
    // Every step's decision, as the replay screen finds it under the cursor.
    const decisions = [...new Set(frames.map((_, i) => findDecisionIndex(frames, i)).filter((j): j is number => j != null))];
    const p = { code: g!.code, decoded, frames, decisions };
    played.set(label, p);
    return p;
}

/* ------------------------------ the TS path ------------------------------ */

/** The bytes __marshalGame wrote for a job's board, captured at its import. */
function tsBoard(blob: unknown): Uint8Array {
    const memory = new WebAssembly.Memory({ initial: 1 });
    let got: Uint8Array | null = null;
    const ex = {
        memory,
        wasm_io_ptr: () => 0,
        wasm_import_state: () => { got = new Uint8Array(memory.buffer).slice(0, 8192); return 0; },
        wasm_import_strategy_keys: () => {},
        wasm_set_deterministic_deck: () => {},
    };
    __setResident(null);
    __marshalGame(ex as never, blob as never, true);
    return got!;
}

/**
 * The TS board under the kernel's rules named in the header: its own length
 * measured off the layout, every card the seat cannot see hidden, the no-flip
 * byte hidden, and the seat's turn flag and the timestamp flag the replay's own.
 */
function expectedBoard(ts: Uint8Array, seat: number, awaiting: boolean, goodTimestamp: boolean): Uint8Array {
    const o = ts.slice();
    let q = 7;
    if (o[q++] === 0) o[q] = HIDDEN;
    q++;
    q += 4;
    o[q++] = goodTimestamp ? 1 : 0;
    const deck = o[q] | (o[q + 1] << 8);
    q += 2;
    for (let i = 0; i < deck; i++) o[q++] = HIDDEN;
    q += 1 + 2 * o[q];
    for (let s = 0; s < ts[1]; s++) {
        q++;
        o[q++] = s === seat && awaiting ? 1 : 0;
        const hand = o[q++];
        for (let i = 0; i < hand; i++, q++) if (s !== seat) o[q] = HIDDEN;
    }
    q += 1 + o[q];
    return o.slice(0, q);
}

/** The acting seat's own board before step `step`, read off its own replay frames. */
function ownBoards(code: Uint8Array, n: number) {
    return Array.from({ length: n }, (_, s) => replayEventFrames(code, s).map((f) => {
        const read = clientTable().readPush(f, { as3: false, identity: 'none' });
        assert.ok(read, 'a replay frame reads');
        return read!.final;
    }));
}

function tsRows(p: Played): { rows: Row[]; states: Uint8Array[]; logs: (Uint8Array | null)[] } {
    const n = p.frames[0].game.seats.length;
    const own = ownBoards(p.code, n);
    const rows: Row[] = [], states: Uint8Array[] = [], logs: (Uint8Array | null)[] = [];
    for (const j of p.decisions) {
        const on = buildOracleJob(p.frames, p.decoded, j, true, 'parity');
        const off = buildOracleJob(p.frames, p.decoded, j, false, 'parity');
        assert.ok(off, `step ${j}: the TS path builds a job`);
        const pre = own[off!.seat][j - 1];
        const state = expectedBoard(tsBoard(off!.gameBlob), off!.seat, pre.seats[off!.seat].awaitingAttack, pre.hasGoodTimestamp);
        const memory = on ? on.logsWire : null;
        rows.push([j, off!.seat, sha(state)!, sha(memory)]);
        states.push(state);
        logs.push(memory);
    }
    return { rows, states, logs };
}

/* -------------------------------- golden --------------------------------- */

function golden(): Golden {
    if (RECORD) return {};
    assert.ok(existsSync(GOLDEN), `${GOLDEN} is recorded (ORACLE_INPUT_RECORD=1)`);
    return JSON.parse(readFileSync(GOLDEN, 'utf8')) as Golden;
}
const recorded: Golden = {};

test('every decision: the C board and memory are the TS path\'s bytes', async () => {
    const want = golden();
    const paths: [string, Played, ReturnType<typeof tsRows>][] = [];
    for (const [label, np, seed] of SHAPES) {
        const p = await game(label, np, seed);
        const ts = tsRows(p);
        if (RECORD) recorded[label] = ts.rows;
        else assert.deepEqual(ts.rows, want[label], `${label}: the TS path still writes the recorded bytes`);
        paths.push([label, p, ts]);
    }
    if (RECORD) {
        mkdirSync('e2e/fixtures/oracle_input', { recursive: true });
        writeFileSync(GOLDEN, JSON.stringify(recorded).replace(/\],\[/g, '],\n[') + '\n');
    }

    let boards = 0, memories = 0, noMemory = 0;
    for (const [label, p, ts] of paths) {
        ts.rows.forEach(([j, seat], i) => {
            const board = replayStepMaskedState(p.code, j, seat);
            assert.deepEqual(board, ts.states[i], `${label} step ${j}: the board seat ${seat} decided on`);
            const memory = replayStepLogs(p.code, j);
            assert.deepEqual(memory, ts.logs[i], `${label} step ${j}: the log before the move`);
            boards++;
            if (memory) memories++; else noMemory++;
        });
    }
    assert.ok(boards > 200 && memories > 150, `covered ${boards} boards, ${memories} memories, ${noMemory} steps with none`);
    console.log(`  [oracle_input_parity] ${boards} decisions: ${memories} memories, ${noMemory} without one`);
});

test('the oracle deliberates the same on the C board as on the TS board', async () => {
    const bytes = gunzip(new Uint8Array(readFileSync('public/oracle.wasm.gz')));
    const run = (inst: OracleInstance, state: Uint8Array, n: number, seat: number, logs: Uint8Array | null, seed: number) => {
        const ex = inst.ex as any;
        const u8 = () => new Uint8Array(ex.memory.buffer);
        u8().set(state, ex.wasm_io_ptr());
        assert.ok(ex.wasm_import_state(1) >= 0, 'the board imports');
        for (let i = 0; i < n; i++) u8()[ex.wasm_io_ptr() + i] = 0xff;
        ex.wasm_import_strategy_keys();
        if (logs && logs.length > 2) { u8().set(logs, ex.wasm_io_ptr()); ex.wasm_import_logs(); }
        ex.wasm_set_strategy_seed(seed >>> 0);
        ex.wasm_og_explain_reset();
        ex.wasm_choose_move(20, seat);
        const at = ex.wasm_og_explain_ptr();
        return new TextDecoder().decode(u8().subarray(at, at + ex.wasm_og_explain_len()));
    };
    const env = { OG_KEEP1: '26', OG_KEEP2: '26', OG_W2: '1', OG_W3: '0', OG_EXPLAIN_SOLVE_BUDGET: '2000000', OG_W1: '8' };
    let same = 0;
    for (const [label, np, seed] of SHAPES) {
        const p = await game(label, np, seed);
        const a = new OracleInstance(); await a.init(bytes); a.writeEnv(env);
        const b = new OracleInstance(); await b.init(bytes); b.writeEnv(env);
        for (const j of p.decisions) {
            for (const memoryOn of [true, false]) {
                const job = buildOracleJob(p.frames, p.decoded, j, memoryOn, 'parity');
                if (!job) continue;
                const ts = run(a, tsBoard(job.gameBlob), np, job.seat, memoryOn ? job.logsWire : null, 0x51 + j);
                const board = replayStepMaskedState(p.code, j, job.seat);
                assert.ok(board, `${label} step ${j}: the kernel writes the board`);
                const c = run(b, board, np, job.seat, memoryOn ? replayStepLogs(p.code, j) : null, 0x51 + j);
                assert.equal(c, ts, `${label} step ${j} memory ${memoryOn ? 'on' : 'off'}: the same deliberation`);
                same++;
            }
        }
    }
    console.log(`  [oracle_input_parity] ${same} deliberations identical`);
});

test('the kernel refuses what is not a decision it can vouch for', async () => {
    const p = await game('3p', 3, 41);
    const steps = p.frames.length;
    assert.equal(replayStepMaskedState(p.code, 0, 0), null, 'the deal was decided by nobody');
    assert.equal(replayStepMaskedState(p.code, steps, 0), null, 'no step past the last');
    assert.equal(replayStepMaskedState(p.code, 1, 3), null, 'a viewer that is not a seat');
    assert.ok(replayStepMaskedState(p.code, 1, -1), 'a spectator is a viewer');
    assert.equal(replayStepLogs(p.code, 0), null, 'the deal has no record');
    assert.equal(replayStepLogs(p.code, steps), null, 'no step past the last');
    const round = p.frames.findIndex((f, i) => i > 0 && f.kind === 6);
    assert.ok(round > 0, 'the game has a round end');
    assert.equal(replayStepLogs(p.code, round), null, 'a round end has no record of its own');
    assert.ok(replayStepMaskedState(p.code, round, -1), 'but it has a board');
});
