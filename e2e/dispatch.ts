// Drive games with the REAL deployed bot code: calculateLegalMoves (the same
// enumeration the bot loop uses) and executeBotMove (the same LegalMove ->
// handler dispatch). The only test glue is iterating players and a card-counting
// assertion that reads the durable DB state directly.

import { Game, AnimationEvent, PLAYER_STATUS } from '../supabase/functions/_shared/core/types.ts';
import { LegalMove } from '../supabase/functions/_shared/core/bot_interfaces.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/common/bot_strategy.ts';
import { executeBotMove } from '../supabase/functions/_shared/common/pure_bot_actions.ts';
import { pgPool } from './harness.ts';

export interface PlayerMove { playerId: string; move: LegalMove }

// Every legal move for every in-play player, via the REAL enumeration.
export function legalMovesFor(game: Game, allow?: (playerId: string) => boolean): PlayerMove[] {
    const out: PlayerMove[] = [];
    for (const p of game.players) {
        if (p.status !== PLAYER_STATUS.IN) continue;
        if (allow && !allow(p.player_id)) continue;
        for (const move of calculateLegalMoves(game, p.player_id)) {
            if (move.type === 'wait') continue;
            out.push({ playerId: p.player_id, move });
        }
    }
    return out;
}

// Apply via the REAL executeBotMove (returns false on a validation race, which
// commits as a harmless no-op — exactly how the live bot loop tolerates races).
export function applyPlayerMove(game: Game, pm: PlayerMove): AnimationEvent[] {
    const player = game.players.find((p) => p.player_id === pm.playerId)!;
    const ev = executeBotMove(game, player, pm.move);
    return ev || [];
}

// Card conservation, read straight from the committed DB (independent of the
// server's in-memory view): deck + flipped + hands + table + discard == full deck,
// no duplicates, no card-backs.
const key = (c: { suit: number; value: number }) => `${c.suit}:${c.value}`;
export async function checkCardConservation(gameId: string): Promise<{ ok: boolean; detail: string }> {
    const g = (await pgPool.query('SELECT players, table_battles, discard_pile_length, flipped, state FROM games WHERE id=$1', [gameId])).rows[0];

    const live: { suit: number; value: number }[] = [];
    // Deck + per-seat hands live in the packed kernel blob once the game is
    // dealt (commit_game no longer writes the hand/deck tables during play —
    // see commitGame). Reconstruct from the blob; fall back to the tables for
    // never-dealt / legacy rows that predate the blob column.
    if (g.state) {
        const { deserializeGameState } = await import('../supabase/functions/_shared/sdk/ts/wasm/engine.ts');
        const { hexToBytes } = await import('../supabase/functions/_shared/common/replay/codec.ts');
        const game = deserializeGameState(hexToBytes(g.state), {
            id: gameId, name: '', version: 0, deck_length: 0,
            players: (g.players ?? []).map((p: any) => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai, strategy_key: 'human' })),
            good_players: [], good_timestamp: null,
        });
        for (const c of game.deck) live.push(c);
        for (const p of game.players) for (const c of p.hand) live.push(c);
    } else {
        const deck = (await pgPool.query('SELECT deck FROM game_decks WHERE game_id=$1', [gameId])).rows[0]?.deck ?? [];
        const ph = (await pgPool.query('SELECT hand FROM player_hands WHERE game_id=$1', [gameId])).rows;
        const bh = (await pgPool.query('SELECT hand FROM bot_hands WHERE game_id=$1', [gameId])).rows;
        for (const c of deck) live.push(c);
        for (const r of [...ph, ...bh]) for (const c of (r.hand ?? [])) live.push(c);
    }
    if (g.flipped) live.push(g.flipped);
    for (const b of (g.table_battles ?? [])) { live.push(b.attack); if (b.defense) live.push(b.defense); }

    const seen = new Map<string, number>();
    let backs = 0;
    for (const c of live) { if (c.suit === -1) backs++; seen.set(key(c), (seen.get(key(c)) ?? 0) + 1); }
    const dupes = [...seen.entries()].filter(([, n]) => n > 1).map(([k, n]) => `${k}x${n}`);
    const expected = (g.players?.length ?? 0) >= 6 ? 52 : 36;
    const total = live.length + (g.discard_pile_length ?? 0);
    const okFlag = total === expected && dupes.length === 0 && backs === 0;
    return { ok: okFlag, detail: `total=${total}/${expected} live=${live.length} discard=${g.discard_pile_length}${dupes.length ? ` DUP[${dupes.join(',')}]` : ''}${backs ? ` BACKS=${backs}` : ''}` };
}
