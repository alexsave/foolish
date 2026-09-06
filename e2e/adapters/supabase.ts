// Minimal pg-backed implementation of the @supabase/supabase-js surface the
// DEPLOYED server code actually uses. This is the ONLY server-side mock: the real
// _shared/*.ts (loadCompleteGame, commitGame, executeWithGameLock,
// broadcastAnimationEvents, the action handlers, the ELO/snapshot tail) run
// unmodified on top of it, against a REAL Postgres running the REAL commit_game /
// bot-lease plpgsql. PostgREST + Realtime are replaced by direct SQL and an
// in-process broadcast recorder; everything else is the genuine article.

import { Pool } from 'pg';
import { basename } from 'path';

// ---- One Postgres DATABASE per test file ---------------------------------
// The suite used to share one database, and every Postgres-backed file opened by
// DROPping and recreating the public/auth/realtime schemas out from under the
// others. That reset - not gameplay - is what forced `--test-concurrency=1`;
// concurrent_games.test.ts exists to prove the gameplay side is innocent (24 real
// games on ONE Postgres neither deadlock nor corrupt each other).
//
// So the shared thing is gone rather than serialised around. `E2E_PGDATABASE`
// (default `foolish`) is now only the maintenance database the CREATE/DROP
// statements are issued FROM; it holds no app tables, so the "relation \"games\"
// does not exist" cascade has nothing left to half-apply. Each file gets its own
// `e2e_<file>` database, created by applySchema() and dropped when the file ends.
//
// The name is derived from the FILE - not a random or clock-derived id - for two
// reasons. The determinism gate forbids entropy under e2e/. And a deterministic
// name is what makes a leaked database self-healing: applySchema() opens with
// `DROP DATABASE IF EXISTS ... WITH (FORCE)`, which terminates whatever backends
// a Ctrl-C'd run left holding it. Cleanup happens on ACQUIRE, not only on
// release, so a namespace a killed process left behind is inert - it belongs to
// exactly one file, and that file destroys it before it uses it.
const suiteFile = basename(process.argv[1] ?? 'e2e');
const suiteSlug = suiteFile.replace(/\.[cm]?[jt]sx?$/, '').replace(/\.test$/, '')
    .replace(/[^A-Za-z0-9_]/g, '_').toLowerCase();

/** Connection settings for the maintenance database (CREATE/DROP DATABASE run here). */
export const pgAdminConfig = {
    host: process.env.E2E_PGHOST || '127.0.0.1',
    port: Number(process.env.E2E_PGPORT || 5432),
    user: process.env.E2E_PGUSER || 'stress',
    password: process.env.E2E_PGPASSWORD || 'stress',
    database: process.env.E2E_PGDATABASE || 'foolish',
};

/** The database THIS test file owns. Nothing else reads or writes it. */
export const suiteDatabase = `${process.env.E2E_DB_PREFIX || 'e2e'}_${suiteSlug}`;

// ---- Connection budget ---------------------------------------------------
// Files run in parallel now, so the ceiling is (pool size x files in flight),
// not one pool. Postgres' default max_connections is 100 - the local dev server
// and the CI `postgres:16` service alike - with 3 slots reserved for superusers.
//
// Measured: across the whole serial suite the peak was 25 backends, all of them
// concurrent_games'. Two suites drive contention deliberately and would, with a
// small pool, queue on the POOL instead of on Postgres - which is the thing they
// exist to measure - so they keep a pool as wide as the race they run: 24 games
// for concurrent_games, 30 simultaneous lease acquires for lease. Everything
// else peaks in the low single digits and gets 8.
//
// Worst case at the DB lane's concurrency of 4 (scripts/run_e2e.mjs): both wide
// suites plus two ordinary ones = 24 + 30 + 8 + 8 = 70, plus at most one
// short-lived admin connection per file = 74. Comfortably inside 97.
const WIDE_POOLS: Record<string, number> = { concurrent_games: 24, lease: 30 };
const poolMax = Number(process.env.E2E_PG_POOL_MAX || WIDE_POOLS[suiteSlug] || 8);

const pool = new Pool({ ...pgAdminConfig, database: suiteDatabase, max: poolMax });

// pg's Pool.end() rejects when called twice, and the suite lifecycle now ends the
// pool from the harness (the same hook that drops the database) while ~20 suites
// still end it themselves in their own after(). Fold repeat calls onto the first
// promise so hook ordering can't turn cleanup into a spurious red.
const closePoolOnce = pool.end.bind(pool);
let poolClosing: Promise<void> | null = null;
(pool as unknown as { end: () => Promise<void> }).end = () => (poolClosing ??= closePoolOnce());

export const e2ePool = pool;

// Primary-key columns per table, for upsert conflict targets when not specified.
const PK: Record<string, string> = {
    games: 'id', player_hands: 'game_id,player_id',
    bot_hands: 'game_id,bot_id', bots: 'id',
    user_elo_ratings: 'user_id', game_snapshots: 'id',
};

type Result = { data: any; error: any };
const ok = (data: any): Result => ({ data, error: null });

// Build the nested object loadCompleteGame expects from its PostgREST embed:
//   games row + player_hands[] + bot_hands[].bots.
// No game_logs: the production select doesn't embed them (logs are loaded
// lazily, only at game end), so the shim shouldn't pay for them either.
async function loadGamesEmbed(id: string): Promise<Result> {
    const c = await pool.connect();
    try {
        await c.query('BEGIN ISOLATION LEVEL REPEATABLE READ');
        const g = (await c.query('SELECT * FROM games WHERE id=$1', [id])).rows[0];
        if (!g) { await c.query('ROLLBACK'); return { data: null, error: { code: 'PGRST116', message: 'no rows' } }; }
        const ph = (await c.query('SELECT player_id FROM player_hands WHERE game_id=$1', [id])).rows;
        const bh = (await c.query(
            `SELECT bh.bot_id, jsonb_build_object('strategy_key', b.strategy_key) AS bots
             FROM bot_hands bh JOIN bots b ON b.id = bh.bot_id WHERE bh.game_id=$1`, [id])).rows;
        await c.query('COMMIT');
        return ok({ ...g, player_hands: ph, bot_hands: bh });
    } catch (e) {
        try { await c.query('ROLLBACK'); } catch { /* */ }
        return { data: null, error: e };
    } finally { c.release(); }
}

interface Filter { col: string; op: 'eq' | 'in' | 'lt'; val: any }

class QueryBuilder implements PromiseLike<Result> {
    private filters: Filter[] = [];
    private selectCols = '*';
    private op: 'select' | 'insert' | 'upsert' | 'update' | 'delete' = 'select';
    private rows: any[] = [];
    private upsertOpts: { onConflict?: string; ignoreDuplicates?: boolean } = {};
    private orders: { col: string; asc: boolean }[] = []; private limitN?: number;
    private wantSingle = false;

    constructor(private table: string) {}

    select(cols = '*') { this.selectCols = cols; if (this.op === 'select') this.op = 'select'; return this; }
    insert(rows: any) { this.op = 'insert'; this.rows = Array.isArray(rows) ? rows : [rows]; return this; }
    upsert(rows: any, opts: any = {}) { this.op = 'upsert'; this.rows = Array.isArray(rows) ? rows : [rows]; this.upsertOpts = opts; return this; }
    update(row: any) { this.op = 'update'; this.rows = [row]; return this; }
    delete() { this.op = 'delete'; return this; }
    eq(col: string, val: any) { this.filters.push({ col, op: 'eq', val }); return this; }
    in(col: string, val: any[]) { this.filters.push({ col, op: 'in', val }); return this; }
    lt(col: string, val: any) { this.filters.push({ col, op: 'lt', val }); return this; }
    // supabase-js appends on repeated .order() calls; mirror that
    order(col: string, opts: any = {}) { this.orders.push({ col, asc: opts.ascending !== false }); return this; }
    limit(n: number) { this.limitN = n; return this; }
    single() { this.wantSingle = true; return this; }
    maybeSingle() { this.wantSingle = true; return this; }

    private where(params: any[]): string {
        if (this.filters.length === 0) return '';
        const parts = this.filters.map((f) => {
            if (f.op === 'in') { params.push(f.val); return `${f.col} = ANY($${params.length})`; }
            params.push(f.val);
            return f.op === 'lt' ? `${f.col} < $${params.length}` : `${f.col} = $${params.length}`;
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
                if (this.orders.length > 0) sql += ` ORDER BY ${this.orders.map((o) => `${o.col} ${o.asc ? 'ASC' : 'DESC'}`).join(', ')}`;
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
            if (this.op === 'update') {
                const params: any[] = [];
                const row = this.rows[0] ?? {};
                const sets = Object.keys(row).map((col) => {
                    const v = row[col];
                    params.push(v !== null && typeof v === 'object' ? JSON.stringify(v) : v);
                    return `${col} = $${params.length}`;
                });
                if (sets.length === 0) return ok([]);
                const sql = `UPDATE ${this.table} SET ${sets.join(', ')}${this.where(params)}${this.selectCols !== '*' ? ` RETURNING ${this.selectCols}` : ''}`;
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

// The server now reaches Realtime by POSTing { messages: [{topic,event,payload}] }
// to /realtime/v1/api/broadcast in ONE batched request (see broadcastMessages in
// _shared/utils.ts) rather than per-recipient channel.send(). There's no Realtime
// server here, so intercept that POST and record each message into broadcastLog —
// the same recorder, moved to the HTTP layer where Realtime now lives. Each
// message becomes one log entry (channel = its topic), so tests see exactly what
// they saw under the old channel.send() shim. Non-broadcast fetches pass through.
const _bcastLatency = Number(process.env.E2E_BCAST_LATENCY_MS || 0);
const _realFetch = globalThis.fetch;
globalThis.fetch = (async (input: any, init?: any): Promise<Response> => {
    const url = typeof input === 'string' ? input : (input?.url ?? '');
    if (url.includes('/realtime/v1/api/broadcast') && init?.method === 'POST') {
        if (_bcastLatency) await new Promise((r) => setTimeout(r, _bcastLatency));
        try {
            const { messages } = JSON.parse(init.body);
            for (const m of messages ?? []) {
                broadcastLog.push({ channel: m.topic, event: m.event, payload: m.payload, at: Date.now() });
            }
        } catch { /* malformed body — record nothing, mirror a server-side reject */ }
        return new Response(null, { status: 202 });
    }
    if (_realFetch) return _realFetch(input, init);
    throw new Error(`e2e: unexpected fetch to ${url}`);
}) as typeof fetch;

export const createClient = (_url?: string, _key?: string) => ({
    from: (table: string) => new QueryBuilder(table),
    rpc: async (name: string, params: Record<string, any> = {}): Promise<Result> => {
        try {
            const keys = Object.keys(params);
            // Named-argument call, like PostgREST: defaulted params may be
            // omitted and the caller's key order can't silently misbind.
            const placeholders = keys.map((k, i) => `${k} => $${i + 1}`).join(',');
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
