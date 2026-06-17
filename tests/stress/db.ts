// DB layer for the stress harness. This is a faithful re-implementation of the
// production load/commit path (supabase/functions/_shared/utils.ts) but talking
// to a local Postgres via `pg` instead of PostgREST. The crucial part — the
// optimistic-concurrency fence — is NOT re-implemented here: commitGame() calls
// the REAL commit_game() plpgsql RPC running in a REAL Postgres, so the actual
// CAS arbitration is exactly the production code.

import { Pool } from 'pg';
import {
  Card, Game, PrivatePlayer, PublicGame, GAME_STATUS, STRATEGY_KEY,
} from '../../supabase/functions/_shared/types.ts';
import { other_player } from '../../supabase/functions/_shared/common_utils.ts';

export const pool = new Pool({
  host: '127.0.0.1', port: 5432, user: 'stress', password: 'stress', database: 'foolish',
  max: 50,
});

// --- gameToPublicGame: verbatim from utils.ts ------------------------------
export const gameToPublicGame = (game: Game): PublicGame => ({
  id: game.id,
  name: game.name,
  deck_length: game.deck.length,
  discard_pile_length: game.discard_pile_length,
  flipped: game.flipped,
  players: game.players.map(other_player),
  status: game.status,
  power_suit: game.power_suit,
  first_attacker: game.first_attacker,
  defender: game.defender,
  table_battles: game.table_battles,
  elimination_order: game.elimination_order,
  good_timestamp: game.good_timestamp,
  good_players: game.good_players,
});

// --- loadCompleteGame: mirrors utils.ts (minus the log-session filtering, which
//     doesn't affect card state) ------------------------------------------------
export const loadCompleteGame = async (gameId: string): Promise<Game> => {
  const c = await pool.connect();
  try {
    // Production's loadCompleteGame is ONE PostgREST select with embedded joins —
    // i.e. a single, consistent MVCC snapshot. We split it across 4 statements, so
    // we must wrap them in a REPEATABLE READ transaction to get the same
    // single-snapshot semantics; otherwise a commit_game landing mid-read would be
    // a torn read (a harness artifact, not a server bug).
    await c.query('BEGIN ISOLATION LEVEL REPEATABLE READ');
    const g = (await c.query('SELECT * FROM games WHERE id=$1', [gameId])).rows[0];
    if (!g) throw new Error(`Game ${gameId} not found`);
    const ph = (await c.query('SELECT player_id, hand, awaiting_attack FROM player_hands WHERE game_id=$1', [gameId])).rows;
    const bh = (await c.query('SELECT bot_id, hand, awaiting_attack FROM bot_hands WHERE game_id=$1', [gameId])).rows;
    const deckRow = (await c.query('SELECT deck FROM game_decks WHERE game_id=$1', [gameId])).rows[0];
    await c.query('COMMIT');

    const players: PrivatePlayer[] = (g.players as any[]).map((player) => {
      let hand: Card[], awaiting_attack: boolean, strategy_key: string;
      if (player.is_ai) {
        const row = bh.find((h) => h.bot_id === player.player_id)!;
        hand = row.hand; awaiting_attack = row.awaiting_attack; strategy_key = player.strategy_key ?? 'bot';
      } else {
        const row = ph.find((h) => h.player_id === player.player_id)!;
        hand = row.hand; awaiting_attack = row.awaiting_attack; strategy_key = STRATEGY_KEY.HUMAN;
      }
      return {
        player_id: player.player_id, name: player.name, status: player.status,
        is_ai: player.is_ai, hand, awaiting_attack, hand_length: hand.length, strategy_key,
      } as PrivatePlayer;
    });

    return {
      id: g.id, version: Number(g.version ?? 0), name: g.name,
      deck: deckRow?.deck ?? [], deck_length: (deckRow?.deck ?? []).length,
      discard_pile_length: g.discard_pile_length, flipped: g.flipped,
      players, status: g.status, power_suit: g.power_suit,
      first_attacker: g.first_attacker, defender: g.defender,
      table_battles: g.table_battles, elimination_order: g.elimination_order,
      good_timestamp: g.good_timestamp == null ? null : Number(g.good_timestamp),
      good_players: g.good_players ?? [], logs: [],
    };
  } catch (e) {
    try { await c.query('ROLLBACK'); } catch { /* already aborted */ }
    throw e;
  } finally {
    c.release();
  }
};

// --- commitGame: mirrors utils.ts; calls the REAL commit_game RPC -----------
export const commitGame = async (
  game: Game, expectedVersion: number,
): Promise<{ status: 'ok' | 'conflict'; version?: number }> => {
  const publicGame = gameToPublicGame(game);
  const humanHands = game.players.filter((p) => !p.is_ai).map((p) => ({
    player_id: p.player_id, hand: p.hand, awaiting_attack: p.awaiting_attack,
  }));
  const botHands = game.players.filter((p) => p.is_ai).map((p) => ({
    bot_id: p.player_id, hand: p.hand, awaiting_attack: p.awaiting_attack,
  }));

  const res = (await pool.query(
    'SELECT commit_game($1,$2,$3,$4,$5,$6) AS r',
    [game.id, expectedVersion, JSON.stringify(publicGame), JSON.stringify(game.deck),
      JSON.stringify(humanHands), JSON.stringify(botHands)],
  )).rows[0].r as { status: 'ok' | 'conflict'; version?: number };

  if (res.status === 'ok' && typeof res.version === 'number') game.version = res.version;
  return res;
};

export const tryAcquireBotLease = async (gameId: string, ttlMs: number): Promise<string | null> =>
  (await pool.query('SELECT try_acquire_bot_lease($1,$2) AS t', [gameId, ttlMs])).rows[0].t ?? null;

export const releaseBotLease = async (gameId: string, token: string): Promise<void> => {
  await pool.query('SELECT release_bot_lease($1,$2)', [gameId, token]);
};

export const renewBotLease = async (gameId: string, token: string, ttlMs: number): Promise<boolean> =>
  (await pool.query('SELECT renew_bot_lease($1,$2,$3) AS r', [gameId, token, ttlMs])).rows[0].r;

// --- seed a fresh waiting game with the given players ----------------------
export interface SeedPlayer { id: string; name: string; is_ai: boolean; strategy_key: string; }

export const seedGame = async (gameId: string, players: SeedPlayer[]): Promise<void> => {
  const c = await pool.connect();
  try {
    await c.query('BEGIN');
    // bots + auth.users rows the FKs need
    for (const p of players) {
      if (p.is_ai) {
        await c.query(
          'INSERT INTO bots(id,nickname,strategy_key) VALUES($1,$2,$3) ON CONFLICT (id) DO NOTHING',
          [p.id, p.name, p.strategy_key]);
      } else {
        await c.query('INSERT INTO auth.users(id) VALUES($1) ON CONFLICT DO NOTHING', [p.id]);
      }
    }
    const playersJson = players.map((p) => ({
      player_id: p.id, name: p.name, status: 'ready', is_ai: p.is_ai,
      hand_length: 0, strategy_key: p.strategy_key,
    }));
    await c.query(
      `INSERT INTO games(id,name,players,status,power_suit,first_attacker,defender,version)
       VALUES($1,$2,$3,'waiting',0,0,0,0)`,
      [gameId, `${gameId} stress game`, JSON.stringify(playersJson)]);
    await c.query('INSERT INTO game_decks(game_id,deck) VALUES($1,$2)', [gameId, JSON.stringify([])]);
    for (const p of players) {
      if (p.is_ai) {
        await c.query('INSERT INTO bot_hands(game_id,bot_id,hand) VALUES($1,$2,$3)', [gameId, p.id, JSON.stringify([])]);
      } else {
        await c.query('INSERT INTO player_hands(game_id,player_id,hand) VALUES($1,$2,$3)', [gameId, p.id, JSON.stringify([])]);
      }
    }
    await c.query('COMMIT');
  } catch (e) {
    await c.query('ROLLBACK'); throw e;
  } finally {
    c.release();
  }
};

export const resetDb = async (): Promise<void> => {
  await pool.query('TRUNCATE games, game_decks, player_hands, bot_hands, game_logs, bots RESTART IDENTITY CASCADE');
  await pool.query('TRUNCATE auth.users CASCADE');
};

export { GAME_STATUS };
