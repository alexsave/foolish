// table_rpc.ts - commit_table and create_table called in SQL with one operation's
// kernel products, the way table_io.ts commitProducts and the create function
// send them through PostgREST: base64 blobs, the roster only when the operation
// changed it, the views as parallel id and envelope arrays, named arguments.
//
// For the migration suites, which build a database by hand (captured rows, then
// each migration) and call the writers directly on it rather than through the
// server's supabase client.

import type { TableProducts, TableSeat } from '../../sdk/ts/table/server_table.ts';
import { pgPool } from '../harness.ts';

type Db = { query: (sql: string, params?: unknown[]) => Promise<{ rows: any[] }> };

const b64 = (b: Uint8Array) => Buffer.from(b).toString('base64');

/** commit_table's OUT columns, as pg returns them (a BIGINT arrives as a string). */
export interface CommitResult { committed: boolean; new_version: string | null; new_round_epoch: string | null }

export interface CommitOptions {
    /** Send the roster even when the operation left it alone (default: only when changed, as the server does). */
    roster?: 'changed' | 'always' | 'never';
    /** Membership arrays (default: when the roster changed, as the server does). */
    membership?: boolean;
    logs?: boolean;
    gameSeedHex?: string | null;
}

/** commit_table with the products of `p` for the loaded `seats`. */
export async function commitTableSql(
    gameId: string, expectedVersion: number, p: TableProducts, seats: TableSeat[] | { id: string; brain: string }[],
    opts: CommitOptions = {}, db: Db = pgPool,
): Promise<CommitResult> {
    const sendRoster = opts.roster === 'always' || (opts.roster !== 'never' && p.rosterChanged);
    const membership = opts.membership ?? p.rosterChanged;
    const humans = seats.flatMap((_, i) => (p.views[i] ? [i] : []));
    const { rows } = await db.query(
        `SELECT * FROM commit_table(
            p_game_id => $1, p_expected_version => $2, p_state => $3, p_status => $4::smallint, p_needs_bots => $5,
            p_roster => $6, p_seats => $7::uuid[], p_bot_seats => $8::uuid[], p_logs_packed => $9, p_logs_reset => $10,
            p_game_seed => $11, p_view_players => $12::uuid[], p_views => $13::text[], p_spectator => $14, p_closed_round => $15)`, [
            gameId, expectedVersion, b64(p.state), p.status, p.needsBots,
            sendRoster ? b64(p.roster) : null,
            membership ? seats.filter((s) => !s.brain).map((s) => s.id) : null,
            membership ? seats.filter((s) => s.brain).map((s) => s.id) : null,
            opts.logs !== false && p.logs ? b64(p.logs) : null, p.logsReset,
            opts.gameSeedHex ?? null,
            humans.map((i) => seats[i].id), humans.map((i) => b64(p.views[i]!)),
            b64(p.spectator), p.closedRound,
        ]);
    return rows[0];
}

/** create_table with table_create's products: the creator is seat 0. */
export async function createTableSql(gameId: string, creatorId: string, p: TableProducts, db: Db = pgPool, withViews = true): Promise<void> {
    await db.query(
        'SELECT create_table(p_game_id => $1, p_player_id => $2, p_state => $3, p_roster => $4, p_view => $5, p_spectator => $6)', [
            gameId, creatorId, b64(p.state), b64(p.roster),
            withViews ? b64(p.views[0]!) : null, withViews ? b64(p.spectator) : null,
        ]);
}
