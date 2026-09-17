/* =============================================================================
 * The C Table writes what today's TS pipeline writes (the cutover gate)
 * =============================================================================
 * docs/C_GAME_SHAPE_MIGRATION.md Phase 3. Before any server path moves onto
 * c/src/table.c (Phase 4b), every product the move path commits and broadcasts
 * must come out of the C Table byte for byte as it comes out of today's TS
 * pipeline, on the same games:
 *
 *   TS today (packed_action.ts executePackedAction, minus its I/O):
 *     runPackedAction -> materializeKernelGame -> logsFromKernelExport(pinned
 *     clock) -> logwireHexClosesRound -> buildPlayerViewRows /
 *     buildSpectatorView -> the per-viewer event buffers broadcast as as2 `b`
 *   C (sdk/ts/table/server_table.ts over a PRIVATE bots.wasm instance):
 *     table_load -> table_act -> table_commit_products -> table_push
 *
 * Each side carries its OWN chain - the TS side feeds its own state blob to its
 * next move and the C side its own - so a drift anywhere surfaces at the step
 * that introduced it rather than being laundered by a shared state.
 *
 * Games driven: every dealt scenario of the pre-migration fixture
 * (e2e/fixtures/pre_table, the rows the hosted database holds) played on with
 * legal moves and hostile requests to its end, and fresh deals driven by the
 * adversarial generator of e2e/fuzz.test.ts (e2e/helpers/fuzz_moves.ts). Every
 * lobby row of the fixture is compared at load.
 *
 * Asserted per step: the outcome class (applied / rejected with the same kernel
 * reason / moot / not seated / malformed wire) and the response body; for an
 * applied move the state blob, the session-log records, closed_round, the
 * status column, needs_bots, every human seat's envelope, the spectator
 * envelope, the event count, and each viewer's as2 bytes (as3 = as2 + a flags
 * byte, 0 on a move). At a game's end also the finish order.
 *
 * INTENTIONAL DIVERGENCES, each decided in the plan and pinned here explicitly:
 *   Q1  The envelope's roster trailer: TS writes the good ids in the order they
 *       were said plus the good timestamp's value; C writes them in seat order
 *       and has_ts = 0 (no reader uses either, plan 5.4). The TS envelopes are
 *       therefore built from a Game normalized to seat-ordered good ids and a
 *       null timestamp, and a separate assertion proves the unnormalized TS
 *       trailer differs from C's in exactly those two things: same ids, names,
 *       bot flags, title, status and the same SET of good players.
 *   Q7  Lobby-op message prose and the r/m push extras (Phase 3.ii).
 * ========================================================================== */

import './harness.ts';
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { applySchema, pgPool, uuid } from './harness.ts';
import { loadCompleteGame } from '../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { buildPlayerViewRows, buildSpectatorView } from '../server/api/common/player_views.ts';
import { start_game_packed } from '../server/api/common/game_lifecycle.ts';
import { calculateGameRankings } from '../server/api/common/finish_order.ts';
import { calculateEloChange } from '../server/api/common/common_utils.ts';
import {
    runPackedAction, materializeKernelGame, serializeGameState, kernelLegalMoves,
    __setDealSeedOverride, __setEngineClock,
} from '../sdk/ts/wasm/engine.ts';
import { kernelBotRoster, wasmBotEligibleMask } from '../sdk/ts/wasm/bots.ts';
import { logsFromKernelExport, logwireHexClosesRound } from '../sdk/ts/wire/logwire.ts';
import { bytesToBareHex } from '../sdk/ts/wire/bytes.ts';
import { decodePackedRoster } from '../sdk/ts/wire/roster.ts';
import {
    AWIRE_KIND, encodeAction, encodeActionRequest, encodeActionResponse, decodeActionRequest,
    ACTION_STATUS, REJECT_STALE_ROUND, wireCard,
} from '../sdk/ts/wire/awire.ts';
import { bytesToHex, hexToBytes } from '../server/api/common/replay/codec.ts';
import { Game, GAME_STATUS, PLAYER_STATUS, PrivatePlayer, STRATEGY_KEY } from '../server/api/core/types.ts';
import { createServerTable, TableProducts } from '../sdk/ts/table/server_table.ts';
import {
    GAME_INVALID_LOBBY_CARDS, GAME_STATUS_GAME_OVER, GAME_STATUS_PLAYING, GAME_STATUS_WAITING,
    TABLE_APPLIED, TABLE_E_NOT_SEATED, TABLE_E_WIRE, TABLE_MOOT, TABLE_OK, TABLE_REJECTED, TABLE_STALE_ROUND,
} from '../sdk/ts/gen/game_layout.bots.ts';
import { cRosterEncode } from './helpers/roster_kernel.ts';
import { FuzzReq, fuzzGenerators, fuzzRng } from './helpers/fuzz_moves.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const NOW = 1_726_531_200_123;                     // the pinned clock both sides stamp records with
const FUZZ_GAMES = Number(process.env.TABLE_PARITY_FUZZ_GAMES || 6);
const FUZZ_STEPS = Number(process.env.TABLE_PARITY_FUZZ_STEPS || 250);
const FIXTURE_STEPS = Number(process.env.TABLE_PARITY_FIXTURE_STEPS || 4000);

const table = createServerTable();
const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');
const STATUS_INT: Record<string, number> = {
    [GAME_STATUS.WAITING]: GAME_STATUS_WAITING, [GAME_STATUS.PLAYING]: GAME_STATUS_PLAYING,
    [GAME_STATUS.GAME_OVER]: GAME_STATUS_GAME_OVER,
};

// ---- the two chains -----------------------------------------------------------

interface Seat { player_id: string; name: string; is_ai: boolean; strategy_key: string }

/** One game as today's TS pipeline holds it between requests: the row. */
interface TsRow {
    id: string;
    title: string;
    version: number;
    status: string;
    state: Uint8Array;
    players: Seat[];
    good_players: string[];
    good_timestamp: number | null;
    /** The full board, for choosing moves. */
    game: Game;
}

/** The same game as the C Table holds it: two blobs. */
interface CRow { state: Uint8Array; roster: Uint8Array }

function rosterBytes(title: string, players: Seat[]): Uint8Array {
    const r = cRosterEncode(title, players.map((p) => ({ id: p.player_id, name: p.name, brain: p.is_ai ? p.strategy_key : '' })));
    assert.ok(r instanceof Uint8Array, `the C roster encoder refused the table (${r})`);
    return r;
}

// Q1: the Game the TS envelopes are built from, with the good ids in seat order
// and no timestamp - the two things the C trailer deliberately does not carry.
function q1Normalized(game: Game): Game {
    return {
        ...game,
        good_players: game.players.filter((p) => game.good_players.includes(p.player_id)).map((p) => p.player_id),
        good_timestamp: null,
    };
}

interface TsViews { views: Map<string, string>; spectator: string; raw: Map<string, string> }

async function tsViews(game: Game, stateHex: string | null, version: number): Promise<TsViews> {
    const norm = q1Normalized(game);
    const rows = await buildPlayerViewRows(norm, stateHex, version);
    const raw = await buildPlayerViewRows(game, stateHex, version);
    return {
        views: new Map(rows.map((r) => [r.player_id, r.view])),
        spectator: await buildSpectatorView(norm, stateHex, version),
        raw: new Map(raw.map((r) => [r.player_id, r.view])),
    };
}

// The JSONB predicate the heartbeat scan and the expand migration read.
const tsNeedsBots = (g: Game) => g.status === GAME_STATUS.PLAYING && g.players.some((p) => p.is_ai && p.status === PLAYER_STATUS.IN);

// bot_actions.ts: a belief bot is eligible to act.
function tsBotsNeedLogs(g: Game): boolean {
    if (g.status !== GAME_STATUS.PLAYING) return false;
    const eligible = wasmBotEligibleMask(g);
    const roster = kernelBotRoster();
    return g.players.some((p, i) => ((eligible >> i) & 1) !== 0 && roster.find((e) => e.key === p.strategy_key)?.usesLogs === true);
}

type Outcome =
    | { kind: 'moot' }
    | { kind: 'not_seated' }
    | { kind: 'wire' }
    | { kind: 'rejected'; reason: number }
    | { kind: 'applied' };

interface TsApplied {
    next: TsRow;
    logs: Uint8Array | null;
    closedRound: boolean;
    views: TsViews;
    events: Map<number, Uint8Array>;
    nEvents: number;
    ended: boolean;
}

// executePackedAction after its load and before its commit, with every I/O cut out.
async function tsAct(row: TsRow, userId: unknown, wire: Uint8Array): Promise<{ outcome: Outcome; applied?: TsApplied }> {
    if (row.status === GAME_STATUS.GAME_OVER) return { outcome: { kind: 'moot' } };
    const seat = row.players.findIndex((p) => p.player_id === userId);
    if (seat < 0) return { outcome: { kind: 'not_seated' } };
    let aiMask = 0;
    const humanSeats: number[] = [];
    row.players.forEach((p, i) => { if (p.is_ai) aiMask |= 1 << i; else humanSeats.push(i); });
    let run;
    try {
        run = runPackedAction(row.state, seat, wire, aiMask, humanSeats);
    } catch (e) {
        if (/malformed action wire/.test(String((e as Error).message))) return { outcome: { kind: 'wire' } };
        throw e;
    }
    if (!run.ok) return { outcome: { kind: 'rejected', reason: run.reason } };
    const game = materializeKernelGame(run.post, {
        id: row.id, name: row.title, version: row.version, deck_length: 0,
        players: row.players.map((p) => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai, strategy_key: p.strategy_key })),
        good_players: row.good_players, good_timestamp: row.good_timestamp,
    }, userId as string);
    const logs = run.logsWire.length > 2 ? logsFromKernelExport(run.logsWire, NOW) : null;
    const closedRound = logs ? logwireHexClosesRound(bytesToBareHex(logs)) : false;
    const version = row.version + 1;
    const views = await tsViews(game, bytesToHex(run.stateBlob), version);
    return {
        outcome: { kind: 'applied' },
        applied: {
            next: {
                ...row, version, status: game.status, state: run.stateBlob, game,
                good_players: game.good_players, good_timestamp: game.good_timestamp,
            },
            logs, closedRound, views, events: run.events, nEvents: run.nEvents, ended: run.ended,
        },
    };
}

function cOutcome(rc: number): Outcome {
    switch (rc) {
        case TABLE_APPLIED: return { kind: 'applied' };
        case TABLE_MOOT: return { kind: 'moot' };
        case TABLE_E_NOT_SEATED: return { kind: 'not_seated' };
        case TABLE_E_WIRE: return { kind: 'wire' };
        case TABLE_REJECTED: return { kind: 'rejected', reason: table.reject() };
        default: throw new Error(`table_act returned ${rc}`);
    }
}

const counts = { steps: 0, applied: 0, rejected: 0, moot: 0, notSeated: 0, wire: 0, views: 0, pushes: 0, games: 0, ended: 0 };

// The views and scalars of a loaded table against the TS builders' for the same row.
function assertViews(label: string, row: { id: string; game: Game }, p: TableProducts, ts: TsViews): void {
    const seats = table.seats();
    assert.equal(p.numPlayers, row.game.players.length, `${label}: seat count`);
    for (let s = 0; s < row.game.players.length; s++) {
        const pid = row.game.players[s].player_id;
        assert.equal(seats[s].id, pid, `${label}: seat ${s} id`);
        const want = ts.views.get(pid);
        if (row.game.players[s].is_ai) {
            assert.equal(p.views[s], null, `${label}: bot seat ${s} has no envelope`);
            assert.equal(want, undefined, `${label}: nor a TS view row`);
            continue;
        }
        assert.ok(p.views[s], `${label}: human seat ${s} has an envelope`);
        assert.equal(hex(p.views[s]!), want, `${label}: seat ${s} envelope`);
        counts.views++;
        // Q1, explicitly: the unnormalized TS trailer differs only in good order and timestamp.
        const raw = hexToBytes(ts.raw.get(pid)!);
        const viewLen = raw[9] | (raw[10] << 8);
        const tsTrailer = decodePackedRoster(raw, 11 + viewLen)!.roster;
        const cTrailer = decodePackedRoster(p.views[s]!, 11 + viewLen)!.roster;
        assert.deepEqual({ ...cTrailer, good_players: [...cTrailer.good_players].sort(), good_timestamp: null },
            { ...tsTrailer, good_players: [...tsTrailer.good_players].sort(), good_timestamp: null },
            `${label}: seat ${s} trailer differs from TS in more than the good order and timestamp`);
        assert.equal(cTrailer.good_timestamp, null, `${label}: C writes no good timestamp`);
    }
    assert.equal(hex(p.spectator), ts.spectator, `${label}: spectator envelope`);
    counts.views++;
}

async function step(label: string, ts: TsRow, c: CRow, userId: unknown, wire: Uint8Array): Promise<{ ts: TsRow; c: CRow; ended: boolean }> {
    counts.steps++;
    const t = await tsAct(ts, userId, wire);
    assert.equal(table.load(c.state, c.roster), TABLE_OK, `${label}: the C row loads`);
    const rc = table.act(typeof userId === 'string' ? userId : '', wire, null, 0);
    const co = cOutcome(rc);
    assert.deepEqual(co, t.outcome, `${label}: outcome (wire ${hex(wire)}, actor ${String(userId)})`);

    // The response body, for every outcome a response is written for.
    if (t.outcome.kind === 'applied' || t.outcome.kind === 'rejected' || t.outcome.kind === 'moot') {
        const status = { applied: ACTION_STATUS.APPLIED, rejected: ACTION_STATUS.REJECTED, moot: ACTION_STATUS.MOOT }[t.outcome.kind];
        const reason = t.outcome.kind === 'rejected' ? t.outcome.reason : 0;
        const version = t.outcome.kind === 'applied' ? ts.version + 1 : ts.version;
        assert.equal(hex(table.actionResponse(rc, table.reject(), version)), hex(encodeActionResponse(status, reason, version)),
            `${label}: response body`);
    }
    switch (t.outcome.kind) {
        case 'moot': counts.moot++; return { ts, c, ended: true };
        case 'not_seated': counts.notSeated++; return { ts, c, ended: false };
        case 'wire': counts.wire++; return { ts, c, ended: false };
        case 'rejected': counts.rejected++; return { ts, c, ended: false };
    }
    counts.applied++;
    const a = t.applied!;
    const p = table.commit(ts.id, a.next.version, NOW);
    assert.ok(typeof p !== 'number', `${label}: commit products (${p})`);
    assert.equal(hex(p.state), hex(a.next.state), `${label}: state blob`);
    assert.equal(p.logs ? hex(p.logs) : null, a.logs ? hex(a.logs) : null, `${label}: session-log records`);
    assert.equal(p.closedRound, a.closedRound, `${label}: closed_round`);
    assert.equal(p.logsReset, false, `${label}: a move never resets the session log`);
    assert.equal(p.status, STATUS_INT[a.next.status], `${label}: status column`);
    assert.equal(p.ended, a.ended, `${label}: ended`);
    assert.equal(p.needsBots, tsNeedsBots(a.next.game), `${label}: needs_bots`);
    assert.equal(table.needsBots(), p.needsBots, `${label}: table_needs_bots agrees with the commit`);
    assert.equal(table.botsNeedLogs(), tsBotsNeedLogs(a.next.game), `${label}: bots_need_logs`);
    assert.equal(p.nEvents, a.nEvents, `${label}: event count`);
    assertViews(label, { id: ts.id, game: a.next.game }, p, a.views);
    for (const [viewer, as2] of a.events) {
        const as3 = table.push(ts.id, viewer);
        assert.ok(as3 instanceof Uint8Array, `${label}: push for viewer ${viewer} (${as3})`);
        assert.equal(hex(as3.subarray(0, as3.length - 1)), hex(as2), `${label}: viewer ${viewer} as2 bytes`);
        assert.equal(as3[as3.length - 1], 0, `${label}: a move's as3 flags byte is 0`);
        counts.pushes++;
    }
    if (a.ended) {
        counts.ended++;
        const ids = table.seats().map((s) => s.id);
        assert.deepEqual(table.rankings().map((s) => ids[s]), calculateGameRankings(a.next.game), `${label}: finish order`);
        assert.equal(p.fool, a.next.game.players.findIndex((pl) => !a.next.game.elimination_order.includes(pl.player_id)),
            `${label}: fool`);
    }
    return { ts: a.next, c: { state: p.state, roster: p.roster }, ended: a.ended };
}

// ---- move choice ----------------------------------------------------------------

function legalWires(game: Game): { actor: string; wire: Uint8Array }[] {
    const out: { actor: string; wire: Uint8Array }[] = [];
    for (const pl of game.players) {
        if (pl.status !== PLAYER_STATUS.IN) continue;
        for (const m of kernelLegalMoves(game, pl.player_id)) {
            if (m.type === 'wait') continue;
            out.push({ actor: pl.player_id, wire: encodeAction({ kind: m.type as keyof typeof AWIRE_KIND, cards: m.cards, attack_cards: m.attack_cards }) });
        }
    }
    return out;
}

// A hostile request as the bytes a client would POST: the awire encoding when
// one exists, else the raw bytes the fields would put on the wire.
function fuzzWire(req: FuzzReq): Uint8Array {
    const kind = (AWIRE_KIND as Record<string, number>)[req.type];
    const cards = (x: unknown) => (Array.isArray(x) ? x : []);
    const main = cards(req.type === 'cover' ? req.cover_cards : req.cards);
    try {
        if (kind === undefined || !Object.prototype.hasOwnProperty.call(AWIRE_KIND, req.type)) throw new Error('no kind');
        return encodeAction({ kind: req.type as keyof typeof AWIRE_KIND, cards: main, attack_cards: cards(req.attack_cards) });
    } catch {
        const raw = [kind ?? 0xee, main.length & 0xff, ...main.slice(0, 40).map((c) => wireCard(c ?? { suit: 0, value: 1 }))];
        return Uint8Array.from(raw);
    }
}

async function drive(label: string, ts: TsRow, c: CRow, steps: number, seed: number): Promise<void> {
    counts.games++;
    const rng = fuzzRng(seed);
    const gens = fuzzGenerators(rng, uuid);
    for (let i = 0; i < steps; i++) {
        let actor: unknown, wire: Uint8Array;
        const legal = ts.status === GAME_STATUS.PLAYING ? legalWires(ts.game) : [];
        if (legal.length > 0 && rng.rnd() < 0.6) {
            ({ actor, wire } = rng.pick(legal));
        } else {
            const req = rng.pick(gens)(ts.game);
            actor = req.player_id;
            wire = fuzzWire(req);
        }
        const r = await step(`${label} step ${i}`, ts, c, actor, wire);
        ts = r.ts; c = r.c;
        if (r.ended) {
            // One more request after the end: moot on both sides.
            const again = await step(`${label} after the end`, ts, c, ts.players[0].player_id, Uint8Array.of(AWIRE_KIND.pickup, 0));
            assert.ok(again.ended, `${label}: a move after the end is moot`);
            return;
        }
    }
}

// ---- the fixture ----------------------------------------------------------------

const FIXTURE = join(process.cwd(), 'e2e', 'fixtures', 'pre_table');

if (!process.env.VALIDATION_ONLY) {
    before(async () => {
        __setEngineClock(() => NOW);
        await applySchema();
        await pgPool.query(readFileSync(join(process.cwd(), 'e2e', 'schema.sql'), 'utf8'));
        await pgPool.query(readFileSync(join(FIXTURE, 'schema.sql'), 'utf8'));
        await pgPool.query(readFileSync(join(FIXTURE, 'rows.sql'), 'utf8'));
    });

    test('every pre-migration row: the C Table loads it and writes the envelopes today\'s builders write', async () => {
        const { rows } = await pgPool.query('SELECT id, name, status::text, state, version, good_players, good_timestamp FROM games ORDER BY id');
        assert.equal(rows.length, 8, 'the fixture has its eight scenarios');
        let dealt = 0, lobbies = 0;
        for (const row of rows) {
            const game = await loadCompleteGame(row.id);
            const players: Seat[] = game.players.map((p) => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai, strategy_key: p.strategy_key as string }));
            const roster = rosterBytes(row.name, players);
            const version = Number(row.version) + 1;
            let state: Uint8Array;
            if (row.status === GAME_STATUS.WAITING) {
                lobbies++;
                if (row.state) {
                    // The damaged shape the TS loaders guard against. The blob is
                    // authoritative (Q6) and says the game is over, which is exactly
                    // why the expand migration rewrites every WAITING row's state
                    // (plan 3.4 step 4) instead of trusting it; relabelled WAITING,
                    // the same board is refused as a lobby with cards.
                    const stale = hexToBytes(row.state);
                    assert.equal(table.load(stale, roster), TABLE_OK, `${row.id}: the finished blob itself loads`);
                    const sp = table.commit(row.id, version, NOW);
                    assert.ok(typeof sp !== 'number' && sp.status === GAME_STATUS_GAME_OVER,
                        `${row.id}: as the finished game it is, not as the lobby the column says`);
                    const relabelled = stale.slice();
                    relabelled[2] = GAME_STATUS_WAITING;
                    assert.equal(table.load(relabelled, roster), GAME_INVALID_LOBBY_CARDS,
                        `${row.id}: relabelled WAITING it is refused as a lobby with cards`);
                }
                // The lobby blob the expand migration backfills (plan 3.4): the kernel's own lobby state.
                state = serializeGameState(game);
            } else {
                dealt++;
                state = hexToBytes(row.state);
            }
            assert.equal(table.load(state, roster), TABLE_OK, `${row.id}: loads`);
            const p = table.commit(row.id, version, NOW);
            assert.ok(typeof p !== 'number', `${row.id}: commit products (${p})`);
            assert.equal(hex(p.state), hex(state), `${row.id}: the state blob round-trips`);
            assert.equal(hex(p.roster), hex(roster), `${row.id}: the roster blob round-trips`);
            assert.equal(p.logs, null, `${row.id}: a load writes no records`);
            assert.equal(p.status, STATUS_INT[row.status], `${row.id}: status`);
            assert.equal(p.needsBots, tsNeedsBots(game), `${row.id}: needs_bots`);
            const ts = await tsViews(game, row.status === GAME_STATUS.WAITING ? null : row.state, version);
            assertViews(row.id, { id: row.id, game }, p, ts);
        }
        assert.ok(dealt === 4 && lobbies === 4, `four dealt rows and four lobbies (${dealt}, ${lobbies})`);
    });

    test('the dealt fixture games, played on to their end through both pipelines', async () => {
        const { rows } = await pgPool.query(`SELECT id, name, status::text, state, version, good_players, good_timestamp
                                               FROM games WHERE status <> 'waiting' ORDER BY id`);
        let seed = 0xf1c;
        for (const row of rows) {
            const game = await loadCompleteGame(row.id);
            const players: Seat[] = game.players.map((p) => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai, strategy_key: p.strategy_key as string }));
            const ts: TsRow = {
                id: row.id, title: row.name, version: Number(row.version), status: row.status, state: hexToBytes(row.state),
                players, good_players: row.good_players ?? [], good_timestamp: row.good_timestamp ?? null, game,
            };
            await drive(row.id, ts, { state: ts.state, roster: rosterBytes(row.name, players) }, FIXTURE_STEPS, seed++);
        }
        assert.ok(counts.ended >= 3, `the three running fixture games reached their end (${counts.ended})`);
        assert.ok(counts.moot >= 4, `every finished game answered moot (${counts.moot})`);
    });

    test('fresh deals driven by the fuzz generator, legal and hostile moves alike', async () => {
        const mkSeat = (id: string, name: string, bot: string | null): PrivatePlayer => ({
            player_id: id, name, status: PLAYER_STATUS.READY, is_ai: bot !== null,
            hand: [], awaiting_attack: false, hand_length: 0, strategy_key: bot ?? STRATEGY_KEY.HUMAN,
        });
        const before = { ...counts };
        for (let k = 0; k < FUZZ_GAMES; k++) {
            const n = 2 + (k % 5);   // 2..6 seats, a bot in every other seat past the first two
            const players = Array.from({ length: n }, (_, i) =>
                mkSeat(uuid(), ['Дмитрий', 'Zoë 🃏', 'H2', '田中花子', 'B4', 'H5'][i], i >= 2 && i % 2 === 0 ? ['random', 'cordite', 'handwritten'][k % 3] : null));
            const game: Game = {
                id: `tp${k}`, name: `Parity ${k}`, version: 0, deck: [], deck_length: 0, discard_pile_length: 0, flipped: null,
                players, status: GAME_STATUS.WAITING, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [],
                elimination_order: [], good_timestamp: null, good_players: [], logs: [],
            };
            __setDealSeedOverride(Uint8Array.from({ length: 32 }, (_, i) => (i * 29 + k * 7 + 3) & 0xff));
            const run = start_game_packed(game);
            __setDealSeedOverride(null);
            const seats: Seat[] = players.map((p) => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai, strategy_key: p.strategy_key as string }));
            const ts: TsRow = {
                id: game.id, title: game.name, version: 1, status: game.status, state: run.stateBlob,
                players: seats, good_players: [], good_timestamp: null, game,
            };
            await drive(`fuzz ${k}`, ts, { state: run.stateBlob, roster: rosterBytes(game.name, seats) }, FUZZ_STEPS, 0x1234abcd + k);
        }
        const d = (key: keyof typeof counts) => counts[key] - before[key];
        assert.ok(d('applied') > 100 && d('rejected') > 20 && d('notSeated') > 5 && d('wire') > 5,
            `the fuzz reached every outcome class (${JSON.stringify(Object.fromEntries(Object.keys(counts).map((key) => [key, d(key as keyof typeof counts)])))})`);
        console.error(`[table_parity] ${JSON.stringify(counts)}`);
    });

    test('the action request and response codecs are the TS ones, byte for byte', () => {
        const wires = [Uint8Array.of(3, 0), Uint8Array.of(0, 1, 7), Uint8Array.of(1, 2, 8, 9, 10, 11)];
        for (const gid of ['a', 'd79ae3', 'x'.repeat(64)]) {
            for (const wire of wires) {
                for (const intent of [undefined, 0, 5, 0x7fffffff, 0xffffffff]) {
                    const body = encodeActionRequest(gid, wire, intent);
                    const c = table.requestDecode(body);
                    const t = decodeActionRequest(body)!;
                    assert.ok(typeof c !== 'number', `${gid}/${hex(wire)}/${intent}: decodes`);
                    assert.deepEqual({ gameId: c.gameId, wire: hex(c.wire), intent: c.intent },
                        { gameId: t.gameId, wire: hex(t.wire), intent: t.intentVersion ?? null }, `${gid}/${hex(wire)}/${intent}`);
                }
            }
        }
        for (const bad of [Uint8Array.of(), Uint8Array.of(1), Uint8Array.of(2, 1, 0x67, 1, 0, 0), Uint8Array.of(9, 0, 3, 0)]) {
            assert.equal(decodeActionRequest(bad), null, `TS refuses ${hex(bad)}`);
            assert.equal(typeof table.requestDecode(bad), 'number', `C refuses ${hex(bad)}`);
        }
        for (const version of [0, 1, 256, 0x01020304, 0x7fffffff]) {
            assert.equal(hex(table.actionResponse(TABLE_STALE_ROUND, 0, version)),
                hex(encodeActionResponse(ACTION_STATUS.REJECTED, REJECT_STALE_ROUND, version)), `stale round at ${version}`);
        }
    });

    test('elo_deltas is updateEloRatings\' arithmetic for every seat count and a sweep of ratings', () => {
        const r = fuzzRng(0xe10);
        let cases = 0;
        for (let n = 2; n <= 8; n++) {
            for (let k = 0; k < 400; k++) {
                const ratings = Array.from({ length: n }, () => (r.rnd() < 0.2 ? 1000 : r.ri(3000)));
                const order = Array.from({ length: n }, (_, i) => i).sort(() => r.rnd() - 0.5);
                // utils.ts updateEloRatings: for each place, the sum of the 1v1 changes against every other place.
                const want = new Array<number>(n).fill(0);
                for (let i = 0; i < n; i++) {
                    let total = 0;
                    for (let j = 0; j < n; j++) {
                        if (i === j) continue;
                        total += calculateEloChange(ratings[order[i]], ratings[order[j]], i < j ? 1 : 0);
                    }
                    want[order[i]] = total;
                }
                assert.deepEqual(table.eloDeltas(ratings, order), want.map((v) => v + 0), `n=${n} ratings=${ratings} order=${order}`);
                cases++;
            }
        }
        assert.equal(cases, 7 * 400);
    });
}
