// table_db.ts - a C-built fixture into a games row (docs/C_GAME_SHAPE_MIGRATION.md Phase 3c).
//
// seedTable writes a TableFixture (e2e/helpers/table_fixture.ts) the way the
// kernel writers do (create_table / commit_table, seed.sql): the state and
// roster blobs, the status column and needs_bots from the kernel, and the
// membership rows every realtime policy reads. It replaced the harness's JSONB
// seedGame in Phase 4b; since the contract migration (Phase 4c) a games row has
// no other columns to fill.
//
// What the row says about its seats comes from the kernel: the fixture is
// loaded into the C Table and the seats, bot brains, status and needs_bots are
// read back from it, never from the builder's arguments. Seat ids must be UUIDs
// (auth.users and bots key on them).

import { pgPool } from '../harness.ts';
import * as L from '../../sdk/ts/gen/game_layout.bots.ts';
import { fixtureTable, reasonOf, type TableFixture } from './table_fixture.ts';

const hex = (b: Uint8Array) => `\\x${Buffer.from(b).toString('hex')}`;

export interface SeedTableOptions {
    /** games.version (default 0). */
    version?: number;
}

/** Inserts the row, its humans (auth.users, player_hands) and bots (bots, bot_hands), in one transaction. */
export async function seedTable(gameId: string, fx: TableFixture, opts: SeedTableOptions = {}): Promise<void> {
    const version = opts.version ?? 0;
    // One kernel section: load, read everything the row needs, before any await.
    const table = fixtureTable();
    const rc = table.load(fx.state, fx.roster);
    if (rc !== L.TABLE_OK) throw new Error(`seedTable: the fixture does not load: ${reasonOf(rc, ['GAME_INVALID_', 'TABLE_E_'])} (${rc})`);
    const seats = table.seats();
    const products = table.commit(gameId, version, 0);
    if (typeof products === 'number') throw new Error(`seedTable: no products: ${reasonOf(products, ['TABLE_E_'])} (${products})`);

    const c = await pgPool.connect();
    try {
        await c.query('BEGIN');
        for (const s of seats) {
            if (s.brain) await c.query('INSERT INTO bots(id, nickname, strategy_key) VALUES ($1, $2, $3) ON CONFLICT (id) DO NOTHING', [s.id, s.name, s.brain]);
            else await c.query('INSERT INTO auth.users(id) VALUES ($1) ON CONFLICT DO NOTHING', [s.id]);
        }
        await c.query(
            `INSERT INTO games (id, status, state, roster, needs_bots, version)
             VALUES ($1, (enum_range(NULL::game_status))[$2 + 1], $3, $4, $5, $6)`,
            [gameId, products.status, hex(products.state), hex(products.roster), products.needsBots, version]);
        for (const s of seats) {
            if (s.brain) await c.query('INSERT INTO bot_hands(game_id, bot_id) VALUES ($1, $2)', [gameId, s.id]);
            else await c.query('INSERT INTO player_hands(game_id, player_id) VALUES ($1, $2)', [gameId, s.id]);
        }
        await c.query('COMMIT');
    } catch (e) {
        await c.query('ROLLBACK');
        throw e;
    } finally {
        c.release();
    }
}

/**
 * Replaces a stored row's board in place, as a test that tampers with a game
 * does: the state and roster blobs, and the kernel's status and needs_bots for
 * them. version, round_epoch, the session log and the membership rows are left
 * alone, and so is this process's server row cache (clear it if the server
 * could hold the row: game_cache.ts __clearGameCache).
 */
export async function overwriteTable(gameId: string, fx: TableFixture): Promise<void> {
    const table = fixtureTable();
    const rc = table.load(fx.state, fx.roster);
    if (rc !== L.TABLE_OK) throw new Error(`overwriteTable: the fixture does not load: ${reasonOf(rc, ['GAME_INVALID_', 'TABLE_E_'])} (${rc})`);
    const p = table.commit(gameId, 0, 0);
    if (typeof p === 'number') throw new Error(`overwriteTable: no products (${p})`);
    const { rowCount } = await pgPool.query(
        `UPDATE games SET state = $2, roster = $3, status = (enum_range(NULL::game_status))[$4 + 1], needs_bots = $5 WHERE id = $1`,
        [gameId, hex(p.state), hex(p.roster), p.status, p.needsBots]);
    if (rowCount !== 1) throw new Error(`overwriteTable: no games row ${gameId}`);
}
