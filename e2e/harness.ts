// Tiny harness around the REAL deployed server code. It only does three things
// the platform would otherwise do: provide Deno env/globals, seed a game row, and
// reset the DB. Everything gameplay-related goes through the genuine _shared
// modules (executeWithGameLock, the action handlers, commit_game, the bot lease).

// Deno globals the server modules read at import/runtime.
(globalThis as any).Deno = (globalThis as any).Deno || { env: { get: (k: string) => process.env[k] || 'x' } };
(globalThis as any).EdgeRuntime = (globalThis as any).EdgeRuntime || { waitUntil: (_p: Promise<unknown>) => {} };

// The real handlers / bot code log play-by-play; silence the gameplay chatter so
// test output stays readable (assertions still surface failures).
if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

import { randomUUID } from 'crypto';
import { readFileSync } from 'fs';
import { join } from 'path';
import { e2ePool as pool, resetBroadcastLog } from './adapters/supabase.ts';

export { broadcastLog, resetBroadcastLog } from './adapters/supabase.ts';
export const uuid = () => randomUUID();

// Stand up the Supabase platform shim, then apply the REAL production schema
// (server/impls/supabase/seed.sql — tables, types, the commit_game CAS, the bot lease, the
// triggers) verbatim. seed.sql is the single source of truth; nothing about the
// gameplay schema is copied here, so the harness can't drift from production.
export async function applySchema(): Promise<void> {
    const shim = readFileSync(join(process.cwd(), 'e2e', 'schema.sql'), 'utf8');
    await pool.query(shim);

    const seed = readFileSync(join(process.cwd(), 'server', 'impls', 'supabase', 'seed.sql'), 'utf8');
    await pool.query(seed);
}

export async function resetDb(): Promise<void> {
    await pool.query('TRUNCATE games, game_decks, player_hands, bot_hands, bots, game_snapshots, user_elo_ratings, player_views RESTART IDENTITY CASCADE');
    await pool.query('TRUNCATE auth.users CASCADE');
    resetBroadcastLog();
}

export interface SeedPlayer { id: string; name: string; is_ai: boolean; strategy_key: string }

// Seed a fresh WAITING game with the given players (lobby state, empty hands).
export async function seedGame(gameId: string, players: SeedPlayer[]): Promise<void> {
    const c = await pool.connect();
    try {
        await c.query('BEGIN');
        for (const p of players) {
            if (p.is_ai) await c.query('INSERT INTO bots(id,nickname,strategy_key) VALUES($1,$2,$3) ON CONFLICT (id) DO NOTHING', [p.id, p.name, p.strategy_key]);
            else await c.query('INSERT INTO auth.users(id) VALUES($1) ON CONFLICT DO NOTHING', [p.id]);
        }
        const playersJson = players.map((p) => ({ player_id: p.id, name: p.name, status: 'ready', is_ai: p.is_ai, hand_length: 0, strategy_key: p.strategy_key }));
        await c.query(
            `INSERT INTO games(id,name,players,status,power_suit,first_attacker,defender,version)
             VALUES($1,$2,$3,'waiting',0,0,0,0)`, [gameId, `${gameId}`, JSON.stringify(playersJson)]);
        await c.query('INSERT INTO game_decks(game_id,deck) VALUES($1,$2)', [gameId, JSON.stringify([])]);
        for (const p of players) {
            if (p.is_ai) await c.query('INSERT INTO bot_hands(game_id,bot_id,hand) VALUES($1,$2,$3)', [gameId, p.id, JSON.stringify([])]);
            else await c.query('INSERT INTO player_hands(game_id,player_id,hand) VALUES($1,$2,$3)', [gameId, p.id, JSON.stringify([])]);
        }
        await c.query('COMMIT');
    } catch (e) { await c.query('ROLLBACK'); throw e; } finally { c.release(); }
}

export const pgPool = pool;
