import { kernelB32Encode, replaySummary } from '@sdk/ts/wasm/bots.ts';
import { decodeExtras, joinReplayCode } from '@api/common/replay_extras.ts';
import { bigintToBytes, bytesToBigint, hexToBytes } from '@api/common/replay_codec.ts';

/**
 * One game_snapshots row, read into the line the match history shows for it.
 * No React and no network here, so the screen's loader is a query and a loop,
 * and this can be asked directly.
 */

/** A game_snapshots row as the history query selects it. */
export interface HistorySnapshotRow {
    id: string;
    player_ids: string[] | null;
    moves: string;          // bytea as PostgREST sends it: hex, '\x'-prefixed
    extras: string | null;
    created_at: string;
}

export interface HistoryEntry {
    id: string;
    code: string;           // base32 moves[-extras] - the replay URL path
    createdAt: Date;
    playerCount: number;
    names: string[];        // seat order, '%'-prefixed for bots
    mySeat: number;
    myRank: number;         // 1 = first out (best), playerCount = the fool
    foolSeat: number;
    durationSec: number | null;
}

// game_snapshots.created_at is TIMESTAMP WITHOUT TIME ZONE holding UTC wall
// time; PostgREST serializes it with no offset, and new Date() would read
// that as LOCAL time. Pin it to UTC unless an offset is already present.
const parseUtcTimestamp = (ts: string): Date =>
    new Date(/[zZ]|[+-]\d\d:?\d\d$/.test(ts) ? ts : ts + 'Z');

/**
 * The history line for `row` as `userId` played it, or null when they hold no
 * seat in it. Throws when the snapshot does not decode (corrupt / future
 * format); extras that do not decode only cost the names and the duration.
 * The kernel has to be warm (ensureBotsAsync) before this is called.
 */
export function decodeHistoryRow(row: HistorySnapshotRow, userId: string): HistoryEntry | null {
    const playerIds = row.player_ids ?? [];
    const mySeat = playerIds.indexOf(userId);
    if (mySeat < 0) return null; // cheap check before the expensive decode

    const movesBytes = hexToBytes(row.moves);
    const extrasBytes = row.extras ? hexToBytes(row.extras) : null;
    const extrasCode = extrasBytes && extrasBytes.length > 0
        ? kernelB32Encode(extrasBytes)
        : null;
    const d = replaySummary(bigintToBytes(bytesToBigint(movesBytes)));
    if (!d) throw new Error('the snapshot does not decode');

    let names: string[] | null = null;
    let durationSec: number | null = null;
    if (extrasCode) {
        try {
            const extras = decodeExtras(extrasCode, d.numPlayers, d.moves);
            names = extras.names;
            if (extras.moveGaps && extras.moveGaps.length > 0)
                durationSec = extras.moveGaps.reduce((a, b) => a + b, 0);
        } catch (e) {
            console.error('History extras ignored:', e);
        }
    }

    // The fool is never in the elimination order (ReplaySummary),
    // so the fallback IS the fool's last place.
    const rankIndex = d.elimination.indexOf(mySeat);
    const myRank = rankIndex >= 0 ? rankIndex + 1 : d.numPlayers;

    return {
        id: row.id,
        code: joinReplayCode(kernelB32Encode(movesBytes), extrasCode),
        createdAt: parseUtcTimestamp(row.created_at),
        playerCount: d.numPlayers,
        names: names ?? playerIds.map((_, seat) => `#${seat + 1}`),
        mySeat,
        myRank,
        foolSeat: d.fool,
        durationSec,
    };
}
