// finalize.ts - the one-time side effects of a finished game
// (docs/C_GAME_SHAPE_MIGRATION.md 2.8, Phase 4b).
//
// Run by table_io.runTableOp and the bot loop AFTER the commit that ended the
// game, so it fires exactly once: only the winning CAS commit reaches it. Two
// writes, both I/O: the replay snapshot (game_snapshots) and the retired session
// log (games.logs_packed). Everything they carry is the kernel's: the verified
// v6 replay code (table_replay_code, with its round-trip gate) and the extras
// blob (table_replay_extras: names from the roster, move times from the log).
// This module reads the columns, hands them over, and writes the results back.
//
// THE RATINGS ARE NOT HERE, and they used to be: a third write that read each
// seat's rating, added the kernel's change and stored the sum, after the game
// had already been committed as over. It was unfenced, so two games sharing a
// seat that ended together lost one game's change; it could fail on its own, so
// a game could end and never be rated; and what it left behind was each
// PLAYER's last change, which the finish screen then showed as the game's. The
// kernel rates the game inside the commit that ends it now (table_rate, the
// stamp in the state blob, commit_table's rated arguments).

import { serverTable, tableCodeName, type ServerTable, type TableSeat } from '@sdk/ts/table/server_table.ts';
import { bytesToColumnHex, columnHexToBytes } from './table_io.ts';
import { supabaseClient } from './utils.ts';

/** `state` and `roster` are the ending commit's products. */
export async function finalizeEndedGame(gameId: string, state: Uint8Array, roster: Uint8Array, reqId = 'finalize'): Promise<void> {
    const table = await serverTable();

    // ---- kernel section: who sat where ----
    const rc = table.load(state, roster);
    if (rc < 0) {
        console.error(`[${reqId}][FINALIZE] game ${gameId} does not load (${tableCodeName(rc, ['TABLE_E_', 'GAME_INVALID_'])}); nothing finalized`);
        return;
    }
    const seats = table.seats();
    // ---- end of the kernel section ----

    await writeReplaySnapshot(table, gameId, state, roster, seats, reqId);
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
