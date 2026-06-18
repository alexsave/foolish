// Dispatch a legal Move to the REAL action handler — the same handlers the
// deployed `action` edge function calls. Pure server code; no mock.
import { Game, AnimationEvent } from '../supabase/functions/_shared/types.ts';
import { handleAttack } from '../supabase/functions/_shared/actions/attack.ts';
import { handleCover } from '../supabase/functions/_shared/actions/cover.ts';
import { handlePass } from '../supabase/functions/_shared/actions/pass.ts';
import { handlePickup } from '../supabase/functions/_shared/actions/pickup.ts';
import { handleGood } from '../supabase/functions/_shared/actions/good.ts';
import { Move } from './moves.ts';

export const applyMove = (game: Game, move: Move): { game: Game; events: AnimationEvent[] } => {
    switch (move.type) {
        case 'attack': return { game, events: handleAttack(game, move.player_id, move.cards) };
        case 'cover': return { game, events: handleCover(game, move.player_id, move.cover_cards, move.attack_cards) };
        case 'pass': return { game, events: handlePass(game, move.player_id, move.cards) };
        case 'pickup': return { game, events: handlePickup(game, move.player_id) };
        case 'good': return { game, events: handleGood(game, move.player_id) };
        default: throw new Error('unknown move');
    }
};

// Read the durably-committed game and check the core invariant directly from the
// DB (independent of the server's in-memory view).
import { pgPool } from './harness.ts';
const key = (c: { suit: number; value: number }) => `${c.suit}:${c.value}`;
export async function checkCardConservation(gameId: string): Promise<{ ok: boolean; detail: string }> {
    const g = (await pgPool.query('SELECT players, table_battles, discard_pile_length FROM games WHERE id=$1', [gameId])).rows[0];
    const deck = (await pgPool.query('SELECT deck FROM game_decks WHERE game_id=$1', [gameId])).rows[0]?.deck ?? [];
    const ph = (await pgPool.query('SELECT hand FROM player_hands WHERE game_id=$1', [gameId])).rows;
    const bh = (await pgPool.query('SELECT hand FROM bot_hands WHERE game_id=$1', [gameId])).rows;
    const flipped = (await pgPool.query('SELECT flipped FROM games WHERE id=$1', [gameId])).rows[0]?.flipped;

    const live: { suit: number; value: number }[] = [];
    for (const c of deck) live.push(c);
    if (flipped) live.push(flipped);
    for (const r of [...ph, ...bh]) for (const c of (r.hand ?? [])) live.push(c);
    for (const b of (g.table_battles ?? [])) { live.push(b.attack); if (b.defense) live.push(b.defense); }

    const seen = new Map<string, number>();
    let backs = 0;
    for (const c of live) { if (c.suit === -1) backs++; seen.set(key(c), (seen.get(key(c)) ?? 0) + 1); }
    const dupes = [...seen.entries()].filter(([, n]) => n > 1).map(([k, n]) => `${k}x${n}`);
    const expected = (g.players?.length ?? 0) > 4 ? 52 : 36;
    const total = live.length + (g.discard_pile_length ?? 0);
    const okFlag = total === expected && dupes.length === 0 && backs === 0;
    return { ok: okFlag, detail: `total=${total}/${expected} live=${live.length} discard=${g.discard_pile_length}${dupes.length ? ` DUP[${dupes.join(',')}]` : ''}${backs ? ` BACKS=${backs}` : ''}` };
}
