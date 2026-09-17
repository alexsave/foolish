/* =============================================================================
 * The C Table's bot cycle writes what today's TS bot cycle writes
 * =============================================================================
 * docs/C_GAME_SHAPE_MIGRATION.md 2.7 and 2.8, for Phase 4b. Before the bot loop
 * and the finalize move onto c/src/table.c, one cycle and one game end must come
 * out of the C Table byte for byte as they come out of today's TS path:
 *
 *   TS today (bot_actions.ts and utils.ts finalizeEndedGame, minus their I/O):
 *     wasmBotDrive(game, { aiMask, humanSeats, logs, prefs }) over the session
 *     log's packed bytes -> the run's state blob, logwire records (pinned clock),
 *     per-viewer event buffers and wasmBotCycleDelayMs; at the end
 *     verifyRoundTripV6FromGame and encodeExtrasBytes(names, moveTimesFromLogs)
 *   C (raw bots.wasm exports on a PRIVATE instance):
 *     wasm_table_load -> wasm_table_set_deal_seed -> wasm_table_import_session_log
 *     (when wasm_table_bots_need_logs) -> wasm_table_bot_drive ->
 *     wasm_table_commit_products / wasm_table_push / wasm_table_cycle_delay_ms;
 *     at the end wasm_table_replay_code and wasm_table_replay_extras
 *
 * Each side carries its own chain (its own state blob and session log). Games:
 * seeded deals of 2 to 5 seats with belief and non-belief brains, one human seat
 * whose moves go through both pipelines (runPackedAction / wasm_table_act), and a
 * bots-only table. A CAS retry is replayed on both sides with the moves the
 * first attempt chose, under a different deal seed so a re-search would differ.
 * ========================================================================== */

import './harness.ts';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { start_game_packed } from '../server/api/common/game_lifecycle.ts';
import { verifyRoundTripV6FromGame } from '../server/api/common/replay/encode.ts';
import { encodeExtrasBytes, moveTimesFromLogs } from '../server/api/common/replay/extras.ts';
import { Game, GAME_STATUS, PLAYER_STATUS, PrivatePlayer, STRATEGY_KEY } from '../server/api/core/types.ts';
import {
    runPackedAction, materializeKernelGame, kernelLegalMoves, __setDealSeedOverride,
} from '../sdk/ts/wasm/engine.ts';
import { BotDrivePref, wasmBotDrive, wasmBotCycleDelayMs, wasmBotEligibleMask, kernelBotRoster } from '../sdk/ts/wasm/bots.ts';
import { decodeLogs, logsFromKernelExport } from '../sdk/ts/wire/logwire.ts';
import { AWIRE_KIND, encodeAction } from '../sdk/ts/wire/awire.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { LAYOUT_HASH } from '../sdk/ts/gen/layout_hash.bots.ts';
import { assertLayoutHash } from '../sdk/ts/wasm/layout_hash.ts';
import { loadWasmGz } from '../sdk/ts/wasm/wasm_asset.ts';
import { ServerTable, type TableExports } from '../sdk/ts/table/server_table.ts';
import { cRosterEncode } from './helpers/roster_kernel.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

// The Phase 4b exports, called raw: server_table.ts wraps them in 4b.
interface CycleExports extends TableExports {
    wasm_table_seat_of(idLen: number): number;
    wasm_table_set_deal_seed(len: number): number;
    wasm_table_import_session_log(len: number): number;
    wasm_table_bot_drive(prefsLen: number, maxActions: number): number;
    wasm_table_drive_ptr(): number;
    wasm_table_drive_prefs(): number;
    wasm_table_cycle_delay_ms(): number;
    wasm_table_replay_code(seedLen: number, logLen: number): number;
    wasm_table_replay_extras(logLen: number): number;
}

const inst = new WebAssembly.Instance(new WebAssembly.Module(loadWasmGz('bots') as BufferSource), {});
const ex = inst.exports as unknown as CycleExports;
assertLayoutHash('bots.wasm', ex, LAYOUT_HASH, 'sdk/ts/gen/layout_hash.bots.ts');
ex.wasm_init();
const table = new ServerTable(ex);
const enc = new TextEncoder();
const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');
const u8 = () => new Uint8Array(ex.memory.buffer);
const put = (...parts: Uint8Array[]): number[] => {
    let at = ex.wasm_io_ptr();
    return parts.map((p) => { u8().set(p, at); at += p.length; return p.length; });
};
const io = (n: number) => u8().slice(ex.wasm_io_ptr(), ex.wasm_io_ptr() + n);
const concat = (a: Uint8Array, b: Uint8Array) => { const o = new Uint8Array(a.length + b.length); o.set(a); o.set(b, a.length); return o; };

const NOW0 = 1_726_600_000_000;
const MOVE_NAMES = ['attack', 'cover', 'pass', 'pickup', 'good', 'wait'];

interface Seat { player_id: string; name: string; is_ai: boolean; strategy_key: string }

/** One game on both pipelines. */
interface Pair {
    id: string;
    title: string;
    seed: Uint8Array;
    seats: Seat[];
    game: Game;            // TS: the loop's Game object
    tsState: Uint8Array;
    tsLog: Uint8Array;     // games.logs_packed as TS appends it
    cState: Uint8Array;
    cRoster: Uint8Array;
    cLog: Uint8Array;
    clock: number;
}

function deal(k: number, brains: (string | null)[]): Pair {
    const seats: Seat[] = brains.map((b, i) => ({
        player_id: `00000000-0000-4000-8000-${String(k * 16 + i).padStart(12, '0')}`,
        name: ['Дмитрий', 'Zoë 🃏', 'B2', '田中花子', 'B4'][i], is_ai: b !== null, strategy_key: b ?? STRATEGY_KEY.HUMAN,
    }));
    const game: Game = {
        id: `tbp${k}`, name: `Bot parity ${k}`, version: 0, deck: [], deck_length: 0, discard_pile_length: 0, flipped: null,
        players: seats.map((s): PrivatePlayer => ({ ...s, status: PLAYER_STATUS.READY, hand: [], hand_length: 0, awaiting_attack: false } as PrivatePlayer)),
        status: GAME_STATUS.WAITING, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [],
        elimination_order: [], good_timestamp: null, good_players: [], logs: [],
    };
    const seed = Uint8Array.from({ length: 32 }, (_, i) => (i * 37 + k * 11 + 5) & 0xff);
    __setDealSeedOverride(seed);
    const run = start_game_packed(game);
    __setDealSeedOverride(null);
    assert.equal(game.game_seed, hex(seed), 'the deal records its seed');
    const roster = cRosterEncode(game.name, seats.map((s) => ({ id: s.player_id, name: s.name, brain: s.is_ai ? s.strategy_key : '' })));
    assert.ok(roster instanceof Uint8Array, `the roster encodes (${roster})`);
    const log = logsFromKernelExport(run.logsWire, NOW0);
    return { id: game.id, title: game.name, seed, seats, game, tsState: run.stateBlob, tsLog: log, cState: run.stateBlob, cRoster: roster, cLog: log, clock: NOW0 };
}

const humanSeats = (p: Pair) => p.seats.map((s, i) => (s.is_ai ? -1 : i)).filter((i) => i >= 0);
const aiMask = (p: Pair) => p.seats.reduce((m, s, i) => (s.is_ai ? m | (1 << i) : m), 0);

function cLoad(p: Pair, seedHex: string): void {
    assert.equal(table.load(p.cState, p.cRoster), L.TABLE_OK, `${p.id}: the C row loads`);
    const [n] = put(enc.encode(seedHex));
    assert.equal(ex.wasm_table_set_deal_seed(n), L.TABLE_OK);
}

interface CycleOut { actions: { seat: number; move: string; cards: number[] }[]; stop: number; delay: number }

function cDriveOut(n: number): CycleOut {
    const m = L.memOf(ex.memory.buffer);
    const d = ex.wasm_table_drive_ptr();
    const actions = [];
    for (let i = 0; i < n; i++) {
        const a = L.BotDriveOut_actions_at(d, i);
        const mv = L.BotDriveAction_move_at(a);
        const k = L.LegalMove_get_n_cards(m, mv);
        const cards: number[] = [];
        const type = MOVE_NAMES[L.LegalMove_get_type(m, mv)];
        for (let c = 0; c < k; c++) cards.push(m.u8[L.LegalMove_cards_at(mv, c)]);
        for (let c = 0; c < k && type === 'cover'; c++) cards.push(m.u8[L.LegalMove_attack_cards_at(mv, c)]);
        actions.push({ seat: L.BotDriveAction_get_seat(m, a), move: type, cards });
    }
    return { actions, stop: L.BotDriveOut_get_stop(m, d), delay: ex.wasm_table_cycle_delay_ms() };
}

// Cards as the kernel's in-memory byte, so the two sides compare as numbers.
const cardByte = (c: { suit: number; value: number }) => (((c.value << 3) | (c.suit & 7)) & 0xff);

const counts = { cycles: 0, logged: 0, actions: 0, humanMoves: 0, pushes: 0, retries: 0, games: 0, codes: 0 };

/** One bot cycle on both sides, compared. `prefs` replays an earlier attempt's choices. */
function cycle(label: string, p: Pair, opts: { seedHex?: string; tsPrefs?: BotDrivePref[]; cPrefs?: Uint8Array; commit: boolean }): { ts: ReturnType<typeof wasmBotDrive>; c: CycleOut; cPrefs: Uint8Array } {
    const seedHex = opts.seedHex ?? hex(p.seed);
    const game = opts.commit ? p.game : structuredClone(p.game);
    game.game_seed = seedHex;
    const eligible = wasmBotEligibleMask(game);
    const roster = kernelBotRoster();
    const usesLogs = game.players.some((pl, i) => ((eligible >> i) & 1) !== 0 && roster.find((e) => e.key === pl.strategy_key)?.usesLogs === true);
    if (usesLogs) game.belief_log_bytes = p.tsLog;
    const ts = wasmBotDrive(game, { aiMask: aiMask(p), humanSeats: humanSeats(p), logs: usesLogs, prefs: opts.tsPrefs ?? [] });
    const tsDelay = wasmBotCycleDelayMs();
    game.belief_log_bytes = undefined;

    cLoad(p, seedHex);
    assert.equal(table.botsNeedLogs(), usesLogs, `${label}: bots_need_logs`);
    if (usesLogs) {
        const [n] = put(p.cLog);
        assert.ok(ex.wasm_table_import_session_log(n) >= 0, `${label}: the session log imports`);
        counts.logged++;
    }
    const [pl] = put(opts.cPrefs ?? new Uint8Array(0));
    const n = ex.wasm_table_bot_drive(pl, 0);
    assert.ok(n >= 0, `${label}: the C drive runs (${n})`);
    const c = cDriveOut(n);
    const cPrefs = io(ex.wasm_table_drive_prefs());

    assert.deepEqual(c.actions, ts.actions.map((a) => ({
        seat: a.seat, move: a.move.type,
        cards: [...(a.move.cards ?? []), ...(a.move.type === 'cover' ? a.move.attack_cards ?? [] : [])].map(cardByte),
    })), `${label}: the same actions`);
    assert.equal(c.stop, ts.stop, `${label}: the same stop`);
    assert.equal(c.delay, tsDelay, `${label}: the same delay`);
    if (!ts.run) return { ts, c, cPrefs };

    counts.actions += n;
    const nowMs = ++p.clock;
    const products = table.commit(p.id, 1, nowMs);
    assert.ok(typeof products !== 'number', `${label}: commit products (${products})`);
    assert.equal(hex(products.state), hex(ts.run.stateBlob), `${label}: state blob`);
    const tsRecords = logsFromKernelExport(ts.run.logsWire, nowMs);
    assert.equal(hex(products.logs ?? new Uint8Array(0)), hex(tsRecords), `${label}: the cycle's own session-log records`);
    assert.equal(products.ended, ts.run.ended, `${label}: ended`);
    assert.equal(products.nEvents, ts.run.nEvents, `${label}: event count`);
    for (const [viewer, bytes] of ts.run.events) {
        const as3 = table.push(p.id, viewer);
        assert.ok(as3 instanceof Uint8Array, `${label}: push ${viewer} (${as3})`);
        assert.equal(hex(as3.subarray(0, as3.length - 1)), hex(bytes), `${label}: viewer ${viewer} event bytes`);
        counts.pushes++;
    }
    if (opts.commit) {
        p.tsState = ts.run.stateBlob;
        p.tsLog = concat(p.tsLog, tsRecords);
        p.cState = products.state;
        p.cLog = concat(p.cLog, products.logs ?? new Uint8Array(0));
        counts.cycles++;
    }
    return { ts, c, cPrefs };
}

/** The human seat's first legal move, through both pipelines. */
function humanMove(label: string, p: Pair): boolean {
    const seat = p.seats.findIndex((s) => !s.is_ai);
    if (seat < 0 || p.game.players[seat].status !== PLAYER_STATUS.IN) return false;
    const moves = kernelLegalMoves(p.game, p.seats[seat].player_id).filter((m) => m.type !== 'wait');
    if (moves.length === 0) return false;
    const m = moves[moves.length > 1 && moves[0].type === 'pickup' ? 1 : 0];
    const wire = encodeAction({ kind: m.type as keyof typeof AWIRE_KIND, cards: m.cards, attack_cards: m.attack_cards });
    const run = runPackedAction(p.tsState, seat, wire, aiMask(p), humanSeats(p));
    assert.ok(run.ok, `${label}: TS applies the human move`);
    p.game = materializeKernelGame(run.post, {
        id: p.id, name: p.title, deck_length: 0, players: p.seats, good_players: p.game.good_players, good_timestamp: p.game.good_timestamp,
    }, p.seats[seat].player_id);
    p.game.game_seed = hex(p.seed);
    p.game.deterministic_deck = run.stateBlob[1] !== 0;   // loadCompleteGame reads it off the blob
    // Today's action path sets no deal seed: its draws are seeded from whatever
    // base the module last had (packed_action.ts), and so is table_load's.
    assert.equal(table.load(p.cState, p.cRoster), L.TABLE_OK, `${label}: the C row loads`);
    assert.equal(table.act(p.seats[seat].player_id, wire, null, 0), L.TABLE_APPLIED, `${label}: C applies the human move`);
    const nowMs = ++p.clock;
    const products = table.commit(p.id, 1, nowMs);
    assert.ok(typeof products !== 'number');
    assert.equal(hex(products.state), hex(run.stateBlob), `${label}: human move state blob`);
    const records = logsFromKernelExport(run.logsWire, nowMs);
    p.tsState = run.stateBlob;
    p.tsLog = concat(p.tsLog, records);
    p.cState = products.state;
    p.cLog = concat(p.cLog, products.logs ?? new Uint8Array(0));
    counts.humanMoves++;
    return true;
}

function playToEnd(label: string, p: Pair, maxSteps = 3000): void {
    for (let step = 0; step < maxSteps; step++) {
        if (p.game.status !== GAME_STATUS.PLAYING) break;
        if (wasmBotEligibleMask(p.game) !== 0) {
            // Every so often, a commit that lost its CAS: the retry is offered what the attempt chose.
            if (step % 7 === 3) {
                const first = cycle(`${label} step ${step} (lost attempt)`, p, { commit: false });
                if (first.ts.actions.length > 0) {
                    counts.retries++;
                    cycle(`${label} step ${step} (retry)`, p, {
                        seedHex: 'ff'.repeat(32), commit: true,
                        tsPrefs: first.ts.actions.map((a) => ({ seat: a.seat, move: a.move })), cPrefs: first.cPrefs,
                    });
                    continue;
                }
            }
            const r = cycle(`${label} step ${step}`, p, { commit: true });
            if (r.ts.actions.length === 0 && !humanMove(`${label} step ${step}`, p)) break;
        } else if (!humanMove(`${label} step ${step}`, p)) {
            break;
        }
    }
    assert.equal(p.game.status, GAME_STATUS.GAME_OVER, `${label}: played to its end`);
    counts.games++;
}

// The game end: the verified v6 code and the extras blob.
async function gameEnd(label: string, p: Pair): Promise<void> {
    const { encoded } = await verifyRoundTripV6FromGame(p.game, p.seed, decodeLogs(p.tsLog, p.id, p.game.players), p.tsLog);
    const tsExtras = encodeExtrasBytes(p.game.players.map((pl) => pl.name), moveTimesFromLogs(decodeLogs(p.tsLog, p.id, p.game.players)));
    cLoad(p, hex(p.seed));
    const [sl, ll] = put(p.seed, p.cLog);
    const codeLen = ex.wasm_table_replay_code(sl, ll);
    assert.ok(codeLen > 0, `${label}: the C replay code (${codeLen})`);
    assert.equal(hex(io(codeLen)), hex(encoded.bytes), `${label}: the replay code`);
    cLoad(p, hex(p.seed));
    const [el] = put(p.cLog);
    const extrasLen = ex.wasm_table_replay_extras(el);
    assert.ok(extrasLen > 0, `${label}: the C extras (${extrasLen})`);
    assert.equal(hex(io(extrasLen)), hex(tsExtras), `${label}: the extras`);
    assert.equal(hex(p.cLog), hex(p.tsLog), `${label}: the two session logs`);
    counts.codes++;
}

test('bot cycles, CAS retries and the game end: the C Table and the TS path write the same bytes', async () => {
    const games: (string | null)[][] = [
        [null, 'blackpowder'],
        [null, 'cordite', 'random'],
        [null, 'robusta', 'handwritten', 'firecracker'],
        [null, 'random', 'firecracker', 'blackpowder', 'simple_heuristic'],
        ['random', 'robusta', 'handwritten'],
    ];
    // Every game is played before any game end is encoded: the replay encoder and
    // its gate replay a whole game through the module they run on, and robusta and
    // firecracker sample the draw stream the module's LAST apply left (bots.ts
    // __seedDrawRngFromState), so the two modules' histories must match move for move.
    const played: [string, Pair][] = [];
    for (let k = 0; k < games.length; k++) {
        const label = `game ${k} (${games[k].map((b) => b ?? 'human').join(', ')})`;
        const p = deal(k, games[k]);
        playToEnd(label, p);
        played.push([label, p]);
    }
    for (const [label, p] of played) await gameEnd(label, p);
    console.error(`[table_bot_parity] ${JSON.stringify(counts)}`);
    assert.ok(counts.logged > 0 && counts.retries > 0 && counts.humanMoves > 0 && counts.codes === games.length,
        `every path was exercised (${JSON.stringify(counts)})`);
});

test('seat_of: the loaded roster\'s seat for an id, exactly', () => {
    const p = deal(9, [null, 'random', 'random']);
    const seatOf = (id: string) => { const [n] = put(enc.encode(id)); return ex.wasm_table_seat_of(n); };
    table.load(p.cState, p.cRoster);
    assert.equal(seatOf(p.seats[0].player_id), 0);
    assert.equal(seatOf(p.seats[2].player_id), 2);
    assert.equal(seatOf(p.seats[0].player_id.slice(0, 20)), -1, 'a prefix is no seat');
    assert.equal(seatOf(''), -1, 'the empty id is no seat');
    assert.equal(seatOf('stranger'), -1);
});
