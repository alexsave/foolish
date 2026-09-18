/* =============================================================================
 * The expand migration carries every live game into the kernel's two blobs
 * =============================================================================
 * docs/C_GAME_SHAPE_MIGRATION.md 3.4 and Phase 4a. Migration
 * 20260917140000_table_expand.sql gives every games row a durable roster
 * (c/src/roster.h) and, for a lobby, a durable lobby state blob, converted in
 * SQL from the JSONB columns (Q3). The SQL restates the C byte layouts once, so
 * this holds the SQL to the C kernel on the rows the hosted database holds:
 *
 *   e2e/fixtures/pre_table/schema.sql + rows.sql  (captured from today's server)
 *     -> the expand migration
 *     -> for every row: the C encoder writes the same roster bytes from the same
 *        JSONB; table_load accepts (state, roster); the seats, bot brains and
 *        needs_bots are the JSONB's; and table_envelope writes, for every human
 *        seat and the spectator, the envelope today's server cached in
 *        player_views / spectator_views.
 *
 * Then the pre-4b server's writes land on the expanded schema and the bridge
 * trigger keeps roster, needs_bots and the lobby blob in step with them; the
 * kernel writers (commit_table / create_table) take a row over, and a legacy
 * commit_game on it is refused. The pre-4b server is the one still deployed
 * while 4a is live, but Phase 4b deleted its code from this tree, so its writes
 * are the commit_game calls its lobby handlers made in this very suite,
 * recorded before the deletion (e2e/fixtures/pre_table/legacy_commits.jsonl, one
 * call per line with the test it belongs to) and replayed here as SQL, exactly as
 * PostgREST binds them.
 *
 * INTENTIONAL DIVERGENCES, each decided in the plan and asserted explicitly:
 *   Q1  The envelope's roster trailer. Today's server writes the good ids in the
 *       order they were said, then the good timestamp; C writes them in seat
 *       order with has_ts = 0. Every cached envelope is compared with C's bytes
 *       whose trailing goods block is rewritten into the legacy form, so every
 *       other byte must be C's exactly. (The captured game with a good said has
 *       one good and no timestamp, where the two forms coincide;
 *       e2e/table_parity.test.ts drives them apart against today's TS.)
 *   Q6  Status is the blob's. The WAITING row carrying a finished session's
 *       blob said GAME_OVER in its blob and WAITING in its column, and that
 *       board read as a lobby is refused (GAME_INVALID_LOBBY_CARDS). After the
 *       backfill the blob is a lobby and the two agree, asserted for every row.
 * ========================================================================== */

import './harness.ts';
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import type { PoolClient } from 'pg';
import { applyPlatformShim, pgPool } from './harness.ts';
import { createServerTable, TableProducts } from '../sdk/ts/table/server_table.ts';
import {
    GAME_INVALID_LOBBY_CARDS, GAME_STATUS_GAME_OVER, GAME_STATUS_PLAYING, GAME_STATUS_WAITING, TABLE_OK,
} from '../sdk/ts/gen/game_layout.bots.ts';
import { cRosterEncode } from './helpers/roster_kernel.ts';
import { commitTableSql, createTableSql } from './helpers/table_rpc.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const FIXTURE = join(process.cwd(), 'e2e', 'fixtures', 'pre_table');
const MIGRATION = join(process.cwd(), 'server', 'impls', 'supabase', 'migrations', '20260917140000_table_expand.sql');
const rowsSql = readFileSync(join(FIXTURE, 'rows.sql'), 'utf8');
const migrationSql = readFileSync(MIGRATION, 'utf8');

const table = createServerTable();
const enc = new TextEncoder();
const bytes = (hex: string) => Uint8Array.from(Buffer.from(hex.startsWith('\\x') ? hex.slice(2) : hex, 'hex'));
const hexOf = (b: Uint8Array) => Buffer.from(b).toString('hex');
const STATUS_INT: Record<string, number> = {
    waiting: GAME_STATUS_WAITING, playing: GAME_STATUS_PLAYING, game_over: GAME_STATUS_GAME_OVER,
};

// The scenario each captured game stands for, from the dump's header.
const scenarios = new Map<string, string>(); // key -> game id
for (const m of rowsSql.matchAll(/^--\s+(\w+): (\w+) - /gm)) scenarios.set(m[2], m[1]);
const idOf = (key: string): string => {
    const id = scenarios.get(key);
    assert.ok(id, `the fixture has a ${key} game`);
    return id;
};

// Captured users (rows.sql auth.users) and bots, by name.
const ALICE = 'dc3c32b8-9486-443b-943c-000000000000';
const DMITRY = 'bf8245d5-0270-4aca-8282-000000000001';
const CORDITE_BOT = 'dfdca6c5-d4e9-4a5a-94dc-000000000005';     // '%Кордит', cordite
const RANDOM_BOT = 'a9e7a7b0-8c30-4808-8ce7-000000000007';      // 'Random 🎲', random

// A synthetic WAITING row whose JSON still holds a round's goods, discard and
// trump: the kernel refuses all of them in a lobby, so the backfill must not
// carry them. A copy of the one-seat lobby otherwise.
const GOODS_LOBBY = 'wgood1';

interface JsonSeat { player_id: string; name: string; is_ai: boolean; status: string }
interface Row {
    id: string; name: string; status: string; players: JsonSeat[]; state: string | null; roster: string | null;
    needs_bots: boolean; writer_gen: number; version: string; updated_at: Date;
    good_players: string[]; good_timestamp: string | null;
}

const rowOf = async (id: string): Promise<Row> =>
    (await pgPool.query('SELECT *, status::text AS status FROM games WHERE id = $1', [id])).rows[0];
const allRows = async (): Promise<Row[]> =>
    (await pgPool.query('SELECT *, status::text AS status FROM games ORDER BY id')).rows;

let brains = new Map<string, string>();   // bot id -> strategy_key

// ---- the C side ----------------------------------------------------------------

function loadRow(r: Row): void {
    assert.ok(r.state, `${r.id}: has a state blob`);
    assert.ok(r.roster, `${r.id}: has a roster`);
    const rc = table.load(bytes(r.state!), bytes(r.roster!));
    assert.equal(rc, TABLE_OK, `${r.id}: table_load accepts the converted row (rc ${rc}, detail ${table.detail()})`);
}

/** The row's roster is the JSONB's, byte for byte as the C encoder writes it, and C reads it back as the JSONB. */
function assertRosterIsTheJsonb(r: Row, label: string): void {
    const seats = r.players.map((p) => ({ id: p.player_id, name: p.name, brain: p.is_ai ? brains.get(p.player_id)! : '' }));
    const c = cRosterEncode(r.name, seats);
    assert.ok(c instanceof Uint8Array, `${label}: the C encoder accepts the JSONB roster (${c})`);
    assert.equal(r.roster, `\\x${hexOf(c)}`, `${label}: the SQL roster is the C encoder's bytes`);
    loadRow(r);
    assert.deepEqual(table.seats(), seats, `${label}: C decodes the JSONB seats, names and brains`);
}

/** needs_bots: the column, the plan's JSONB predicate and the kernel agree. */
function assertNeedsBots(r: Row, label: string): void {
    const jsonb = r.status === 'playing' && r.players.some((p) => p.is_ai && p.status === 'in');
    assert.equal(r.needs_bots, jsonb, `${label}: needs_bots is the JSONB predicate`);
    loadRow(r);
    assert.equal(table.needsBots(), jsonb, `${label}: needs_bots is the kernel's table_needs_bots`);
}

function products(r: Row): TableProducts {
    loadRow(r);
    const p = table.commit(r.id, Number(r.version), 0);
    assert.ok(typeof p !== 'number', `${r.id}: table_commit_products (${p})`);
    return p as TableProducts;
}

/**
 * Q1. A C envelope ends with the trailer's goods block, `u8 n, n x {u16 len, id}
 * (seat order), u8 has_ts = 0`. Today's server wrote the ids in the order they
 * were said and, when set, `has_ts = 1, f64 timestamp`. Returns C's bytes with
 * that block rewritten into the legacy form.
 */
function withLegacyGoods(c: Uint8Array, r: Row): Uint8Array {
    const good = new Set(r.good_players);
    const seatOrder = r.players.map((p) => p.player_id).filter((id) => good.has(id));
    const block = (ids: string[], ts: number | null): Uint8Array => {
        const parts: number[] = [ids.length];
        for (const id of ids) {
            const b = enc.encode(id);
            parts.push(b.length & 0xff, b.length >> 8, ...b);
        }
        if (ts === null) return Uint8Array.from([...parts, 0]);
        const f = new Uint8Array(8);
        new DataView(f.buffer).setFloat64(0, ts, true);
        return Uint8Array.from([...parts, 1, ...f]);
    };
    const cBlock = block(seatOrder, null);
    assert.deepEqual(c.subarray(c.length - cBlock.length), cBlock, `${r.id}: C ends with seat-ordered goods and no timestamp`);
    const legacy = block(r.good_players, r.good_timestamp === null ? null : Number(r.good_timestamp));
    const out = new Uint8Array(c.length - cBlock.length + legacy.length);
    out.set(c.subarray(0, c.length - cBlock.length), 0);
    out.set(legacy, c.length - cBlock.length);
    return out;
}

/** Every cached envelope of the game is the one C writes from the converted row (Q1 as above). */
async function assertCachedEnvelopes(r: Row, label: string): Promise<number> {
    loadRow(r);
    let seats = 0;
    const compare = (viewer: number, cached: string, version: string, who: string) => {
        const c = table.envelope(r.id, viewer, Number(version));
        assert.ok(c instanceof Uint8Array, `${label}/${who}: table_envelope (${c})`);
        assert.equal(hexOf(withLegacyGoods(c as Uint8Array, r)), cached, `${label}/${who}: C writes the envelope today's server cached`);
    };
    const views = await pgPool.query('SELECT player_id::text, view, version FROM player_views WHERE game_id = $1', [r.id]);
    const humans = r.players.filter((p) => !p.is_ai).map((p) => p.player_id).sort();
    assert.deepEqual(views.rows.map((v) => v.player_id).sort(), humans, `${label}: one cached view per human seat`);
    for (const v of views.rows) {
        compare(r.players.findIndex((p) => p.player_id === v.player_id), v.view, v.version, v.player_id);
        seats++;
    }
    const spec = await pgPool.query('SELECT view, version FROM spectator_views WHERE game_id = $1', [r.id]);
    assert.equal(spec.rows.length, 1, `${label}: one spectator view`);
    compare(-1, spec.rows[0].view, spec.rows[0].version, 'spectator');
    return seats;
}

// ---- the database side --------------------------------------------------------

async function inRolledBackTx<T>(fn: (c: PoolClient) => Promise<T>): Promise<T> {
    const c = await pgPool.connect();
    try {
        await c.query('BEGIN');
        return await fn(c);
    } finally {
        await c.query('ROLLBACK').catch(() => {});
        c.release();
    }
}

// The pre-4b server's commit_game calls, in the order its handlers made them.
interface LegacyCall { name: string; params: Record<string, unknown>; test: string }
const LEGACY: LegacyCall[] = readFileSync(join(FIXTURE, 'legacy_commits.jsonl'), 'utf8')
    .split('\n').filter(Boolean).map((l) => JSON.parse(l));
const legacyCursor = new Map<string, number>();

/** Replays the next recorded commit_game call of `test`; resolves to its JSON result, rejects on a RAISE. */
async function legacyCommit(test: string, gameId: string): Promise<{ status: string }> {
    const calls = LEGACY.filter((c) => c.test === test);
    const i = legacyCursor.get(test) ?? 0;
    legacyCursor.set(test, i + 1);
    const call = calls[i];
    assert.ok(call, `${test}: a recorded legacy call #${i}`);
    assert.equal(call.params.p_game_id, gameId, `${test}: recorded call #${i} is for ${gameId}`);
    const keys = Object.keys(call.params);
    const r = await pgPool.query(`SELECT ${call.name}(${keys.map((k, j) => `${k} => $${j + 1}`).join(',')}) AS result`,
        keys.map((k) => { const v = call.params[k]; return v !== null && typeof v === 'object' ? JSON.stringify(v) : v; }));
    return r.rows[0].result;
}

/** After a legacy write: the bridge kept roster, needs_bots and the lobby blob in step with the JSONB. */
async function assertInStep(gameId: string, label: string): Promise<Row> {
    const r = await rowOf(gameId);
    const strict = (await pgPool.query('SELECT legacy_roster_hex($1::jsonb, $2) AS h', [JSON.stringify(r.players), r.name])).rows[0].h;
    assert.equal(r.roster, strict, `${label}: roster = legacy_roster_hex(players, name)`);
    assert.equal(r.writer_gen, 1, `${label}: still owned by the JSONB writers`);
    assertRosterIsTheJsonb(r, label);
    assertNeedsBots(r, label);
    const p = products(r);
    assert.equal(p.status, STATUS_INT[r.status], `${label}: the blob's status is the column's`);
    assert.equal(r.state, `\\x${hexOf(p.state)}`, `${label}: C writes back the stored state blob unchanged`);
    await assertCachedEnvelopes(r, label);
    return r;
}

if (!process.env.VALIDATION_ONLY) {
    let before4a: Map<string, Row>;

    before(async () => {
        await applyPlatformShim();
        await pgPool.query(readFileSync(join(FIXTURE, 'schema.sql'), 'utf8'));
        await pgPool.query(rowsSql);
        await pgPool.query(
            `INSERT INTO games (id, name, players, status, discard_pile_length, flipped, good_players, good_timestamp, state, version)
             SELECT $1, name, players, status, 7, '{"suit": 1, "value": 9}', jsonb_build_array(players->0->>'player_id'),
                    1726531200123, (SELECT state FROM games WHERE id = $3), 4
             FROM games WHERE id = $2`,
            [GOODS_LOBBY, idOf('lobby_1_seat'), idOf('waiting_stale_blob')],
        );
        brains = new Map((await pgPool.query('SELECT id::text, strategy_key FROM bots')).rows.map((b) => [b.id, b.strategy_key]));
    });

    test('the conversion refuses what the kernel would refuse, and the migration fails loudly on it', async () => {
        const lobby = idOf('lobby_3_seat_bots');
        const cases: Array<{ what: string; mutate: string; params: unknown[]; error: RegExp }> = [
            {
                what: 'a 65-byte name',
                mutate: `UPDATE games SET players = jsonb_set(players, '{0,name}', to_jsonb(repeat('é', 32) || 'x')) WHERE id = $1`,
                params: [lobby], error: /name is 65 bytes, over the kernel's cap of 64/,
            },
            {
                what: 'a 37-byte id',
                mutate: `UPDATE games SET players = jsonb_set(players, '{0,player_id}', to_jsonb(repeat('a', 37))) WHERE id = $1`,
                params: [lobby], error: /id is 37 bytes, over the kernel's cap of 36/,
            },
            {
                what: 'a 201-byte title',
                mutate: `UPDATE games SET name = repeat('я', 100) || '!' WHERE id = $1`,
                params: [lobby], error: /the title is 201 bytes, over the kernel's cap of 200/,
            },
            {
                what: 'nine seats',
                mutate: `UPDATE games SET players = (SELECT jsonb_agg(jsonb_set(p, '{player_id}', to_jsonb(p->>'player_id' || i)))
                           FROM jsonb_array_elements(players || players || players) WITH ORDINALITY AS t(p, i)) WHERE id = $1`,
                params: [lobby], error: /9 seats, over the kernel's 8/,
            },
            {
                what: 'a bot seat with no bots row',
                mutate: `UPDATE games SET players = jsonb_set(players, '{1,player_id}', '"00000000-0000-4000-8000-00000000dead"') WHERE id = $1`,
                params: [lobby], error: /bot seat 00000000-0000-4000-8000-00000000dead has no bots row/,
            },
            {
                what: 'a lobby seat with an unknown status',
                mutate: `UPDATE games SET players = jsonb_set(players, '{0,status}', '"napping"') WHERE id = $1`,
                params: [lobby], error: /has status napping/,
            },
        ];
        for (const k of cases) {
            await inRolledBackTx(async (c) => {
                await c.query(k.mutate, k.params);
                await assert.rejects(c.query(migrationSql), k.error, `${k.what} fails the migration`);
            });
        }

        // The caps are the kernel's exactly: at them the migration applies and C reads the row.
        await inRolledBackTx(async (c) => {
            const name64 = 'é'.repeat(32), title200 = 'я'.repeat(100);
            await c.query(`UPDATE games SET name = $2, players = jsonb_set(players, '{0,name}', to_jsonb($3::text)) WHERE id = $1`,
                [lobby, title200, name64]);
            await c.query(migrationSql);
            const r: Row = (await c.query('SELECT *, status::text AS status FROM games WHERE id = $1', [lobby])).rows[0];
            assert.equal(Buffer.byteLength(r.players[0].name), 64);
            assertRosterIsTheJsonb(r, 'a 64-byte name under a 200-byte title');
        });
    });

    test('the expand migration applies to the captured rows without touching version or updated_at', async () => {
        before4a = new Map((await allRows()).map((r) => [r.id, r]));
        assert.equal(before4a.size, scenarios.size + 1, 'every captured game and the synthetic goods lobby');
        await pgPool.query(migrationSql);
        const after = await allRows();
        assert.deepEqual(after.map((r) => r.id), [...before4a.keys()]);
        for (const r of after) {
            const was = before4a.get(r.id)!;
            assert.equal(r.version, was.version, `${r.id}: version unchanged`);
            assert.equal(r.updated_at.getTime(), was.updated_at.getTime(), `${r.id}: updated_at unchanged (the heartbeat scans it)`);
            assert.equal(r.writer_gen, 1, `${r.id}: owned by today's writers`);
            if (r.status !== 'waiting') assert.equal(r.state, was.state, `${r.id}: a dealt blob is untouched`);
        }
    });

    test('every converted row loads in C, with the JSONB seats, bot brains, title and needs_bots', async () => {
        const rows = await allRows();
        let bots = 0, dealtBots = 0;
        for (const r of rows) {
            assertRosterIsTheJsonb(r, r.id);
            assertNeedsBots(r, r.id);
            bots += r.players.filter((p) => p.is_ai).length;
            if (r.needs_bots) dealtBots++;
            const p = products(r);
            // Q6: the blob is authoritative for status, and on every converted row it agrees with the column.
            assert.equal(p.status, STATUS_INT[r.status], `${r.id}: the blob's status is the column's`);
            // The SQL lobby blob is the one C writes for that board, byte for byte (so is every dealt blob).
            assert.equal(r.state, `\\x${hexOf(p.state)}`, `${r.id}: C writes back the stored state blob unchanged`);
        }
        assert.ok(bots > 0 && dealtBots >= 2, 'the fixture exercises bot brains and needs_bots on dealt games');
    });

    test('C writes the envelope today\'s server cached for every human seat and the spectator', async () => {
        let seats = 0, games = 0, withGoods = 0;
        for (const r of await allRows()) {
            if (r.id === GOODS_LOBBY) continue;   // synthetic: no cached views
            seats += await assertCachedEnvelopes(r, r.id);
            games++;
            if (r.good_players.length > 0) withGoods++;
        }
        assert.equal(games, scenarios.size);
        assert.equal(seats, 12, 'every captured player view compared');
        // Q1 on the captured rows: the one game with a good said holds a single
        // good and no timestamp, so the said order is the seat order and the
        // cached bytes are C's exactly. The rewrite is still what they are held
        // to; e2e/table_parity.test.ts drives the orders apart against today's TS.
        assert.equal(withGoods, 1, 'the good-said game went through the Q1 comparison');
    });

    test('Q6: the WAITING row that carried a finished session\'s blob now loads as a clean lobby', async () => {
        const id = idOf('waiting_stale_blob');
        const was = before4a.get(id)!;
        const now = await rowOf(id);
        assert.equal(now.status, 'waiting');
        // Before: under the blob-authoritative rule this row was a FINISHED game
        // (the fool's cards and all) while its column said WAITING, and no
        // WAITING reading of that blob is one the kernel accepts.
        const stale = bytes(was.state!);
        assert.equal(table.load(stale, bytes(now.roster!)), TABLE_OK, 'the stale blob is a valid finished game');
        const finished = table.commit(id, Number(now.version), 0) as TableProducts;
        assert.equal(finished.status, GAME_STATUS_GAME_OVER, 'Q6: the blob said GAME_OVER where the column said WAITING');
        const asLobby = stale.slice();
        asLobby[2] = GAME_STATUS_WAITING;   // [format][deterministic deck][status ...]
        assert.equal(table.load(asLobby, bytes(now.roster!)), GAME_INVALID_LOBBY_CARDS,
            'the kernel refuses that board as a lobby');
        // After: a lobby of the same seats, which C and the column agree is WAITING.
        const p = products(now);
        assert.equal(p.status, GAME_STATUS_WAITING);
        assert.equal(p.needsBots, false);
        assert.equal(now.state, (await pgPool.query('SELECT legacy_lobby_state_hex($1::jsonb) AS h', [JSON.stringify(now.players)])).rows[0].h);

        // The synthetic lobby whose JSON still held goods, a timestamp, a discard
        // pile, a trump and a stale blob: none of it is carried.
        const goods = await rowOf(GOODS_LOBBY);
        loadRow(goods);
        assert.equal(goods.state, (await rowOf(idOf('lobby_1_seat'))).state, 'the same lobby blob as the same seats without them');
        const env = table.envelope(GOODS_LOBBY, -1, Number(goods.version));
        const plain = await rowOf(idOf('lobby_1_seat'));
        loadRow(plain);
        const plainEnv = table.envelope(GOODS_LOBBY, -1, Number(goods.version));
        assert.deepEqual(env, plainEnv, 'the spectator sees no goods in the lobby');
    });

    test('the pre-4b lobby handlers\' writes keep roster, needs_bots and the lobby blob in step on the expanded schema', async () => {
        const T = "today's lobby handlers keep roster, needs_bots and the lobby blob in step on the expanded schema";
        const id = idOf('lobby_1_seat');   // alice alone
        await legacyCommit(T, id);   // { type: 'join' }
        let r = await assertInStep(id, 'join');
        assert.deepEqual(r.players.map((p) => p.player_id), [ALICE, DMITRY]);

        await legacyCommit(T, id);   // { type: 'add-bot', bot_id: CORDITE_BOT }
        r = await assertInStep(id, 'add-bot');
        assert.equal(r.players.length, 3);

        await legacyCommit(T, id);   // { type: 'update-name', new_name: '  Новое имя 🃏 за столом  ' }
        r = await assertInStep(id, 'rename');
        assert.equal(r.name, 'Новое имя 🃏 за столом');

        await legacyCommit(T, id);   // { type: 'rearrange-players', new_order: [CORDITE_BOT, DMITRY, ALICE] }
        r = await assertInStep(id, 'reseat');
        assert.deepEqual(r.players.map((p) => p.player_id), [CORDITE_BOT, DMITRY, ALICE]);

        await legacyCommit(T, id);   // { type: 'exit', bot_id: CORDITE_BOT }
        r = await assertInStep(id, 'exit (a bot)');
        await legacyCommit(T, id);   // { type: 'exit', player_id: DMITRY }
        r = await assertInStep(id, 'exit (a player)');
        assert.deepEqual(r.players.map((p) => p.player_id), [ALICE]);

        // Through the deal: the dealt commit's blob is the kernel's, and a bot seat IN sets needs_bots.
        await legacyCommit(T, id);   // { type: 'start' }
        await legacyCommit(T, id);   // { type: 'add-bot', bot_id: RANDOM_BOT }
        r = await assertInStep(id, 'add-bot deals');
        assert.equal(r.status, 'playing');
        assert.equal(r.needs_bots, true);
    });

    test('the pre-4b server still runs a lobby whose JSON held a round\'s goods, trump and discard', async () => {
        // Today's server builds a lobby's views by marshalling the JSONB board
        // into the kernel, and the kernel refuses a lobby holding any of those
        // (GAME_INVALID_LOBBY_CARDS). So the backfill empties them in the JSONB
        // too, exactly as game_reset_to_lobby empties them in C.
        const r0 = await rowOf(GOODS_LOBBY);
        assert.deepEqual([r0.good_players, r0.good_timestamp], [[], null], 'no goods left in the JSONB lobby');
        await legacyCommit("today's server still runs a lobby whose JSON held a round's goods, trump and discard", GOODS_LOBBY);   // a join
        const r = await assertInStep(GOODS_LOBBY, 'join a lobby that held goods');
        assert.deepEqual(r.players.map((p) => p.player_id), [ALICE, DMITRY]);

        // And a legacy write that carries a round's board INTO the lobby (an old
        // continue that never cleared it) is emptied on the way in, not only by the backfill.
        await pgPool.query(
            `UPDATE games SET players = players, good_players = jsonb_build_array($2::text), good_timestamp = 1726531200123,
                              discard_pile_length = 7, flipped = '{"suit": 1, "value": 9}', power_suit = 1,
                              table_battles = '[{"attack": {"suit": 1, "value": 9}}]', elimination_order = jsonb_build_array($2::text)
             WHERE id = $1`, [GOODS_LOBBY, ALICE]);
        const emptied = await pgPool.query(
            `SELECT good_players, good_timestamp, discard_pile_length, flipped, power_suit, table_battles, elimination_order FROM games WHERE id = $1`,
            [GOODS_LOBBY]);
        assert.deepEqual(emptied.rows[0], {
            good_players: [], good_timestamp: null, discard_pile_length: 0, flipped: null, power_suit: 0, table_battles: [], elimination_order: [],
        }, 'the bridge empties a lobby\'s JSONB board on every legacy write');
        await assertInStep(GOODS_LOBBY, 'a legacy write carrying a round\'s board into the lobby');
    });

    test('a username over 64 bytes still joins during the bridge, cut as the kernel cuts it', async () => {
        const id = idOf('continued_lobby');
        const user = '00000000-0000-4000-8000-000000010000';
        const long = `${'🃏'.repeat(15)}ab`;   // 62 bytes, then a 4-byte scalar crosses 64
        const name = `${long}🂡`;
        assert.equal(Buffer.byteLength(name), 66);
        await pgPool.query(`INSERT INTO auth.users (id, raw_user_meta_data) VALUES ($1, $2)`, [user, JSON.stringify({ username: name })]);
        await legacyCommit('a username over 64 bytes still joins during the bridge, cut as the kernel cuts it', id);   // the join
        const r = await rowOf(id);
        assert.equal(r.players.at(-1)!.name, name, 'the JSONB keeps the whole name');
        loadRow(r);
        assert.equal(table.seats().at(-1)!.name, long, 'the roster holds the name cut on a scalar boundary');
        await assert.rejects(pgPool.query('SELECT legacy_roster_hex(players, name) FROM games WHERE id = $1', [id]),
            /name is 66 bytes/, 'the strict conversion refuses it');
        const views = await pgPool.query('SELECT player_id::text, view, version FROM player_views WHERE game_id = $1', [id]);
        for (const v of views.rows) {
            const seat = r.players.findIndex((p) => p.player_id === v.player_id);
            assert.equal(hexOf(table.envelope(id, seat, Number(v.version)) as Uint8Array), v.view,
                `${v.player_id}: the envelope today's server wrote (it trims names the same way)`);
        }
    });

    test('a hand-run legacy write to a dealt row keeps needs_bots the JSONB predicate: bots IN, humans only, finished', async () => {
        // The recorded handler calls only ever deal a game with a bot IN, so the
        // other two answers of the predicate come from the other writer the bridge
        // exists for: SQL run by hand against the JSONB roster.
        const expected = new Map([
            ['humans_2_mid_bout_good', false], ['mixed_4_after_pickup', true], ['bots_8_mid_game', true], ['finished', false],
        ]);
        for (const [key, needsBots] of expected) {
            const id = idOf(key);
            const { rowCount } = await pgPool.query('UPDATE games SET players = players WHERE id = $1 AND writer_gen = 1', [id]);
            assert.equal(rowCount, 1, `${key}: still a legacy-owned row`);
            const r = await assertInStep(id, `${key} after a hand-run write`);
            assert.equal(r.needs_bots, needsBots, `${key}: needs_bots`);
        }
    });

    test('the kernel writers take a row over, and a legacy commit_game on it is refused', async () => {
        // create_table: a lobby from table_create's products, owned by the kernel writers.
        const created = 'kt0001';
        assert.equal(table.create(ALICE, 'alice'), TABLE_OK);
        const c0 = table.commit(created, 0, 0) as TableProducts;
        await createTableSql(created, ALICE, c0);
        let r = await rowOf(created);
        assert.equal(r.writer_gen, 2);
        assert.equal(r.status, 'waiting');
        assert.equal(r.players.length, 0, 'the bridge derived nothing for a kernel-owned row');
        assert.equal(r.roster, `\\x${hexOf(c0.roster)}`, 'the base64 roster is stored as the column\'s \\x-hex text');
        loadRow(r);
        assert.deepEqual(table.seats(), [{ id: ALICE, name: 'alice', brain: '' }]);
        const createdViews = await pgPool.query('SELECT player_id::text, view, version, status FROM player_views WHERE game_id = $1', [created]);
        assert.deepEqual(createdViews.rows, [{ player_id: ALICE, view: hexOf(c0.views[0]!), version: '0', status: 'waiting' }],
            'the creator\'s envelope is stored as the bare hex the legacy writers store');

        // commit_table over a row today's writers own: the JSONB lobby, joined in C.
        const id = idOf('lobby_3_seat_bots');
        const legacy = await rowOf(id);
        loadRow(legacy);
        assert.equal(table.join(ALICE, 'alice'), TABLE_OK);
        const next = Number(legacy.version) + 1;
        const p = table.commit(id, next, 0) as TableProducts;
        const seats = table.seats();
        assert.deepEqual(await commitTableSql(id, next + 5, p, seats), { committed: false, new_version: null, new_round_epoch: null },
            'the version fence');
        assert.deepEqual(await commitTableSql(id, Number(legacy.version), p, seats),
            { committed: true, new_version: String(next), new_round_epoch: '0' });
        r = await rowOf(id);
        assert.equal(r.writer_gen, 2);
        assert.equal(r.roster, `\\x${hexOf(p.roster)}`);
        assert.equal(r.state, `\\x${hexOf(p.state)}`);
        assert.deepEqual(r.players, legacy.players, 'commit_table leaves the JSONB alone');
        const members = await pgPool.query('SELECT player_id::text FROM player_hands WHERE game_id = $1 ORDER BY 1', [id]);
        assert.deepEqual(members.rows.map((m) => m.player_id), [ALICE, DMITRY].sort());
        const views = await pgPool.query('SELECT player_id::text, view, status FROM player_views WHERE game_id = $1 ORDER BY 1', [id]);
        assert.deepEqual(views.rows, seats.flatMap((s, i) => (p.views[i] ? [{ player_id: s.id, view: hexOf(p.views[i]!), status: 'waiting' }] : []))
            .sort((a, b) => a.player_id.localeCompare(b.player_id)), 'one view row per human seat, the parallel arrays paired in order');

        // The next operation leaves the roster alone: a NULL roster keeps the stored bytes.
        loadRow(r);
        const unchanged = table.commit(id, next + 1, 0) as TableProducts;
        assert.equal(unchanged.rosterChanged, false);
        assert.equal((await commitTableSql(id, next, unchanged, seats)).committed, true);
        assert.equal((await rowOf(id)).roster, r.roster, 'a NULL roster keeps the stored one');

        // Mismatched view arrays are refused, nothing written.
        await assert.rejects(pgPool.query(
            `SELECT * FROM commit_table(p_game_id => $1, p_expected_version => $2, p_state => $3, p_status => 0::smallint, p_needs_bots => FALSE,
                                        p_view_players => $4::uuid[], p_views => $5::text[])`,
            [id, next + 1, Buffer.from(p.state).toString('base64'), [ALICE, DMITRY], [Buffer.from(p.views[0] ?? p.spectator).toString('base64')]]),
            /one envelope per player/);

        // The in-flight old request: the pre-4b handler reloaded the JSONB and committed. Refused, row untouched.
        const T = 'the kernel writers take a row over, and a legacy commit_game on it is refused';
        await assert.rejects(legacyCommit(T, id), /owned by the table writers \(writer_gen 2\)/);   // an update-name
        await assert.rejects(legacyCommit(T, id), /writer_gen 2/);   // a commit at the new version
        const after = await rowOf(id);
        assert.equal(after.version, String(next + 1));
        assert.equal(after.roster, r.roster);
        assert.equal(after.name, r.name);

        // A NULL blob is refused outright.
        await assert.rejects(pgPool.query(
            `SELECT * FROM commit_table(p_game_id => $1, p_expected_version => $2, p_state => NULL, p_status => 0::smallint, p_needs_bots => FALSE)`,
            [id, next + 1]), /state and needs_bots are required/);
    });
}
