// The pre-migration golden rows load and read with today's code.
//
// e2e/fixtures/pre_table/ is the record of the row shapes the hosted database
// holds before the C game shape migration rewrites `games` (docs/
// C_GAME_SHAPE_MIGRATION.md 0.4): a frozen copy of seed.sql and the rows
// e2e/helpers/capture_pre_table_rows.ts captured from the real handlers.
// Later phases load the same rows to prove live games survive the deploy; this
// smoke test pins that the fixture is sound to begin with:
//
//   - the rows load into the frozen schema they were captured under;
//   - every scenario the plan lists is present, in the status it was captured in;
//   - today's loadCompleteGame reads every games row, with the roster the
//     column holds and a dealt board wherever a blob is authoritative;
//   - the WAITING row carrying a stale finished blob loads as a lobby with no cards;
//   - every cached player_views / spectator_views envelope still decodes, as the
//     viewer and at the version its row says.

import './harness.ts';
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { applySchema, pgPool } from './harness.ts';
import { loadCompleteGame } from '../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { decodePackedGame } from '../sdk/ts/wire/view.ts';
import { hexToBytes } from '../server/api/common/replay/codec.ts';
import { GAME_STATUS } from '../server/api/core/types.ts';

const FIXTURE = join(process.cwd(), 'e2e', 'fixtures', 'pre_table');
const rowsSql = readFileSync(join(FIXTURE, 'rows.sql'), 'utf8');

// The scenario each captured game stands for, from the dump's header.
const scenarios = new Map<string, string>(); // key -> game id
for (const m of rowsSql.matchAll(/^--\s+(\w+): (\w+) - /gm)) scenarios.set(m[2], m[1]);

const EXPECTED_STATUS: Record<string, string> = {
    lobby_1_seat: GAME_STATUS.WAITING,
    lobby_3_seat_bots: GAME_STATUS.WAITING,
    humans_2_mid_bout_good: GAME_STATUS.PLAYING,
    mixed_4_after_pickup: GAME_STATUS.PLAYING,
    bots_8_mid_game: GAME_STATUS.PLAYING,
    finished: GAME_STATUS.GAME_OVER,
    continued_lobby: GAME_STATUS.WAITING,
    waiting_stale_blob: GAME_STATUS.WAITING,
};

const idOf = (key: string): string => {
    const id = scenarios.get(key);
    assert.ok(id, `the fixture has a ${key} game`);
    return id;
};

if (!process.env.VALIDATION_ONLY) {
    before(async () => {
        // applySchema creates this file's database (and owns its teardown); the
        // platform shim then resets the schemas so the FROZEN seed.sql, not
        // today's, is what the rows load into.
        await applySchema();
        await pgPool.query(readFileSync(join(process.cwd(), 'e2e', 'schema.sql'), 'utf8'));
        await pgPool.query(readFileSync(join(FIXTURE, 'schema.sql'), 'utf8'));
        await pgPool.query(rowsSql);
    });

    test('every planned scenario is in the fixture, in the status it was captured in', async () => {
        assert.deepEqual([...scenarios.keys()].sort(), Object.keys(EXPECTED_STATUS).sort());
        const { rows } = await pgPool.query('SELECT id, status::text FROM games');
        assert.equal(rows.length, scenarios.size, 'one games row per scenario');
        const status = new Map(rows.map((r) => [r.id, r.status]));
        for (const [key, want] of Object.entries(EXPECTED_STATUS)) {
            assert.equal(status.get(idOf(key)), want, `${key} is ${want}`);
        }
    });

    test('the shapes the migration must carry are in the rows', async () => {
        const g = async (key: string) => (await pgPool.query('SELECT * FROM games WHERE id=$1', [idOf(key)])).rows[0];
        const bots8 = await g('bots_8_mid_game');
        assert.equal(bots8.players.length, 8);
        assert.ok(bots8.players.every((p: { is_ai: boolean }) => p.is_ai), 'eight bots, no human');
        const good = await g('humans_2_mid_bout_good');
        assert.equal(good.players.filter((p: { is_ai: boolean }) => !p.is_ai).length, 2);
        assert.ok(good.good_players.length > 0 && good.table_battles.length > 0, 'a good said with a battle on the table');
        const mixed = await g('mixed_4_after_pickup');
        assert.deepEqual(mixed.players.map((p: { is_ai: boolean }) => p.is_ai).sort(), [false, false, true, true]);
        const titles = (await pgPool.query('SELECT name FROM games')).rows.map((r) => r.name as string);
        assert.ok(titles.some((t) => t.length === 50 && Buffer.byteLength(t) === 150), 'a 50-character title in 3-byte characters');
        assert.ok(titles.some((t) => t.length === 50 && Buffer.byteLength(t) === 50), 'a 50-character ASCII title');
        const names = (await pgPool.query(`SELECT p->>'name' AS name FROM games, jsonb_array_elements(players) p`)).rows.map((r) => r.name as string);
        for (const width of [2, 3, 4]) {
            assert.ok(names.some((n) => [...n].some((ch) => Buffer.byteLength(ch) === width)), `a seat name with a ${width}-byte character`);
        }
    });

    test('loadCompleteGame reads every captured games row', async () => {
        const { rows } = await pgPool.query('SELECT id, status::text, players, state FROM games ORDER BY id');
        for (const row of rows) {
            const game = await loadCompleteGame(row.id);
            assert.equal(game.id, row.id);
            assert.equal(game.status, row.status, `${row.id}: status is the column's`);
            assert.deepEqual(game.players.map((p) => p.player_id), row.players.map((p: { player_id: string }) => p.player_id),
                `${row.id}: the roster is the column's, in seat order`);
            assert.deepEqual(game.players.map((p) => p.name), row.players.map((p: { name: string }) => p.name),
                `${row.id}: names survive byte for byte`);
            if (row.status === GAME_STATUS.PLAYING) {
                const cards = game.players.reduce((n, p) => n + p.hand.length, 0) + game.deck.length;
                assert.ok(cards > 0, `${row.id}: a dealt game loads a board from its blob`);
            }
        }
    });

    test('the WAITING row carrying a stale finished blob loads as a lobby with no cards', async () => {
        const id = idOf('waiting_stale_blob');
        const { rows } = await pgPool.query('SELECT status::text, state FROM games WHERE id=$1', [id]);
        assert.equal(rows[0].status, GAME_STATUS.WAITING);
        assert.ok(rows[0].state, 'precondition: the row really does carry a blob');
        const game = await loadCompleteGame(id);
        assert.equal(game.status, GAME_STATUS.WAITING);
        assert.deepEqual(game.players.map((p) => p.hand.length), game.players.map(() => 0), 'no hand from the finished session');
        assert.deepEqual(game.deck, [], 'no deck');
        assert.equal(game.table_battles.length, 0, 'no table');
        assert.equal(game.flipped, null, 'no trump');
    });

    test('every cached view envelope decodes for its viewer at its row version', async () => {
        const players = await pgPool.query(
            `SELECT v.game_id, v.player_id::text, v.view, v.version, g.players
               FROM player_views v JOIN games g ON g.id = v.game_id ORDER BY v.game_id, v.player_id`);
        assert.ok(players.rows.length > 0, 'the fixture has player views');
        for (const r of players.rows) {
            const d = decodePackedGame(hexToBytes(r.view));
            assert.ok(d, `${r.game_id}/${r.player_id}: decodes`);
            assert.equal(d!.version, Number(r.version), `${r.game_id}/${r.player_id}: envelope version`);
            assert.equal(d!.seat, r.players.findIndex((p: { player_id: string }) => p.player_id === r.player_id),
                `${r.game_id}/${r.player_id}: the viewer's own seat`);
        }
        const spectators = await pgPool.query('SELECT game_id, view, version FROM spectator_views ORDER BY game_id');
        assert.equal(spectators.rows.length, scenarios.size, 'one spectator view per game');
        for (const r of spectators.rows) {
            const d = decodePackedGame(hexToBytes(r.view));
            assert.ok(d, `${r.game_id}: spectator view decodes`);
            assert.equal(d!.seat, -1, `${r.game_id}: as a spectator`);
            assert.equal(d!.version, Number(r.version), `${r.game_id}: envelope version`);
        }
    });
}
