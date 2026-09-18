// finalize.ts - the one-time side effects of a finished game
// (docs/C_GAME_SHAPE_MIGRATION.md 2.8, Phase 4b).
//
// Run by table_io.runTableOp and the bot loop AFTER the commit that ended the
// game, so it fires exactly once: only the winning CAS commit reaches it. Three
// writes, all I/O: the replay snapshot (game_snapshots), the retired session log
// (games.logs_packed), and the rating upserts. Everything they carry is the
// kernel's: the verified v6 replay code (table_replay_code, with its round-trip
// gate), the extras blob (table_replay_extras: names from the roster, move times
// from the log), the finish order (table_rankings) and every rating change
// (elo_deltas). This module reads the columns, hands them over, and writes the
// results back by the seat ids the roster names.

import { serverTable, tableCodeName, type ServerTable, type TableSeat } from '@sdk/ts/table/server_table.ts';
import { bytesToColumnHex, columnHexToBytes } from './table_io.ts';
import { supabaseClient } from './utils.ts';

const BASE_RATING = 1000;

/** `state` and `roster` are the ending commit's products. */
export async function finalizeEndedGame(gameId: string, state: Uint8Array, roster: Uint8Array, reqId = 'finalize'): Promise<void> {
    const table = await serverTable();

    // ---- kernel section: who sat where, and the finish order ----
    const rc = table.load(state, roster);
    if (rc < 0) {
        console.error(`[${reqId}][FINALIZE] game ${gameId} does not load (${tableCodeName(rc, ['TABLE_E_', 'GAME_INVALID_'])}); nothing finalized`);
        return;
    }
    const seats = table.seats();
    const order = table.rankings();
    // ---- end of the kernel section ----

    // ELO and the replay snapshot touch disjoint tables: run them together.
    const elo = updateRatings(table, gameId, state, roster, seats, order, reqId);
    await writeReplaySnapshot(table, gameId, state, roster, seats, reqId);
    await elo;
}

async function writeReplaySnapshot(
    table: ServerTable, gameId: string, state: Uint8Array, roster: Uint8Array, seats: TableSeat[], reqId: string,
): Promise<void> {
    try {
        const { data, error } = await supabaseClient
            .from('games').select('logs_packed, game_seed').eq('id', gameId).single();
        if (error) throw error;
        if (!data?.game_seed) throw new Error('no deal seed: nothing to encode a replay from');
        const log = data?.logs_packed ? columnHexToBytes(data.logs_packed) : new Uint8Array(0);
        if (log.length === 0) throw new Error('no session log: nothing to encode a replay from');
        const seed = columnHexToBytes(data.game_seed);

        // ---- kernel section: the extras, then the verified code ----
        let rc = table.load(state, roster);
        if (rc < 0) throw new Error(`does not load (${rc})`);
        const extras = table.replayExtras(log);
        if (typeof extras === 'number') throw new Error(`replay extras refused (${tableCodeName(extras, ['TABLE_E_'])})`);
        rc = table.load(state, roster);
        if (rc < 0) throw new Error(`does not load (${rc})`);
        const code = table.replayCode(seed, log);
        if (typeof code === 'number') throw new Error(`replay code refused (${tableCodeName(code, ['TABLE_E_', 'REPLAY_E'])}, ${code})`);
        // ---- end of the kernel section ----
        console.log(`[${reqId}][REPLAY] game ${gameId} encoded (v6) to ${code.length}+${extras.length} bytes`);

        // The snapshot BEFORE the log is retired, so a failure between the two
        // never loses both. player_ids is the snapshot's read ACL.
        const { error: snapError } = await supabaseClient.from('game_snapshots').insert({
            game_id: gameId,
            player_ids: seats.map((s) => s.id),
            moves: bytesToColumnHex(code),
            extras: bytesToColumnHex(extras),
        });
        if (snapError) throw snapError;
        const { error: retireError } = await supabaseClient.from('games').update({ logs_packed: '' }).eq('id', gameId);
        if (retireError) throw retireError;
    } catch (e) {
        // Never break the end of a game over the snapshot: the session log stays
        // as the record, replaced by the next session.
        console.error(`[${reqId}][REPLAY] snapshot failed for game ${gameId}, keeping the log:`, e);
    }
}

async function updateRatings(
    table: ServerTable, gameId: string, state: Uint8Array, roster: Uint8Array,
    seats: TableSeat[], order: number[], reqId: string,
): Promise<void> {
    if (seats.length < 2 || order.length !== seats.length) return;
    try {
        const botIds = seats.filter((s) => s.brain).map((s) => s.id);
        const humanIds = seats.filter((s) => !s.brain).map((s) => s.id);
        const rating = new Map<string, { elo_rating: number; games_played: number }>();
        const botRows = new Map<string, { nickname: string; strategy_key: string }>();
        if (botIds.length > 0) {
            const { data, error } = await supabaseClient
                .from('bots').select('id, elo_rating, games_played, nickname, strategy_key').in('id', botIds);
            if (error || !data || data.length !== botIds.length) throw new Error(`bot ratings: ${error?.message ?? `expected ${botIds.length} rows`}`);
            for (const r of data) { rating.set(r.id, r); botRows.set(r.id, r); }
        }
        if (humanIds.length > 0) {
            const { data, error } = await supabaseClient.from('user_elo_ratings').select('*').in('user_id', humanIds);
            if (error) throw new Error(`user ratings: ${error.message}`);
            const rows = (data ?? []) as { user_id: string; elo_rating: number; games_played: number }[];
            const have = new Map(rows.map((r) => [r.user_id, r] as const));
            for (const id of humanIds) rating.set(id, have.get(id) ?? { elo_rating: BASE_RATING, games_played: 0 });
        }

        // ---- kernel section: the rating changes by seat ----
        const loaded = table.load(state, roster);
        if (loaded < 0) throw new Error(`game ${gameId} does not load (${loaded})`);
        const deltas = table.eloDeltas(seats.map((s) => Number(rating.get(s.id)!.elo_rating)), order);
        // ---- end of the kernel section ----

        const next = (id: string, seat: number) => {
            const r = rating.get(id)!;
            // A rating never goes below zero: a column rule, not a game one.
            return { elo_rating: Math.max(0, r.elo_rating + deltas[seat]), previous_elo: r.elo_rating, games_played: r.games_played + 1 };
        };
        const humans = seats.flatMap((s, i) => (s.brain ? [] : [{ user_id: s.id, ...next(s.id, i) }]));
        const bots = seats.flatMap((s, i) => (s.brain
            ? [{ id: s.id, nickname: botRows.get(s.id)!.nickname, strategy_key: botRows.get(s.id)!.strategy_key, ...next(s.id, i) }]
            : []));
        if (humans.length > 0) {
            const { error } = await supabaseClient.from('user_elo_ratings').upsert(humans);
            if (error) throw error;
        }
        if (bots.length > 0) {
            const { error } = await supabaseClient.from('bots').upsert(bots).select();
            if (error) throw error;
        }
    } catch (e) {
        // Never break the end of a game over ratings.
        console.error(`[${reqId}][ELO] rating update failed for game ${gameId}:`, e);
    }
}
