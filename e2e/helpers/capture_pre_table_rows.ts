// Capture the pre-migration golden rows (docs/C_GAME_SHAPE_MIGRATION.md 0.4).
//
// Drives TODAY's real server code on a scratch e2e database and dumps what it
// wrote, so the migration has a record of the row shapes the hosted database
// holds before Phase 4 rewrites them:
//
//   e2e/fixtures/pre_table/schema.sql  a frozen copy of server/impls/supabase/seed.sql
//   e2e/fixtures/pre_table/rows.sql    INSERTs for the scenarios below
//
// The fixture is committed and never regenerated after Phase 4b. Until then a
// regeneration is:
//
//   TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/helpers/capture_pre_table_rows.ts
//
// HOW EACH ROW IS MADE. Everything a user can reach goes through the real edge
// entry points (e2e/helpers/edge.ts): `create`, `meta` and the packed `action`
// with a signed bearer token, and the bot loop those requests schedule through
// EdgeRuntime.waitUntil. The two rows the API cannot produce are made the way
// the e2e suites make them, and say so:
//   - the 8-seat bots-only game: no human can seat eight bots, so it is seeded
//     with harness seedGame, dealt with executeWithGameLock + start_game_packed
//     (as e2e/belief_logs_wiring.test.ts does) and played by lockedBotLoop;
//   - the WAITING row carrying a stale finished blob: a real finished game,
//     `continue`d through meta, then the finished blob written back with raw SQL
//     (as e2e/waiting_stale_blob.test.ts does).
//
// DETERMINISM. Two runs write byte-identical files (checked when this was
// written). What pins it:
//   - ids: harness uuid() for users, bots and the seeded game; createId's
//     crypto.randomUUID is replaced by a derived sequence, so `create` ids are
//     fixed too;
//   - deals: __setDealSeedOverride with a per-scenario seed, which also fixes
//     games.game_seed and every bot's RNG (seeded from it, as in production);
//   - human moves: a fixed LCG over the real legal-move enumeration;
//   - bot picks: every add-bot names its bot_id (no Math.random);
//   - the JS clock: Date.now is frozen and moves one second before every
//     request, so good_timestamp and the u48 log timestamps are the same every
//     run however many times a request reads the clock (the count is not stable:
//     fire-and-forget broadcasts interleave with the next request). Only the
//     bots-only segment runs on a clock that advances per call, which is what
//     makes the loop's CPU predictor end the segment mid-game; that segment has
//     no concurrent request, and its call count was stable across runs.
// The columns the database fills from its own clock or entropy cannot be pinned
// from here, so the dump rewrites them to fixed values (see NORMALIZED below):
//   *.created_at, *.updated_at, *.joined_at   -> '2026-09-17 00:00:00'
//   player_views.updated_at, spectator_views.updated_at -> '2026-09-17 00:00:00+00'
//   games.bot_lease_until                     -> '2026-09-17 00:00:00+00' when set
//   games.bot_lease_token                     -> a fixed uuid when set
// Everything else is exactly what the handlers wrote.

import '../harness.ts';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { applySchema, pgPool, seedGame, uuid } from '../harness.ts';
import { settle, tokenFor, postJson, postPacked } from './edge.ts';
import { mkLcg } from './rng.ts';
import { derivedUuid } from '../../sdk/ts/wire/detid.ts';
import { executeWithGameLock, loadCompleteGame } from '../../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { lockedBotLoop } from '../../server/impls/supabase/functions/_shared/adapter/bot_actions.ts';
import { packedProducts, start_game_packed } from '../../server/api/common/game_lifecycle.ts';
import { __setDealSeedOverride } from '../../sdk/ts/wasm/engine.ts';
import { ACTION_STATUS, AwireMove, decodeActionResponse, encodeAction, encodeActionRequest } from '../../sdk/ts/wire/awire.ts';
import { decodeLogs } from '../../sdk/ts/wire/logwire.ts';
import { hexToBytes } from '../../server/api/common/replay/codec.ts';
import { legalMovesFor } from '../dispatch.ts';
import { Game, GAME_STATUS } from '../../server/api/core/types.ts';

const say = (s: string) => process.stdout.write(`${s}\n`);
if (!process.env.E2E_VERBOSE) console.error = () => {};

const OUT_DIR = join(process.cwd(), 'e2e', 'fixtures', 'pre_table');

// ---- pinned entropy and clock -------------------------------------------------

let createSeq = 0;
(globalThis.crypto as { randomUUID: () => string }).randomUUID =
    () => derivedUuid('pre_table:createId', createSeq++) as ReturnType<Crypto['randomUUID']>;

const CLOCK_BASE = 1_758_067_200_000; // 2025-09-17T00:00:00Z
let clockNow = CLOCK_BASE;
let clockStep = 0; // ms per Date.now call; 0 = frozen between requests
Date.now = () => (clockNow += clockStep);
const tick = () => { clockNow += 1000; };

const dealSeed = (n: number) => Uint8Array.from({ length: 32 }, (_, i) => (i * 29 + n * 101 + 7) & 0xff);

// ---- cast ---------------------------------------------------------------------

interface Human { id: string; name: string; tok: string }
interface Bot { id: string; nickname: string; strategy: string }

// Usernames exercise every UTF-8 width the roster must carry: ASCII, 2-byte
// (Cyrillic, Latin-1), 3-byte (CJK) and 4-byte (emoji), up to the 16-character
// cap the web enforces.
const HUMAN_NAMES = ['alice', 'Дмитрий', '田中花子', 'Zoë 🃏', 'José Ñúñez-Peña'];

async function makeHumans(): Promise<Human[]> {
    const out: Human[] = [];
    for (const name of HUMAN_NAMES) {
        const id = uuid();
        await pgPool.query('INSERT INTO auth.users(id, raw_user_meta_data) VALUES ($1, $2)', [id, JSON.stringify({ username: name })]);
        out.push({ id, name, tok: await tokenFor(id, name) });
    }
    return out;
}

async function makeBot(nickname: string, strategy: string): Promise<Bot> {
    const id = uuid();
    await pgPool.query('INSERT INTO bots(id, nickname, strategy_key) VALUES ($1, $2, $3)', [id, nickname, strategy]);
    return { id, nickname, strategy };
}

// ---- driving the real endpoints --------------------------------------------------

async function ok(what: string, res: { status: number; json: any; bytes: Uint8Array }): Promise<void> {
    if (res.status !== 200) throw new Error(`${what}: HTTP ${res.status} ${JSON.stringify(res.json)}`);
    await settle();
}

async function create(h: Human): Promise<string> {
    const before = createSeq;
    tick();
    const res = await postJson('create', h.tok, {});
    await ok(`create by ${h.name}`, res);
    if (createSeq !== before + 1) throw new Error('create did not draw exactly one id');
    return derivedUuid('pre_table:createId', before).slice(0, 6);
}

const meta = async (h: Human, body: Record<string, unknown>) => {
    tick();
    await ok(`meta ${body.type} by ${h.name}`, await postJson('meta', h.tok, body));
};

async function row(gameId: string) {
    return (await pgPool.query('SELECT status, state, version, good_players, table_battles, logs_packed, players FROM games WHERE id=$1', [gameId])).rows[0];
}

// One human move chosen by `rng` from the real legal-move enumeration, sent as
// the packed request a client sends (v2 envelope, intent = the loaded version).
// Returns false when no human has a move (the bots are to act, or the game is over).
async function humanMove(gameId: string, humans: Human[], rng: () => number, prefer?: (type: string) => boolean): Promise<boolean> {
    const g: Game = await loadCompleteGame(gameId);
    if (g.status !== GAME_STATUS.PLAYING) return false;
    const seated = new Map(humans.map((h) => [h.id, h]));
    let moves = legalMovesFor(g, (pid) => seated.has(pid));
    if (moves.length === 0) return false;
    const preferred = prefer ? moves.filter((m) => prefer(m.move.type)) : [];
    if (preferred.length > 0) moves = preferred;
    const pm = moves[Math.floor(rng() * moves.length)];
    const wire = encodeAction({ kind: pm.move.type, cards: pm.move.cards, attack_cards: pm.move.attack_cards } as AwireMove);
    tick();
    const res = await postPacked(seated.get(pm.playerId)!.tok, encodeActionRequest(gameId, wire, g.version ?? 0));
    await ok(`action ${pm.move.type}`, res);
    const out = decodeActionResponse(res.bytes);
    if (!out) throw new Error('undecodable action response');
    // A menu entry the human narrowing refuses (a good over an uncovered attack)
    // comes back REJECTED and changes nothing; the caller simply picks again.
    return out.status === ACTION_STATUS.APPLIED || out.status === ACTION_STATUS.REJECTED;
}

async function playUntil(gameId: string, humans: Human[], rng: () => number, done: (r: any) => boolean | Promise<boolean>,
    prefer?: (type: string) => boolean): Promise<void> {
    for (let step = 0; step < 3000; step++) {
        const r = await row(gameId);
        if (await done(r)) return;
        if (r.status !== 'playing') break;
        if (!(await humanMove(gameId, humans, rng, prefer))) {
            // Nobody human can move: nudge the bot loop the way a client does.
            tick();
            await ok('action bump', await postJson('action', humans[0].tok, { type: 'bump', game_id: gameId }));
            const after = await row(gameId);
            if (after.version === r.version) throw new Error(`${gameId}: stalled at version ${r.version}`);
        }
    }
    const r = await row(gameId);
    if (!(await done(r))) throw new Error(`${gameId}: condition not reached (status ${r.status}, version ${r.version})`);
}

const loggedPickup = (gameId: string, r: any) =>
    r.logs_packed && decodeLogs(hexToBytes(r.logs_packed), gameId, r.players).some((l) => l.log_type === 'pickup');

// ---- scenarios -----------------------------------------------------------------------

interface Scenario { key: string; gameId: string; note: string }

async function capture(): Promise<Scenario[]> {
    const [alice, dmitry, tanaka, zoe, jose] = await makeHumans();
    const kordit = await makeBot('%Кордит', 'cordite');
    const handy = await makeBot('Handwritten 1', 'handwritten');
    const rando = await makeBot('Random 🎲', 'random');
    const simple = await makeBot('Simple Heuristic 1', 'simple_heuristic');
    const s: Scenario[] = [];

    // 1. A one-seat lobby, exactly as `create` leaves it.
    {
        const id = await create(alice);
        s.push({ key: 'lobby_1_seat', gameId: id, note: 'create only' });
    }

    // 2. A three-seat lobby with two bots, titled at the 50-character cap in
    //    3-byte characters (150 bytes, the widest title the cap admits).
    {
        const id = await create(dmitry);
        await meta(dmitry, { type: 'add-bot', game_id: id, bot_id: kordit.id });
        await meta(dmitry, { type: 'add-bot', game_id: id, bot_id: handy.id });
        await meta(dmitry, { type: 'update-name', game_id: id, new_name: '日本語のゲーム名'.repeat(7).slice(0, 50) });
        s.push({ key: 'lobby_3_seat_bots', gameId: id, note: 'create + 2 add-bot + update-name (50 CJK chars)' });
    }

    // 3. Two humans mid-bout with a good said, titled at the cap in ASCII.
    {
        const id = await create(tanaka);
        await meta(tanaka, { type: 'update-name', game_id: id, new_name: 'A title that is exactly fifty characters long, ok.' });
        await meta(zoe, { type: 'join', game_id: id });
        __setDealSeedOverride(dealSeed(3));
        await meta(tanaka, { type: 'start', game_id: id });
        await meta(zoe, { type: 'start', game_id: id });
        __setDealSeedOverride(null);
        await playUntil(id, [tanaka, zoe], mkLcg(3), (r) =>
            r.status === 'playing' && r.good_players.length > 0 && r.table_battles.length > 0);
        s.push({ key: 'humans_2_mid_bout_good', gameId: id, note: 'create + join + 2 start, packed moves until a good is said with a battle on the table' });
    }

    // 4. Four seats, two humans and two bots, after a pickup.
    {
        const id = await create(jose);
        await meta(alice, { type: 'join', game_id: id });
        await meta(jose, { type: 'add-bot', game_id: id, bot_id: simple.id });
        await meta(alice, { type: 'add-bot', game_id: id, bot_id: rando.id });
        __setDealSeedOverride(dealSeed(4));
        await meta(jose, { type: 'start', game_id: id });
        await meta(alice, { type: 'start', game_id: id });
        __setDealSeedOverride(null);
        await playUntil(id, [jose, alice], mkLcg(4), (r) => r.status === 'playing' && loggedPickup(id, r),
            (type) => type === 'pickup');
        s.push({ key: 'mixed_4_after_pickup', gameId: id, note: 'create + join + 2 add-bot + 2 start, packed moves (humans prefer pickup) and the bot loop, until the session log holds a pickup' });
    }

    // 5. Eight bots, mid-game. Not reachable through the API (see the header).
    {
        const id = uuid().slice(0, 6);
        const strategies = ['cordite', 'handwritten', 'random', 'simple_heuristic', 'firecracker', 'blackpowder', 'handwritten', 'random'];
        const bots: Bot[] = [];
        for (let i = 0; i < strategies.length; i++) bots.push(await makeBot(`Bot ${i + 1} ${['♠', '♥', '♣', '♦'][i % 4]}`, strategies[i]));
        await seedGame(id, bots.map((b) => ({ id: b.id, name: b.nickname, is_ai: true, strategy_key: b.strategy })));
        __setDealSeedOverride(dealSeed(5));
        tick();
        await executeWithGameLock(id, async (g: Game) => ({ game: g, events: [], packed: packedProducts(start_game_packed(g)) }), 'start', false);
        __setDealSeedOverride(null);
        // A large clock step makes the loop's CPU predictor bail after a fixed
        // number of drives, leaving the game mid-play, the way a heartbeat
        // segment ends in production.
        tick();
        clockStep = 40;
        await lockedBotLoop(id);
        await settle();
        clockStep = 0;
        const r = await row(id);
        if (r.status !== 'playing') throw new Error(`bots-only game is ${r.status}, expected playing`);
        s.push({ key: 'bots_8_mid_game', gameId: id, note: 'seedGame + start_game_packed + one lockedBotLoop segment' });
    }

    // 6-8. Finished games: one kept finished, one continued, one continued and
    //      then damaged with its finished blob.
    const finished = async (n: number, a: Human, b: Human, bot: Bot | null): Promise<string> => {
        const id = await create(a);
        await meta(b, { type: 'join', game_id: id });
        if (bot) await meta(a, { type: 'add-bot', game_id: id, bot_id: bot.id });
        __setDealSeedOverride(dealSeed(n));
        await meta(a, { type: 'start', game_id: id });
        await meta(b, { type: 'start', game_id: id });
        __setDealSeedOverride(null);
        await playUntil(id, [a, b], mkLcg(n), (r) => r.status === 'game_over');
        return id;
    };
    {
        const id = await finished(6, alice, dmitry, handy);
        s.push({ key: 'finished', gameId: id, note: 'create + join + add-bot + 2 start, played to game_over' });
    }
    {
        const id = await finished(7, zoe, jose, null);
        await meta(zoe, { type: 'continue', game_id: id });
        s.push({ key: 'continued_lobby', gameId: id, note: 'played to game_over, then meta continue' });
    }
    {
        const id = await finished(8, tanaka, alice, kordit);
        const { state } = await row(id);
        if (!state) throw new Error('the finished game carries no blob');
        await meta(tanaka, { type: 'continue', game_id: id });
        await pgPool.query('UPDATE games SET state=$1 WHERE id=$2', [state, id]);
        s.push({ key: 'waiting_stale_blob', gameId: id, note: 'played to game_over, meta continue, then the finished state blob written back with raw SQL' });
    }
    return s;
}

// ---- dump ---------------------------------------------------------------------------

const FIXED_TS = "'2026-09-17 00:00:00'";
const FIXED_TSTZ = "'2026-09-17 00:00:00+00'";
const FIXED_LEASE = "'00000000-0000-4000-8000-00000000cafe'";
const NORMALIZED: Record<string, (v: string) => string> = {
    created_at: () => FIXED_TS,
    joined_at: () => FIXED_TS,
    updated_at: () => FIXED_TS,
    'player_views.updated_at': () => FIXED_TSTZ,
    'spectator_views.updated_at': () => FIXED_TSTZ,
    'games.bot_lease_until': () => FIXED_TSTZ,
    'games.bot_lease_token': () => FIXED_LEASE,
};

const ident = (s: string) => `"${s.replace(/"/g, '""')}"`;

async function dumpTable(schema: string, table: string, where: string, order: string, params: unknown[]): Promise<string[]> {
    const cols: string[] = (await pgPool.query(
        `SELECT column_name FROM information_schema.columns WHERE table_schema=$1 AND table_name=$2 ORDER BY ordinal_position`,
        [schema, table])).rows.map((r) => r.column_name);
    const select = cols.map((c) => `quote_nullable(${ident(c)}::text) AS ${ident(c)}`).join(', ');
    const rows = (await pgPool.query(`SELECT ${select} FROM ${schema}.${table} WHERE ${where} ORDER BY ${order}`, params)).rows;
    return rows.map((r) => {
        const values = cols.map((c) => {
            const v: string = r[c];
            if (v === 'NULL') return v;
            const norm = NORMALIZED[`${table}.${c}`] ?? NORMALIZED[c];
            return norm ? norm(v) : v;
        });
        return `INSERT INTO ${schema}.${table} (${cols.join(', ')}) VALUES (${values.join(', ')});`;
    });
}

async function dump(scenarios: Scenario[], commit: string): Promise<string> {
    const ids = scenarios.map((x) => x.gameId);
    const humans = `id IN (SELECT player_id FROM public.player_hands WHERE game_id = ANY($1)
                         UNION SELECT (p->>'player_id')::uuid FROM public.games, jsonb_array_elements(players) p
                               WHERE id = ANY($1) AND NOT (p->>'is_ai')::boolean)`;
    const bots = `id IN (SELECT bot_id FROM public.bot_hands WHERE game_id = ANY($1)
                       UNION SELECT (p->>'player_id')::uuid FROM public.games, jsonb_array_elements(players) p
                             WHERE id = ANY($1) AND (p->>'is_ai')::boolean)`;
    const lines = [
        '-- Pre-migration golden rows (docs/C_GAME_SHAPE_MIGRATION.md 0.4).',
        '-- GENERATED by e2e/helpers/capture_pre_table_rows.ts; never edit by hand, never regenerate after Phase 4b.',
        `-- Captured from the server code at ${commit}. Load after e2e/schema.sql and e2e/fixtures/pre_table/schema.sql.`,
        '--',
        '-- Scenarios (game id: key - how it was made):',
        ...scenarios.map((x) => `--   ${x.gameId}: ${x.key} - ${x.note}`),
        '--',
        '-- Normalized by the dump (the database fills them from its own clock or entropy):',
        `--   created_at, updated_at, joined_at -> ${FIXED_TS} (timestamptz columns ${FIXED_TSTZ})`,
        `--   games.bot_lease_until -> ${FIXED_TSTZ}, games.bot_lease_token -> ${FIXED_LEASE} (when set)`,
        '-- Every other column is what the handlers wrote.',
        '',
        '-- auth.users: the FK targets of player_hands / player_views (platform rows, not app data).',
        ...await dumpTable('auth', 'users', humans, 'id', [ids]),
        '',
        ...await dumpTable('public', 'bots', bots, 'id', [ids]),
        '',
        ...await dumpTable('public', 'games', 'id = ANY($1)', 'id', [ids]),
        '',
        ...await dumpTable('public', 'player_hands', 'game_id = ANY($1)', 'game_id, player_id', [ids]),
        '',
        ...await dumpTable('public', 'bot_hands', 'game_id = ANY($1)', 'game_id, bot_id', [ids]),
        '',
        ...await dumpTable('public', 'player_views', 'game_id = ANY($1)', 'game_id, player_id', [ids]),
        '',
        ...await dumpTable('public', 'spectator_views', 'game_id = ANY($1)', 'game_id', [ids]),
    ];
    return `${lines.join('\n')}\n`;
}

async function main(): Promise<void> {
    await applySchema();
    const scenarios = await capture();
    const commit = process.env.CAPTURE_COMMIT ?? 'the working tree';
    mkdirSync(OUT_DIR, { recursive: true });
    writeFileSync(join(OUT_DIR, 'schema.sql'), readFileSync(join(process.cwd(), 'server', 'impls', 'supabase', 'seed.sql')));
    const sql = await dump(scenarios, commit);
    writeFileSync(join(OUT_DIR, 'rows.sql'), sql);
    for (const x of scenarios) {
        const r = await row(x.gameId);
        say(`${x.key.padEnd(24)} ${x.gameId}  status=${r.status} version=${r.version} blob=${r.state ? 'yes' : 'no'}`);
    }
    say(`wrote ${join(OUT_DIR, 'rows.sql')} (${sql.split('\n').filter((l) => l.startsWith('INSERT')).length} INSERTs)`);
}

main().catch((e) => { process.stderr.write(`${e?.stack ?? e}\n`); process.exit(1); });
