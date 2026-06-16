// Targeted bot/lock diagnostics for the "bots slow / 5-minute deadlock"
// investigation. Emits one structured, greppable line to the function's stdout,
// which Supabase already ships to the SQL-queryable Edge Functions logs — no
// bespoke table needed. Query with the `[DIAG]` marker (see below). TEMPORARY;
// remove once root-caused.
//
// Line format (stable, easy to filter/parse):
//   [DIAG] <TAG> game=<id> req=<id> <json-data>
//
// Tags (the theory each serves):
//   T1_BATON       bot-loop baton (bot_locks) acquire/leak/release events
//   T2_GAMELOCK    per-op game_locks contention / stale-delete / final failure
//   T3_PACE        per-decision compute time vs the artificial inter-bot delay
//   T4_NOPROGRESS  eligible bots exist but none could move (logic-level stall)
//   T5_BROADCAST   realtime broadcast latency / errors (perceived freeze)
//   T6_DELAYGLOBAL the cross-game module-global currentBotDelay being clobbered
//   T7_GHOSTHUMAN  loop ends mid-game waiting on a (possibly gone) human
//   T9_RUNAWAY     a single bot decision overran the 10s game_lock stale window
//
// Supabase Logs Explorer (Edge Functions source):
//   select timestamp, event_message
//   from function_edge_logs cross join unnest(metadata) as m
//   where event_message like '%[DIAG]%'
//   order by timestamp desc limit 1000;

export type DiagTag =
    | 'T1_BATON' | 'T2_GAMELOCK' | 'T3_PACE' | 'T4_NOPROGRESS'
    | 'T5_BROADCAST' | 'T6_DELAYGLOBAL' | 'T7_GHOSTHUMAN' | 'T9_RUNAWAY';

// Kept async + awaited at the call sites so wiring is uniform, but it does no I/O
// (console.log is synchronous) — so it adds no latency inside the game lock and
// never throws.
export const botDiag = async (
    game_id: string | null,
    tag: DiagTag,
    data: Record<string, unknown>,
    reqId?: string,
): Promise<void> => {
    try {
        console.log(`[DIAG] ${tag} game=${game_id ?? '-'} req=${reqId ?? '-'} ${JSON.stringify(data)}`);
    } catch (_e) {
        // Diagnostics must never affect the game.
    }
};
