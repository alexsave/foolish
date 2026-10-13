// ratings.ts - the seats' ratings, read for the kernel as a game ends.
//
// A finished game's rating changes are the kernel's (c/src/table.h table_rate):
// it works them out from the ratings it is handed, stamps them on the game, and
// names them in the ending commit's products, and commit_table applies them in
// that commit's transaction. All this module does is the one thing the kernel
// cannot: read the numbers. It decides nothing.
//
// The read happens BEFORE the commit and outside its transaction, so a rating
// here can be a moment old - a bot may have finished another table since. That
// is why the commit ADDS each change to the row instead of storing "what I read,
// plus my change" (seed.sql commit_table has the reasoning).

import type { ServerTable, TableSeat } from '@sdk/ts/table/server_table.ts';
import * as L from '@sdk/ts/gen/game_layout.bots.ts';
import { supabaseClient } from './utils.ts';

/**
 * Each seat's rating, by seat, or null when they cannot all be read. A game
 * whose ratings cannot be read still ENDS: it ends unrated, with no stamp and no
 * change, rather than not ending or being rated from a guess.
 */
export async function readRatings(seats: TableSeat[], reqId: string): Promise<number[] | null> {
    try {
        const botIds = seats.filter((s) => s.brain).map((s) => s.id);
        const humanIds = seats.filter((s) => !s.brain).map((s) => s.id);
        const rating = new Map<string, number>();
        if (botIds.length > 0) {
            const { data, error } = await supabaseClient.from('bots').select('id, elo_rating').in('id', botIds);
            if (error || !data || data.length !== botIds.length) throw new Error(`bot ratings: ${error?.message ?? `expected ${botIds.length} rows`}`);
            for (const r of data as { id: string; elo_rating: number }[]) rating.set(r.id, Number(r.elo_rating));
        }
        if (humanIds.length > 0) {
            const { data, error } = await supabaseClient.from('user_elo_ratings').select('user_id, elo_rating').in('user_id', humanIds);
            if (error) throw new Error(`user ratings: ${error.message}`);
            for (const r of (data ?? []) as { user_id: string; elo_rating: number }[]) rating.set(r.user_id, Number(r.elo_rating));
            // A human with no row has no rating yet. What they are rated from is
            // the kernel's to say (TABLE_RATING_START), not a number kept here.
            for (const id of humanIds) if (!rating.has(id)) rating.set(id, L.TABLE_RATING_NONE);
        }
        return seats.map((s) => rating.get(s.id)!);
    } catch (e) {
        console.error(`[${reqId}][ELO] the seats' ratings could not be read; the game ends unrated:`, e);
        return null;
    }
}

/**
 * SYNCHRONOUS, inside a kernel section, between the operation and its commit
 * products: hands the loaded table the ratings read for it. With no ratings
 * this does nothing. Whether there is a game to rate is the kernel's call
 * (table_rate answers TABLE_MOOT when there is not, as on a retry of the last
 * move that finds the row has moved on); only a malformed hand-over is negative.
 */
export function handRatings(table: ServerTable, ratings: number[] | null, gameId: string, reqId: string): void {
    if (!ratings) return;
    const rc = table.rate(ratings);
    if (rc < 0) console.error(`[${reqId}][ELO] game ${gameId}: the kernel refused the ratings (${rc}); the game ends unrated`);
}
