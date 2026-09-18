/* =============================================================================
 * The BYTEA migration stores every blob as the bytes it was, and the writers
 * keep working on bytes
 * =============================================================================
 * docs/C_GAME_SHAPE_MIGRATION.md 3.1, final pass. On the rows the hosted
 * database really held:
 *
 *   e2e/fixtures/pre_table/schema.sql + rows.sql   (captured from the pre-4a server)
 *     -> 20260917140000_table_expand.sql            (4a)
 *     -> kernel writes as the functions make them   (a join, a dealt commit that
 *                                                    leaves the roster alone, a create)
 *     -> 20260918210000_table_contract.sql          (4c)
 *     -> legacy snapshots with JSONB seat lists, and a blob of each column
 *        rewritten in its other text form ('\x'-hex where bare hex was
 *        written, bare where '\x' was), which a hand-run write could leave
 *     -> 20260918220000_table_bytea.sql
 *
 * and holds: every state, roster, session log and cached view is byte for byte
 * the bytes its text spelled, whichever form; version, status, needs_bots,
 * round_epoch, the deal seed and updated_at are untouched; table_load accepts
 * every row and C writes the envelopes it wrote before, which are the cached
 * views; the snapshot seat lists keep their order as UUID[] and gate reads
 * exactly as the JSONB policy did; commit_table and create_table write bytes
 * (a NULL roster keeps the stored one); and the migration refuses, naming the
 * row, a blob that is not whole hex bytes or a seat list that is not UUIDs.
 * ========================================================================== */

import './harness.ts';
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import type { PoolClient } from 'pg';
import { applyPlatformShim, pgPool } from './harness.ts';
import { createServerTable, type TableProducts } from '../sdk/ts/table/server_table.ts';
import { TABLE_OK } from '../sdk/ts/gen/game_layout.bots.ts';
import { commitTableSql, createTableSql } from './helpers/table_rpc.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const FIXTURE = join(process.cwd(), 'e2e', 'fixtures', 'pre_table');
const MIGRATIONS = join(process.cwd(), 'server', 'impls', 'supabase', 'migrations');
const sqlOf = (name: string) => readFileSync(join(MIGRATIONS, name), 'utf8');
const BYTEA = '20260918220000_table_bytea.sql';

const table = createServerTable();
/** The bytes a hex text spells, '\x'-prefixed (how a BYTEA column reads) or bare. */
const bytes = (hex: string) => Uint8Array.from(Buffer.from(hex.startsWith('\\x') ? hex.slice(2) : hex, 'hex'));
const hexOf = (b: Uint8Array) => Buffer.from(b).toString('hex');

const rowsSql = readFileSync(join(FIXTURE, 'rows.sql'), 'utf8');
const scenarios = new Map<string, string>();
for (const m of rowsSql.matchAll(/^--\s+(\w+): (\w+) - /gm)) scenarios.set(m[2], m[1]);
const idOf = (key: string): string => {
    const id = scenarios.get(key);
    assert.ok(id, `the fixture has a ${key} game`);
    return id;
};

const ALICE = 'dc3c32b8-9486-443b-943c-000000000000';
const DMITRY = 'bf8245d5-0270-4aca-8282-000000000001';
const STRANGER = '00000000-0000-4000-8000-00000000057a';

interface Blobs {
    games: Record<string, { state: string; roster: string; logs: string | null; scalars: unknown }>;
    views: Record<string, string>;        // `${game}/${player}` -> hex of the bytes
    spectators: Record<string, string>;   // game -> hex of the bytes
}

/** Every blob, as the hex of the bytes its column's text spells, and the scalars beside them. */
async function blobs(): Promise<Blobs> {
    const out: Blobs = { games: {}, views: {}, spectators: {} };
    for (const r of (await pgPool.query(
        `SELECT id, state, roster, logs_packed, status::text AS status, needs_bots, version, round_epoch, game_seed, updated_at
         FROM games ORDER BY id`)).rows) {
        out.games[r.id] = {
            state: hexOf(bytes(r.state)), roster: hexOf(bytes(r.roster)),
            logs: r.logs_packed === null ? null : hexOf(bytes(r.logs_packed)),
            scalars: { status: r.status, needs_bots: r.needs_bots, version: r.version, round_epoch: r.round_epoch, game_seed: r.game_seed, updated_at: r.updated_at },
        };
    }
    for (const r of (await pgPool.query('SELECT game_id, player_id::text, view FROM player_views ORDER BY 1, 2')).rows) {
        out.views[`${r.game_id}/${r.player_id}`] = hexOf(bytes(r.view));
    }
    for (const r of (await pgPool.query('SELECT game_id, view FROM spectator_views ORDER BY 1')).rows) {
        out.spectators[r.game_id] = hexOf(bytes(r.view));
    }
    return out;
}

/** Every envelope C writes for a stored row (each human seat by id, and the spectator), as hex. */
async function envelopesOf(gameId: string): Promise<Map<string, string>> {
    const r = (await pgPool.query('SELECT state, roster, version FROM games WHERE id = $1', [gameId])).rows[0];
    const rc = table.load(bytes(r.state), bytes(r.roster));
    assert.equal(rc, TABLE_OK, `${gameId}: table_load accepts the row (rc ${rc}, detail ${table.detail()})`);
    const out = new Map<string, string>();
    table.seats().forEach((s, seat) => {
        if (s.brain) return;
        out.set(s.id, hexOf(table.envelope(gameId, seat, Number(r.version)) as Uint8Array));
    });
    out.set('spectator', hexOf(table.envelope(gameId, -1, Number(r.version)) as Uint8Array));
    return out;
}

async function cachedViews(gameId: string): Promise<Map<string, string>> {
    const out = new Map<string, string>();
    for (const v of (await pgPool.query('SELECT player_id::text, view FROM player_views WHERE game_id = $1', [gameId])).rows) out.set(v.player_id, hexOf(bytes(v.view)));
    for (const v of (await pgPool.query('SELECT view FROM spectator_views WHERE game_id = $1', [gameId])).rows) out.set('spectator', hexOf(bytes(v.view)));
    return out;
}

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

/** Reads game_snapshots as a signed-in user under RLS, the way PostgREST runs a client's select. */
async function snapshotIdsAs(user: string): Promise<string[]> {
    return inRolledBackTx(async (c) => {
        await c.query('SET LOCAL ROLE authenticated');
        await c.query(`SELECT set_config('request.jwt.claims', $1, true)`, [JSON.stringify({ role: 'authenticated', sub: user })]);
        return (await c.query('SELECT id::text FROM game_snapshots ORDER BY id')).rows.map((r) => r.id);
    });
}

const SNAP_BOTH = '00000000-0000-4000-8000-0000000005a1';
const SNAP_ALICE = '00000000-0000-4000-8000-0000000005a2';

if (!process.env.VALIDATION_ONLY) {
    let beforeBlobs: Blobs;
    const envelopesBefore = new Map<string, Map<string, string>>();
    const dealt = idOf('mixed_4_after_pickup');
    const joined = idOf('lobby_3_seat_bots');

    before(async () => {
        await applyPlatformShim();
        await pgPool.query(readFileSync(join(FIXTURE, 'schema.sql'), 'utf8'));
        await pgPool.query(rowsSql);
        await pgPool.query(sqlOf('20260917140000_table_expand.sql'));

        // The functions' writes between 4a and 4c: a join, a dealt commit that leaves the roster alone, a create.
        let r = (await pgPool.query('SELECT state, roster, version FROM games WHERE id = $1', [joined])).rows[0];
        assert.equal(table.load(bytes(r.state), bytes(r.roster)), TABLE_OK);
        assert.equal(table.join(ALICE, 'alice'), TABLE_OK);
        let p = table.commit(joined, Number(r.version) + 1, 0) as TableProducts;
        assert.equal((await commitTableSql(joined, Number(r.version), p, table.seats())).committed, true);
        r = (await pgPool.query('SELECT state, roster, version FROM games WHERE id = $1', [dealt])).rows[0];
        assert.equal(table.load(bytes(r.state), bytes(r.roster)), TABLE_OK);
        p = table.commit(dealt, Number(r.version) + 1, 0) as TableProducts;
        assert.equal((await commitTableSql(dealt, Number(r.version), p, table.seats())).committed, true);
        assert.equal(table.create(DMITRY, 'Dmitry'), TABLE_OK);
        await createTableSql('kb0001', DMITRY, table.commit('kb0001', 0, 0) as TableProducts);

        await pgPool.query(sqlOf('20260918210000_table_contract.sql'));

        // Snapshots as the functions before this migration wrote them: seat lists as JSON arrays.
        await pgPool.query(`INSERT INTO auth.users (id) VALUES ($1) ON CONFLICT DO NOTHING`, [STRANGER]);
        await pgPool.query(
            `INSERT INTO game_snapshots (id, game_id, player_ids, moves, extras) VALUES
               ($1, $3, $4::jsonb, '\\x0102', NULL), ($2, NULL, $5::jsonb, '\\x03', '\\x04')`,
            [SNAP_BOTH, SNAP_ALICE, dealt, JSON.stringify([DMITRY, ALICE]), JSON.stringify([ALICE])]);

        // The other text form of each blob column, as a hand-run write could leave it.
        await pgPool.query(`UPDATE games SET state = substr(state, 3) WHERE id = $1`, [idOf('lobby_1_seat')]);
        await pgPool.query(`UPDATE games SET logs_packed = '\\x' || logs_packed WHERE id = $1`, [dealt]);
        await pgPool.query(`UPDATE player_views SET view = '\\x' || view WHERE game_id = $1 AND player_id = $2`, [joined, ALICE]);
        await pgPool.query(`UPDATE spectator_views SET view = '\\x' || view WHERE game_id = $1`, [idOf('finished')]);

        // Grants a Supabase database gives client roles, for the RLS reads below.
        await pgPool.query(`
            GRANT SELECT ON game_snapshots TO authenticated;
            CREATE OR REPLACE FUNCTION auth.uid() RETURNS uuid LANGUAGE sql STABLE AS $$
              SELECT (nullif(current_setting('request.jwt.claims', true), '')::jsonb ->> 'sub')::uuid $$;
            CREATE OR REPLACE FUNCTION auth.role() RETURNS text LANGUAGE sql STABLE AS $$
              SELECT coalesce(nullif(current_setting('request.jwt.claims', true), '')::jsonb ->> 'role', 'service_role') $$;`);
    });

    test('the fixture exercises both text forms in every blob column, and logs and views that are not empty', async () => {
        const forms = async (sql: string) => new Set((await pgPool.query(sql)).rows.map((r) => r.f));
        const q = (col: string, from: string) => `SELECT DISTINCT CASE WHEN left(${col}, 2) = '\\x' THEN 'prefixed' ELSE 'bare' END AS f FROM ${from} WHERE ${col} <> ''`;
        assert.deepEqual(await forms(q('state', 'games')), new Set(['prefixed', 'bare']), 'games.state');
        assert.deepEqual(await forms(q('logs_packed', 'games')), new Set(['prefixed', 'bare']), 'games.logs_packed');
        assert.deepEqual(await forms(q('view', 'player_views')), new Set(['prefixed', 'bare']), 'player_views.view');
        assert.deepEqual(await forms(q('view', 'spectator_views')), new Set(['prefixed', 'bare']), 'spectator_views.view');
        assert.deepEqual(await forms(q('roster', 'games')), new Set(['prefixed']), 'games.roster (every writer spells it \\x)');
        beforeBlobs = await blobs();
        for (const id of Object.keys(beforeBlobs.games)) envelopesBefore.set(id, await envelopesOf(id));
        assert.ok(Object.values(beforeBlobs.games).filter((g) => g.logs && g.logs.length > 0).length >= 3, 'dealt rows carry session logs');
        assert.deepEqual(await snapshotIdsAs(ALICE), [SNAP_BOTH, SNAP_ALICE], 'before: alice reads both snapshots she sat in');
        assert.deepEqual(await snapshotIdsAs(DMITRY), [SNAP_BOTH]);
        assert.deepEqual(await snapshotIdsAs(STRANGER), []);
    });

    test('it refuses, naming them, a blob that is not whole hex bytes and a seat list that is not UUIDs', async () => {
        await inRolledBackTx(async (c) => {
            await c.query(`UPDATE player_views SET view = view || 'z' WHERE game_id = $1 AND player_id = $2`, [joined, ALICE]);
            await c.query(`UPDATE games SET roster = roster || '0' WHERE id = $1`, [dealt]);
            await c.query(`INSERT INTO game_snapshots (player_ids, moves) VALUES ('["not-a-uuid"]', '\\x00')`);
            await assert.rejects(c.query(sqlOf(BYTEA)), (e: Error) =>
                e.message.includes(`player_views ${joined}/${ALICE}`) && e.message.includes(`games ${dealt} roster`) && /game_snapshots [0-9a-f-]{36}/.test(e.message));
        });
        const { rows } = await pgPool.query(`SELECT data_type FROM information_schema.columns WHERE table_name = 'games' AND column_name = 'state'`);
        assert.equal(rows[0].data_type, 'text', 'nothing converted');
    });

    test('it applies: every blob is the bytes it spelled, every scalar and updated_at untouched, and C writes the same envelopes, which are the cached views', async () => {
        await pgPool.query(sqlOf(BYTEA));
        const types = (await pgPool.query(
            `SELECT table_name || '.' || column_name || ' ' || data_type || coalesce(' ' || udt_name, '') AS c FROM information_schema.columns
             WHERE table_schema = 'public' AND ((table_name = 'games' AND column_name IN ('state', 'roster', 'logs_packed', 'game_seed'))
                OR (table_name IN ('player_views', 'spectator_views') AND column_name = 'view')
                OR (table_name = 'game_snapshots' AND column_name = 'player_ids')) ORDER BY 1`)).rows.map((r) => r.c);
        assert.deepEqual(types, [
            'game_snapshots.player_ids ARRAY _uuid', 'games.game_seed text text', 'games.logs_packed bytea bytea',
            'games.roster bytea bytea', 'games.state bytea bytea', 'player_views.view bytea bytea', 'spectator_views.view bytea bytea',
        ]);
        const after = await blobs();
        assert.deepEqual(after, beforeBlobs, 'every blob byte for byte, and version, status, needs_bots, round_epoch, seed, updated_at');
        let compared = 0;
        for (const id of Object.keys(after.games)) {
            const envelopes = await envelopesOf(id);
            assert.deepEqual(envelopes, envelopesBefore.get(id), `${id}: C writes the envelopes it wrote before`);
            assert.deepEqual(await cachedViews(id), envelopes, `${id}: every cached view is the envelope C writes from the row`);
            compared += envelopes.size;
        }
        assert.ok(compared >= 12 + 8, `every human seat and spectator compared (${compared})`);
        process.stderr.write(`[table_bytea] ${Object.keys(after.games).length} rows, ${Object.keys(after.views).length} player views, ${Object.keys(after.spectators).length} spectator views byte-equal; ${compared} envelopes\n`);
    });

    test('snapshot seat lists are UUID[] in seat order, and the participants policy reads exactly as before', async () => {
        const { rows } = await pgPool.query(`SELECT id::text, player_ids FROM game_snapshots ORDER BY id`);
        assert.deepEqual(rows, [{ id: SNAP_BOTH, player_ids: [DMITRY, ALICE] }, { id: SNAP_ALICE, player_ids: [ALICE] }]);
        assert.deepEqual(await snapshotIdsAs(ALICE), [SNAP_BOTH, SNAP_ALICE]);
        assert.deepEqual(await snapshotIdsAs(DMITRY), [SNAP_BOTH]);
        assert.deepEqual(await snapshotIdsAs(STRANGER), []);
        const idx = (await pgPool.query(`SELECT indexdef FROM pg_indexes WHERE indexname = 'idx_game_snapshots_player_ids'`)).rows;
        assert.match(idx[0].indexdef, /USING gin \(player_ids\)/);
    });

    test('the writers write bytes: a join with its roster, a move without one (NULL keeps the stored roster), a create', async () => {
        let r = (await pgPool.query('SELECT state, roster, version FROM games WHERE id = $1', [idOf('lobby_1_seat')])).rows[0];
        const id = idOf('lobby_1_seat');
        assert.equal(table.load(bytes(r.state), bytes(r.roster)), TABLE_OK);
        assert.equal(table.join(DMITRY, 'Dmitry'), TABLE_OK);
        let p = table.commit(id, Number(r.version) + 1, 0) as TableProducts;
        assert.deepEqual(await commitTableSql(id, Number(r.version), p, table.seats()),
            { committed: true, new_version: String(Number(r.version) + 1), new_round_epoch: '0' });
        let stored = (await pgPool.query('SELECT state, roster FROM games WHERE id = $1', [id])).rows[0];
        assert.equal(stored.state, `\\x${hexOf(p.state)}`, 'the state is stored as its bytes');
        assert.equal(stored.roster, `\\x${hexOf(p.roster)}`, 'the changed roster is stored as its bytes');
        assert.deepEqual(await cachedViews(id), await envelopesOf(id), 'the views are the envelopes, as bytes');

        r = (await pgPool.query('SELECT state, roster, version, logs_packed FROM games WHERE id = $1', [dealt])).rows[0];
        assert.equal(table.load(bytes(r.state), bytes(r.roster)), TABLE_OK);
        p = table.commit(dealt, Number(r.version) + 1, 0) as TableProducts;
        assert.equal(p.rosterChanged, false);
        const extra = Uint8Array.of(1, 2, 3, 4);
        assert.equal((await commitTableSql(dealt, Number(r.version), { ...p, logs: extra }, table.seats())).committed, true);
        stored = (await pgPool.query('SELECT roster, logs_packed FROM games WHERE id = $1', [dealt])).rows[0];
        assert.equal(stored.roster, r.roster, 'a NULL roster keeps the stored bytes');
        assert.equal(stored.logs_packed, `${r.logs_packed}01020304`, 'log records append as bytes');
        assert.deepEqual(await cachedViews(dealt), await envelopesOf(dealt));

        const conflict = await commitTableSql(dealt, Number(r.version), p, table.seats());
        assert.deepEqual(conflict, { committed: false, new_version: null, new_round_epoch: null }, 'the version fence');

        assert.equal(table.create(ALICE, 'alice'), TABLE_OK);
        const c = table.commit('kb0002', 0, 0) as TableProducts;
        await createTableSql('kb0002', ALICE, c);
        stored = (await pgPool.query('SELECT state, roster, logs_packed FROM games WHERE id = $1', ['kb0002'])).rows[0];
        assert.deepEqual(stored, { state: `\\x${hexOf(c.state)}`, roster: `\\x${hexOf(c.roster)}`, logs_packed: null });
        assert.deepEqual(await cachedViews('kb0002'), await envelopesOf('kb0002'));
    });
}
