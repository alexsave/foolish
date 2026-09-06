// THE FINISH ORDER, answered once by the kernel.
//
// Rank 1 is the first seat out, counting up to the fool last - whose place is
// the SEAT count, not the row count. That rule lives in
// c/src/anim_plan.c anim_finish_rows and reaches here through
// sdk/ts/wasm/bots.ts animFinishRows; this module only maps seats to
// player_ids either side of the call.
//
// It is NOT in common_utils.ts on purpose: that module is shared with the
// client and its header forbids a static kernel import. bots.wasm is already
// loaded by both hosts that need this (the edge, and the browser through
// providers.tsx ensureBotsAsync), so the kernel is reachable from here for
// free.
//
// No dedup. Both TS rankers used to launder elimination_order through a Set()
// "to handle backend bugs"; the bug was the round-end append in
// c/src/game.c that could record a seat the refill had already eliminated,
// it is fixed at the source by the was_in guard, and
// c/tests/tests.c test_elimination_order_never_repeats_a_seat holds it there.
import { animFinishRows } from '@sdk/ts/wasm/bots.ts';

interface FinishPlayer { player_id: string }
interface FinishGame {
    players: FinishPlayer[];
    elimination_order: string[];
}

/** One player's place on the end screen. `place` is 1-based and counts to the
 *  seat count; `isYou` is set for the viewer's own row. */
export interface FinishPlace {
    player_id: string;
    place: number;
    isYou: boolean;
}

/** The end screen's rows, kernel-ordered. `mySeat` may be negative (or the
 *  argument omitted) for a spectator, who owns no row. */
export function gameFinishPlaces(game: FinishGame, mySeat = -1): FinishPlace[] {
    const seatOf = (id: string) => game.players.findIndex((p) => p.player_id === id);
    const elimSeats = game.elimination_order.map(seatOf).filter((s) => s >= 0);
    // The fool is the one seat the elimination list does not name. The kernel
    // takes it as an argument rather than deriving it, because the caller is
    // the one that knows whether the game is over at all: a negative seat means
    // still running, and no last-place row is emitted.
    const foolSeat = game.players.findIndex((_, s) => !elimSeats.includes(s));
    return animFinishRows(elimSeats, foolSeat, game.players.length, mySeat)
        .map((r) => ({
            player_id: game.players[r.seat].player_id,
            place: r.place,
            isYou: r.isYou,
        }));
}

/** The finish order as bare player_ids, best first. The shape the ELO pass
 *  wants. */
export function calculateGameRankings(game: FinishGame): string[] {
    return gameFinishPlaces(game).map((r) => r.player_id);
}
