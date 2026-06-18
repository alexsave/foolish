// Minimal pg-backed implementation of the @supabase/supabase-js surface the
// DEPLOYED server code actually uses. This is the ONLY server-side mock: the real
// _shared/*.ts (loadCompleteGame, commitGame, executeWithGameLock,
// broadcastAnimationEvents, the action handlers, the ELO/snapshot tail) run
// unmodified on top of it, against a REAL Postgres running the REAL commit_game /
// bot-lease plpgsql. PostgREST + Realtime are replaced by direct SQL and an
// in-process broadcast recorder; everything else is the genuine article.

import { Pool } from 'pg';

const pool = new Pool({
    host: process.env.E2E_PGHOST || '127.0.0.1',
    port: Number(process.env.E2E_PGPORT || 5432),
    user: process.env.E2E_PGUSER || 'stress',
    password: process.env.E2E_PGPASSWORD || 'stress',
    database: process.env.E2E_PGDATABASE || 'foolish',
    max: 40,
});
export const e2ePool = pool;

// Primary-key columns per table, for upsert conflict targets when not specified.
const PK: Record<string, string> = {
    games: 'id', game_decks: 'game_id', player_hands: 'game_id,player_id',
    bot_hands: 'game_id,bot_id', bots: 'id', game_logs: 'id',
    user_elo_ratings: 'user_id', game_snapshots: 'id',
};

type Result = { data: any; error: any };
const ok = (data: any): Result => ({ data, error: null });

// Build the nested object loadCompleteGame expects from its PostgREST embed:
//   games row + game_decks(object) + player_hands[] + bot_hands[].bots + game_logs[]
async function loadGamesEmbed(id: string): Promise<Result> {
    const c = await pool.connect();
    try {
        await c.query('BEGIN ISOLATION LEVEL REPEATABLE READ');
        const g = (await c.query('SELECT * FROM games WHERE id=$1', [id])).rows[0];
        if (!g) { await c.query('ROLLBACK'); return { data: null, error: { code: 'PGRST116', message: 'no rows' } }; }
        const deck = (await c.query('SELECT deck FROM game_decks WHERE game_id=$1', [id])).rows[0] ?? { deck: [] };
        const ph = (await c.query('SELECT player_id, hand, awaiting_attack FROM player_hands WHERE game_id=$1', [id])).rows;
        const bh = (await c.query(
            `SELECT bh.bot_id, bh.hand, bh.awaiting_attack, jsonb_build_object('strategy_key', b.strategy_key) AS bots
             FROM bot_hands bh JOIN bots b ON b.id = bh.bot_id WHERE bh.game_id=$1`, [id])).rows;
        const logs = (await c.query(
            'SELECT id, game_id, log_type, player_id, card_pairs, defender_index, created_at FROM game_logs WHERE game_id=$1', [id])).rows;
        await c.query('COMMIT');
        return ok({ ...g, game_decks: deck, player_hands: ph, bot_hands: bh, game_logs: logs });
    } catch (e) {
        try { await c.query('ROLLBACK'); } catch { /* */ }
        return { data: null, error: e };
    } finally { c.release(); }
}

interface Filter { col: string; op: 'eq' | 'in'; val: any }

class QueryBuilder implements PromiseLike<Result> {
    private filters: Filter[] = [];
    private selectCols = '*';
    private op: 'select' | 'insert' | 'upsert' | 'update' | 'delete' = 'select';
    private rows: any[] = [];
    private upsertOpts: { onConflict?: string; ignoreDuplicates?: boolean } = {};
    private orderCol?: string; private orderAsc = true; private limitN?: number;
    private wantSingle = false;

    constructor(private table: string) {}

    select(cols = '*') { this.selectCols = cols; if (this.op === 'select') this.op = 'select'; return this; }
    insert(rows: any) { this.op = 'insert'; this.rows = Array.isArray(rows) ? rows : [rows]; return this; }
    upsert(rows: any, opts: any = {}) { this.op = 'upsert'; this.rows = Array.isArray(rows) ? rows : [rows]; this.upsertOpts = opts; return this; }
    update(row: any) { this.op = 'update'; this.rows = [row]; return this; }
    delete() { this.op = 'delete'; return this; }
    eq(col: string, val: any) { this.filters.push({ col, op: 'eq', val }); return this; }
    in(col: string, val: any[]) { this.filters.push({ col, op: 'in', val }); return this; }
    order(col: string, opts: any = {}) { this.orderCol = col; this.orderAsc = opts.ascending !== false; return this; }
    limit(n: number) { this.limitN = n; return this; }
    single() { this.wantSingle = true; return this; }
    maybeSingle() { this.wantSingle = true; return this; }

    private where(params: any[]): string {
        if (this.filters.length === 0) return '';
        const parts = this.filters.map((f) => {
            if (f.op === 'in') { params.push(f.val); return `${f.col} = ANY($${params.length})`; }
            params.push(f.val); return `${f.col} = $${params.length}`;
        });
        return ' WHERE ' + parts.join(' AND ');
    }

    private async run(): Promise<Result> {
        // loadCompleteGame's embedded games select
        if (this.table === 'games' && this.op === 'select' && this.selectCols.includes('(')) {
            const id = this.filters.find((f) => f.col === 'id')?.val;
            return loadGamesEmbed(id);
        }
        try {
            if (this.op === 'select') {
                const params: any[] = [];
                let sql = `SELECT ${this.selectCols === '*' ? '*' : this.selectCols} FROM ${this.table}${this.where(params)}`;
                if (this.orderCol) sql += ` ORDER BY ${this.orderCol} ${this.orderAsc ? 'ASC' : 'DESC'}`;
                if (this.limitN != null) sql += ` LIMIT ${this.limitN}`;
                const r = await pool.query(sql, params);
                if (this.wantSingle) {
                    if (r.rows.length === 0) return { data: null, error: { code: 'PGRST116', message: 'no rows' } };
                    return ok(r.rows[0]);
                }
                return ok(r.rows);
            }
            if (this.op === 'delete') {
                const params: any[] = [];
                const sql = `DELETE FROM ${this.table}${this.where(params)}${this.selectCols !== '*' ? ` RETURNING ${this.selectCols}` : ''}`;
                const r = await pool.query(sql, params);
                return ok(r.rows);
            }
            // insert / upsert
            if (this.rows.length === 0) return ok([]);
            const cols = Array.from(new Set(this.rows.flatMap((row) => Object.keys(row))));
            const params: any[] = [];
            const valuesSql = this.rows.map((row) => '(' + cols.map((col) => {
                const v = row[col];
                params.push(v !== null && typeof v === 'object' ? JSON.stringify(v) : v);
                return `$${params.length}`;
            }).join(',') + ')').join(',');
            let sql = `INSERT INTO ${this.table} (${cols.join(',')}) VALUES ${valuesSql}`;
            if (this.op === 'upsert') {
                const conflict = this.upsertOpts.onConflict || PK[this.table] || 'id';
                if (this.upsertOpts.ignoreDuplicates) {
                    sql += ` ON CONFLICT (${conflict}) DO NOTHING`;
                } else {
                    const updates = cols.filter((col) => !conflict.split(',').includes(col)).map((col) => `${col}=EXCLUDED.${col}`);
                    sql += ` ON CONFLICT (${conflict}) DO UPDATE SET ${updates.join(',')}`;
                }
            }
            sql += ` RETURNING ${this.selectCols === '*' ? '*' : this.selectCols}`;
            const r = await pool.query(sql, params);
            if (this.wantSingle) return ok(r.rows[0] ?? null);
            return ok(r.rows);
        } catch (error) {
            return { data: null, error };
        }
    }

    then<R1 = Result, R2 = never>(onfulfilled?: ((v: Result) => R1 | PromiseLike<R1>) | null, onrejected?: ((r: any) => R2 | PromiseLike<R2>) | null): PromiseLike<R1 | R2> {
        return this.run().then(onfulfilled, onrejected);
    }
}

// ---- Broadcast recorder (replaces Realtime) -------------------------------
export interface RecordedSend { channel: string; event: string; payload: any; at: number }
export const broadcastLog: RecordedSend[] = [];
export const resetBroadcastLog = () => { broadcastLog.length = 0; };

class Channel {
    constructor(private name: string) {}
    async send(msg: { type: string; event: string; payload: any }) {
        broadcastLog.push({ channel: this.name, event: msg.event, payload: msg.payload, at: Date.now() });
        return 'ok';
    }
}

export const createClient = (_url?: string, _key?: string) => ({
    from: (table: string) => new QueryBuilder(table),
    rpc: async (name: string, params: Record<string, any> = {}): Promise<Result> => {
        try {
            const keys = Object.keys(params);
            const placeholders = keys.map((_k, i) => `$${i + 1}`).join(',');
            const vals = keys.map((k) => { const v = params[k]; return v !== null && typeof v === 'object' ? JSON.stringify(v) : v; });
            const r = await pool.query(`SELECT ${name}(${placeholders}) AS result`, vals);
            return ok(r.rows[0]?.result ?? null);
        } catch (error) { return { data: null, error }; }
    },
    channel: (name: string, _cfg?: any) => new Channel(name),
    removeChannel: async (_ch: any) => 'ok',
});

export type User = { id: string; user_metadata: { username: string }; email?: string };

export const closePool = () => pool.end();
