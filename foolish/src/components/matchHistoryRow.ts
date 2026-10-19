import type { NetHistoryRow } from '@sdk/ts/net/net_client.ts';

/**
 * One finished game, read into the line the match history shows for it. The
 * server cut the summary when the game ended (no replay is sent with the list:
 * a replay is fetched when its row is tapped) and reads the seats' names out
 * of the replay's extras when it answers the list.
 */

export interface HistoryEntry {
    id: string;             // the replay's: GET /v1/replay/<id>
    createdAt: Date;
    playerCount: number;
    names: string[];        // seat order, '%'-prefixed for bots; '#<seat>' when the replay holds no names
    mySeat: number;
    myRank: number;         // 1 = first out (best), playerCount = the fool
    foolSeat: number;
    durationSec: number | null;
}

/** The history line for `row`, or null for a row with no summary (cut before summaries existed). */
export function historyEntry(row: NetHistoryRow): HistoryEntry | null {
    if (row.rank === 0 || row.seat === NONE) return null;
    return {
        id: row.id,
        createdAt: new Date(row.createdMs),
        playerCount: row.nPlayers,
        names: row.names.length === row.nPlayers ? row.names : Array.from({ length: row.nPlayers }, (_, seat) => `#${seat + 1}`),
        mySeat: row.seat,
        myRank: row.rank,
        foolSeat: row.fool === NONE ? -1 : row.fool,
        durationSec: row.durationS === NO_TIME ? null : row.durationS,
    };
}

const NONE = 255;
const NO_TIME = 0xffffffff;
