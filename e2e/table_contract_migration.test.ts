/* =============================================================================
 * The contract migration drops the JSONB game shape without touching a game
 * =============================================================================
 * docs/C_GAME_SHAPE_MIGRATION.md 3.4 and Phase 4c. The hosted database goes
 * through three deploys: 4a (expand: every row gains the kernel's state and
 * roster blobs, a bridge trigger keeps them in step), 4b (the edge functions
 * read and write only those blobs, through commit_table and create_table), and
 * 4c (contract: the JSONB columns, writer_gen, games.name, the legacy writers,
 * the bridge, their indexes and the client grants on games go).
 *
 * This builds that history on the rows the hosted database really held:
 *
 *   e2e/fixtures/pre_table/schema.sql + rows.sql   (captured from the pre-4a server)
 *     + pre_blob_finished.sql                       (a finished game from before
 *                                                    games.state existed)
 *     -> 20260917140000_table_expand.sql            (4a)
 *     -> kernel writes as 4b makes them             (a join taken over by
 *                                                    commit_table, a dealt row
 *                                                    committed, a new create_table)
 *     -> the contract migration                     (4c)
 *
 * and holds every row to what it was before 4c: the same state and roster
 * bytes, version, status and needs_bots; table_load accepts it; and the
 * envelope C writes for every human seat and the spectator is byte for byte
 * the one before 4c and the one cached in player_views / spectator_views. Then
 * the kernel writers keep working on the contracted table, nothing named like a
 * legacy writer is left, and no client role holds any privilege on games.
 *
 * The migration refuses to run while no row is owned by the kernel writers
 * (4b is not live, so today's functions still call commit_game) and while any
 * row lacks a blob. Both refusals are exercised in rolled-back transactions.
 *
 * One kind of row can never satisfy the second refusal: a game that reached
 * game_over before games.state existed (20260707120000) has a JSONB record of
 * itself and no blob, and 4a cannot synthesise one (it builds a lobby blob for
 * WAITING rows only). The hosted database has 34 of them. The owner's call is
 * to delete them, which the migration does before the refusal, exactly on
 * `status = 'game_over' AND state IS NULL` - so a waiting or playing row
 * without a blob still stops the deploy.
 * ========================================================================== */

import './harness.ts';
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import type { PoolClient } from 'pg';
import { applyPlatformShim, pgPool } from './harness.ts';
import { createServerTable, type TableProducts } from '../sdk/ts/table/server_table.ts';
import { TABLE_OK } from '../sdk/ts/gen/game_layout.bots.ts';
import { commitTableSql, createTableSql } from './helpers/table_rpc.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const FIXTURE = join(process.cwd(), 'e2e', 'fixtures', 'pre_table');
const MIGRATIONS = join(process.cwd(), 'server', 'impls', 'supabase', 'migrations');
const EXPAND = '20260917140000_table_expand.sql';

/** The contract migration: the one file after the expand migration named *_table_contract.sql. */
function contractSql(): string {
    const files = readdirSync(MIGRATIONS).filter((f) => /^\d{14}_table_contract\.sql$/.test(f));
    assert.equal(files.length, 1, `exactly one *_table_contract.sql migration (found: ${files.join(', ') || 'none'})`);
    assert.ok(files[0] > EXPAND, `${files[0]} runs after ${EXPAND}`);
    return readFileSync(join(MIGRATIONS, files[0]), 'utf8');
}

const table = createServerTable();
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

// e2e/fixtures/pre_table/pre_blob_finished.sql: the finished game from before
// games.state existed, and the snapshot rows of it and of its blob-carrying twin.
const PRE_BLOB = 'prb001';
const PRE_BLOB_SNAPSHOT = '00000000-0000-4000-8000-0000000000f2';
const CONTROL_SNAPSHOT = '00000000-0000-4000-8000-0000000000f3';

// The final games shape (docs/C_GAME_SHAPE_MIGRATION.md 3.1): column, type, nullable.
const FINAL_COLUMNS = [
    'bot_lease_token uuid YES', 'bot_lease_until timestamp with time zone YES', 'created_at timestamp without time zone YES',
    'game_seed text YES', 'id text NO', 'logs_packed text YES', 'needs_bots boolean NO', 'roster text NO',
    'round_epoch bigint NO', 'state text NO', 'status USER-DEFINED NO', 'updated_at timestamp without time zone YES',
    'version bigint NO',
];

interface Stored {
    id: string; state: string; roster: string; status: string; needs_bots: boolean; version: string;
    round_epoch: string; game_seed: string | null; logs_packed: string | null;
}
const STORED_COLS = 'id, state, roster, status::text AS status, needs_bots, version, round_epoch, game_seed, logs_packed';
const storedRows = async (): Promise<Stored[]> => (await pgPool.query(`SELECT ${STORED_COLS} FROM games ORDER BY id`)).rows;

/** Every envelope C writes for the row (each human seat by id, and the spectator), as hex. */
function envelopesOf(r: Stored): Map<string, string> {
    const rc = table.load(bytes(r.state), bytes(r.roster));
    assert.equal(rc, TABLE_OK, `${r.id}: table_load accepts the row (rc ${rc}, detail ${table.detail()})`);
    const out = new Map<string, string>();
    const version = Number(r.version);
    table.seats().forEach((s, seat) => {
        if (s.brain) return;
        const e = table.envelope(r.id, seat, version);
        assert.ok(e instanceof Uint8Array, `${r.id}/${s.id}: table_envelope (${e})`);
        out.set(s.id, hexOf(e));
    });
    const spec = table.envelope(r.id, -1, version);
    assert.ok(spec instanceof Uint8Array, `${r.id}/spectator: table_envelope (${spec})`);
    out.set('spectator', hexOf(spec));
    return out;
}

async function cachedViews(gameId: string): Promise<Map<string, string>> {
    const out = new Map<string, string>();
    for (const v of (await pgPool.query('SELECT player_id::text, view FROM player_views WHERE game_id = $1', [gameId])).rows) out.set(v.player_id, v.view);
    for (const v of (await pgPool.query('SELECT view FROM spectator_views WHERE game_id = $1', [gameId])).rows) out.set('spectator', v.view);
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

if (!process.env.VALIDATION_ONLY) {
    let before4c: Stored[] = [];
    const envelopesBefore = new Map<string, Map<string, string>>();
    const membersBefore = new Map<string, unknown>();
    const takenOver = idOf('lobby_3_seat_bots');
    const dealtCommitted = idOf('mixed_4_after_pickup');
    const created = 'kc0001';

    before(async () => {
        await applyPlatformShim();
        await pgPool.query(readFileSync(join(FIXTURE, 'schema.sql'), 'utf8'));
        await pgPool.query(rowsSql);
        await pgPool.query(readFileSync(join(FIXTURE, 'pre_blob_finished.sql'), 'utf8'));
        await pgPool.query(readFileSync(join(MIGRATIONS, EXPAND), 'utf8'));
    });

    test('4c refuses to run while no row is owned by the kernel writers (the 4b functions are not live)', async () => {
        const sql = contractSql();
        await inRolledBackTx(async (c) => {
            await assert.rejects(c.query(sql), /no games row is owned by the kernel writers/,
                'on the rows 4a left, only the legacy writers have written');
        });
        // Nothing was dropped.
        const { rows } = await pgPool.query(`SELECT count(*)::int AS n FROM pg_proc WHERE proname = 'commit_game'`);
        assert.equal(rows[0].n, 1);
    });

    test('4b\'s writes land on the expanded rows: a lobby taken over by a join, a dealt row committed, a new table', async () => {
        const lobby = (await pgPool.query(`SELECT ${STORED_COLS} FROM games WHERE id = $1`, [takenOver])).rows[0] as Stored;
        assert.equal(table.load(bytes(lobby.state), bytes(lobby.roster)), TABLE_OK);
        assert.equal(table.join(ALICE, 'alice'), TABLE_OK);
        let p = table.commit(takenOver, Number(lobby.version) + 1, 0) as TableProducts;
        assert.ok(p.rosterChanged, 'a join changes the roster, so the commit carries it');
        assert.deepEqual(await commitTableSql(takenOver, Number(lobby.version), p, table.seats()),
            { committed: true, new_version: String(Number(lobby.version) + 1), new_round_epoch: '0' });
        assert.equal((await pgPool.query('SELECT roster FROM games WHERE id = $1', [takenOver])).rows[0].roster, `\\x${hexOf(p.roster)}`,
            'the roster the commit carried is stored, as the column\'s \\x-hex text');

        const dealt = (await pgPool.query(`SELECT ${STORED_COLS} FROM games WHERE id = $1`, [dealtCommitted])).rows[0] as Stored;
        assert.equal(table.load(bytes(dealt.state), bytes(dealt.roster)), TABLE_OK);
        p = table.commit(dealtCommitted, Number(dealt.version) + 1, 0) as TableProducts;
        assert.equal(p.rosterChanged, false, 'a dealt commit leaves the roster alone');
        assert.equal((await commitTableSql(dealtCommitted, Number(dealt.version), p, table.seats())).committed, true);
        assert.equal((await pgPool.query('SELECT roster FROM games WHERE id = $1', [dealtCommitted])).rows[0].roster, dealt.roster,
            'a NULL roster keeps the stored one, byte for byte');

        assert.equal(table.create(ALICE, 'alice'), TABLE_OK);
        p = table.commit(created, 0, 0) as TableProducts;
        await createTableSql(created, ALICE, p);

        const gens = (await pgPool.query('SELECT id, writer_gen FROM games WHERE writer_gen = 2 ORDER BY id')).rows.map((r) => r.id);
        assert.deepEqual(gens, [takenOver, dealtCommitted, created].sort(), 'exactly those rows are the kernel writers\'');
        const total = (await pgPool.query('SELECT count(*)::int AS n FROM games')).rows[0].n as number;
        assert.ok(total - gens.length >= 5, 'most rows are still the legacy writers\', untouched since 4a');
    });

    test('4a leaves a game that finished before games.state existed with a roster and no blob', async () => {
        const { rows } = await pgPool.query(
            `SELECT state, roster IS NOT NULL AS has_roster, status::text AS status, writer_gen FROM games WHERE id = $1`, [PRE_BLOB]);
        assert.deepEqual(rows, [{ state: null, has_roster: true, status: 'game_over', writer_gen: 1 }],
            '4a synthesises a lobby blob for WAITING rows only, so this one arrives at 4c with a NULL state');
    });

    test('4c refuses a row without a state or roster blob, naming it', async () => {
        const sql = contractSql();
        const lobby = idOf('lobby_1_seat');
        await inRolledBackTx(async (c) => {
            // Only a hand-run write could leave one: every writer since 4a stores both.
            await c.query('ALTER TABLE games DISABLE TRIGGER games_legacy_bridge');
            await c.query('UPDATE games SET state = NULL WHERE id = $1', [lobby]);
            await assert.rejects(c.query(sql), new RegExp(`without a state or roster blob: ${lobby}`));
        });
    });

    test('4c applies: every surviving row keeps its blobs, version, status and needs_bots, and C writes the same envelopes, which are the cached views', async () => {
        const all = await storedRows();
        assert.equal(all.length, scenarios.size + 2, 'every captured game, the pre-blob one and the new table');
        before4c = all.filter((r) => r.id !== PRE_BLOB);
        for (const r of before4c) {
            envelopesBefore.set(r.id, envelopesOf(r));
            membersBefore.set(r.id, {
                humans: (await pgPool.query('SELECT player_id::text FROM player_hands WHERE game_id = $1 ORDER BY 1', [r.id])).rows,
                bots: (await pgPool.query('SELECT bot_id::text FROM bot_hands WHERE game_id = $1 ORDER BY 1', [r.id])).rows,
                views: await cachedViews(r.id),
            });
        }
        const updatedAt = (await pgPool.query('SELECT id, updated_at FROM games WHERE id <> $1 ORDER BY id', [PRE_BLOB])).rows;

        await pgPool.query(contractSql());

        const after = await storedRows();
        assert.deepEqual(after, before4c, 'state, roster, status, needs_bots, version, round_epoch, deal seed and session log are untouched');
        assert.deepEqual((await pgPool.query('SELECT id, updated_at FROM games ORDER BY id')).rows, updatedAt,
            'updated_at is untouched (the bot heartbeat scans it)');
        let compared = 0;
        for (const r of after) {
            const envelopes = envelopesOf(r);
            assert.deepEqual(envelopes, envelopesBefore.get(r.id), `${r.id}: C writes the envelopes it wrote before 4c`);
            const members = membersBefore.get(r.id) as { views: Map<string, string> };
            assert.deepEqual({
                humans: (await pgPool.query('SELECT player_id::text FROM player_hands WHERE game_id = $1 ORDER BY 1', [r.id])).rows,
                bots: (await pgPool.query('SELECT bot_id::text FROM bot_hands WHERE game_id = $1 ORDER BY 1', [r.id])).rows,
                views: await cachedViews(r.id),
            }, members, `${r.id}: membership and cached views are untouched`);
            assert.deepEqual(members.views, envelopes, `${r.id}: every cached view is the envelope C writes from the row`);
            compared += envelopes.size;
        }
        assert.ok(compared >= 12 + 8, `every human seat and spectator of every game compared (${compared})`);
        process.stderr.write(`[table_contract] ${after.length} rows, ${compared} envelopes byte-equal to before 4c and to the cached views\n`);
    });

    test('4c deletes the pre-blob finished game and takes exactly the rows its foreign keys say', async () => {
        const q = async (sql: string, p: unknown[] = []) => (await pgPool.query(sql, p)).rows;
        assert.deepEqual(await q('SELECT id FROM games WHERE id = $1', [PRE_BLOB]), [],
            'the game that never had a state blob is gone, so `state` can be NOT NULL');

        // ON DELETE CASCADE: membership, chat and both view caches of a game
        // nobody can open again.
        for (const t of ['player_hands', 'bot_hands', 'chat_messages', 'player_views', 'spectator_views'])
            assert.deepEqual(await q(`SELECT count(*)::int AS n FROM ${t} WHERE game_id = $1`, [PRE_BLOB]),
                [{ n: 0 }], `${t} cascaded away with the game`);

        // ON DELETE SET NULL: the replay outlives the game row. player_ids is
        // both the read ACL and what match history queries on, so the game stays
        // in its players' history and stays replayable.
        assert.deepEqual(await q(
            `SELECT game_id, player_ids, encode(moves, 'hex') AS moves FROM game_snapshots WHERE id = $1`, [PRE_BLOB_SNAPSHOT]),
            [{ game_id: null, player_ids: [ALICE, 'bf8245d5-0270-4aca-8282-000000000001'], moves: '0102030405' }],
            'the snapshot survives with its game_id cleared and its ACL intact');

        // The finished game that DOES have a blob is not in the predicate.
        const control = idOf('finished');
        assert.deepEqual(await q('SELECT id, status::text AS status FROM games WHERE id = $1', [control]),
            [{ id: control, status: 'game_over' }], 'a game_over row WITH a blob survives');
        assert.deepEqual(await q('SELECT game_id FROM game_snapshots WHERE id = $1', [CONTROL_SNAPSHOT]),
            [{ game_id: control }], 'and so does its replay, still pointing at it');
    });

    test('games is the final shape: the JSONB columns, writer_gen and name are gone, both blobs are NOT NULL', async () => {
        const cols = (await pgPool.query(
            `SELECT column_name || ' ' || data_type || ' ' || is_nullable AS c FROM information_schema.columns
             WHERE table_schema = 'public' AND table_name = 'games' ORDER BY 1`)).rows.map((r) => r.c);
        assert.deepEqual(cols, FINAL_COLUMNS);
        const indexes = (await pgPool.query(`SELECT indexname FROM pg_indexes WHERE schemaname = 'public' AND tablename = 'games' ORDER BY 1`)).rows.map((r) => r.indexname);
        assert.deepEqual(indexes, ['games_pkey', 'idx_games_bot_scan', 'idx_games_status', 'idx_games_updated_at']);
        const triggers = (await pgPool.query(`SELECT tgname FROM pg_trigger WHERE tgrelid = 'public.games'::regclass AND NOT tgisinternal ORDER BY 1`)).rows.map((r) => r.tgname);
        assert.deepEqual(triggers, ['update_games_updated_at'], 'the bridge trigger is gone');
        const policies = (await pgPool.query(`SELECT policyname FROM pg_policies WHERE schemaname = 'public' AND tablename = 'games'`)).rows;
        assert.deepEqual(policies, [], 'no games policy: RLS stays on and denies every client');
        await assert.rejects(pgPool.query(`INSERT INTO games (id, status, state) VALUES ('nullroster', 'waiting', '\\x00')`), /null value in column "roster"/);
    });

    test('no legacy writer is left: commit_game, create_game and every legacy_* function are gone', async () => {
        const { rows } = await pgPool.query(
            `SELECT p.oid::regprocedure::text AS fn FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
             WHERE n.nspname = 'public' AND (p.proname IN ('commit_game', 'create_game') OR p.proname LIKE 'legacy\\_%') ORDER BY 1`);
        assert.deepEqual(rows.map((r) => r.fn), []);
        await assert.rejects(pgPool.query(`SELECT commit_game('x', 0, '{}'::jsonb)`), /function commit_game\(.*\) does not exist/);
    });

    test('anon and authenticated hold no privilege at all on games, at table or column level', async () => {
        const { rows } = await pgPool.query(`
            SELECT r.role || ' ' || p.priv AS g
            FROM (VALUES ('anon'), ('authenticated')) AS r(role)
            CROSS JOIN (VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('DELETE'), ('TRUNCATE'), ('REFERENCES'), ('TRIGGER')) AS p(priv)
            WHERE CASE WHEN p.priv IN ('SELECT', 'INSERT', 'UPDATE', 'REFERENCES')
                       THEN has_any_column_privilege(r.role, 'public.games', p.priv)
                       ELSE has_table_privilege(r.role, 'public.games', p.priv) END
            ORDER BY 1`);
        assert.deepEqual(rows.map((r) => r.g), []);
    });

    test('the kernel writers keep working on the contracted table, and delete_account no longer touches a game', async () => {
        // A legacy-era lobby, joined through commit_table.
        const id = idOf('lobby_1_seat');
        const r = (await pgPool.query(`SELECT ${STORED_COLS} FROM games WHERE id = $1`, [id])).rows[0] as Stored;
        assert.equal(table.load(bytes(r.state), bytes(r.roster)), TABLE_OK);
        const joiner = '5566ca51-b3f7-4277-b366-000000000003';
        assert.equal(table.join(joiner, 'Zoë 🃏'), TABLE_OK);
        const p = table.commit(id, Number(r.version) + 1, 0) as TableProducts;
        assert.equal((await commitTableSql(id, Number(r.version), p, table.seats())).committed, true);
        const joined = (await pgPool.query(`SELECT ${STORED_COLS} FROM games WHERE id = $1`, [id])).rows[0] as Stored;
        assert.deepEqual(await cachedViews(id), envelopesOf(joined), 'the join\'s cached views are the envelopes of the stored row');
        assert.deepEqual(table.seats().map((s) => s.id), [ALICE, joiner]);
        // The next commit leaves the roster alone and sends none: the stored one stays.
        const same = table.commit(id, Number(joined.version) + 1, 0) as TableProducts;
        assert.equal(same.rosterChanged, false);
        assert.equal((await commitTableSql(id, Number(joined.version), same, table.seats())).committed, true);
        assert.equal((await pgPool.query('SELECT roster FROM games WHERE id = $1', [id])).rows[0].roster, joined.roster,
            'a NULL roster keeps the stored one on the contracted table');

        // A new table.
        assert.equal(table.create(joiner, 'Zoë 🃏'), TABLE_OK);
        const c = table.commit('kc0002', 0, 0) as TableProducts;
        await createTableSql('kc0002', joiner, c, pgPool, false);
        const made = (await pgPool.query(`SELECT ${STORED_COLS} FROM games WHERE id = 'kc0002'`)).rows[0] as Stored;
        assert.equal(made.status, 'waiting');
        envelopesOf(made);

        // delete_account: the name is redacted by the edge function through table_redact
        // and commit_table (Q8); the SQL only clears the leaderboard copy.
        const everyColumn = async () => (await pgPool.query('SELECT * FROM games ORDER BY id')).rows;
        const gamesBefore = await everyColumn();
        await pgPool.query('INSERT INTO user_elo_ratings (user_id, username) VALUES ($1, $2) ON CONFLICT (user_id) DO UPDATE SET username = EXCLUDED.username', [joiner, 'Zoë 🃏']);
        await pgPool.query('SELECT delete_account($1)', [joiner]);
        assert.deepEqual(await everyColumn(), gamesBefore, 'no games row was written (not even updated_at)');
        assert.deepEqual((await pgPool.query('SELECT username FROM user_elo_ratings WHERE user_id = $1', [joiner])).rows, [{ username: null }]);
    });
}
