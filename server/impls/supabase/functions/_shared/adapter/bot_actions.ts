// The bot loop (docs/C_GAME_SHAPE_MIGRATION.md 2.7): one leased drive segment
// for one game, cycle by cycle.
//
// Each cycle is two kernel sections around the one read the kernel cannot do:
//   A  load the table; the kernel says whether a bot has work and whether the
//      bot about to choose reads the session log (a belief brain)
//   -- await the session log (games.logs_packed), only when it does
//   B  load the table again (never trust the slot across the await), hand it the
//      deal seed and the session log, drive one cycle (table_bot_drive), copy out
//      every product, the pushes, the wait it is worth and the moves to offer a
//      retry
// then the version-fenced commit, the end of the game when the cycle ended it,
// the broadcast, and the wait. What stays here is what the kernel cannot do: the
// lease, the CAS commit, the broadcast, the CPU budget and the sleep.

import { serverTable, tableCodeName, type TableProducts, type TableSeat } from '@sdk/ts/table/server_table.ts';
import { broadcastPushes, columnHexToBytes, commitProducts, loadRow } from './table_io.ts';
import { supabaseClient } from './utils.ts';

const lazy = <T>(load: () => Promise<T>): (() => Promise<T>) => {
    let mod: Promise<T> | undefined;
    return () => (mod ??= load());
};
const finalizeMod = lazy(() => import('./finalize.ts'));

// One-line memory snapshot against the edge limits (150MB heap + 150MB
// external, where wasm linear memory counts as external).
const memLine = (wasmBytes: number): string => {
    let d = '';
    try {
        const m = (globalThis as { Deno?: { memoryUsage?: () => { rss: number; heapTotal: number; heapUsed: number; external: number } } }).Deno?.memoryUsage?.();
        if (m) d = `heap=${Math.round(m.heapUsed / 1048576)}/${Math.round(m.heapTotal / 1048576)}MB ext=${Math.round(m.external / 1048576)}MB rss=${Math.round(m.rss / 1048576)}MB `;
    } catch { /* memoryUsage unavailable */ }
    return `${d}botsWasm=${Math.round(wasmBytes / 1048576)}MB`;
};

// --- Adaptive CPU budgeting (instead of a hardcoded wall cap) ---------------
// Supabase caps CPU at ~2s per request (async I/O and sleeps don't count; the
// Monte-Carlo search does). The loop MEASURES each cycle's cost and bails when
// the PREDICTED next cycle would risk the cap.
const CPU_SOFT_BUDGET_MS = 1700;       // bail target - margin for non-bot CPU + tail
const CPU_PREDICT_FACTOR = 1.5;        // next-cycle estimate = avg * this (variance margin)

// Secondary WALL ceiling: a cheap loop still releases before the ~150s isolate
// wall-clock kill. The lease is renewed each cycle, so this is not bounded by it.
const WALL_CEILING_MS = 120_000;

// Bot-loop lease lifetime (games.bot_lease_*). Auto-expiring; renewed each cycle.
const BOT_LEASE_TTL_MS = 25_000;

const MAX_ATTEMPTS = 5;

const acquireBotLease = async (gameId: string): Promise<string | null> => {
    try {
        const { data, error } = await supabaseClient.rpc('try_acquire_bot_lease', { p_game_id: gameId, p_ttl_ms: BOT_LEASE_TTL_MS });
        if (error) { console.error(`Failed to acquire bot lease for ${gameId}:`, error); return null; }
        return (data as string | null) ?? null;
    } catch (error) {
        console.error(`Error acquiring bot lease for ${gameId}:`, error);
        return null;
    }
};

const releaseBotLease = async (gameId: string, token: string): Promise<void> => {
    try {
        await supabaseClient.rpc('release_bot_lease', { p_game_id: gameId, p_token: token });
    } catch (error) {
        console.error(`Error releasing bot lease for ${gameId}:`, error);
    }
};

// false when another loop took the lease over; a transport error keeps going.
const renewBotLease = async (gameId: string, token: string): Promise<boolean> => {
    try {
        const { data, error } = await supabaseClient.rpc('renew_bot_lease', { p_game_id: gameId, p_token: token, p_ttl_ms: BOT_LEASE_TTL_MS });
        if (error) { console.error(`Error renewing bot lease for ${gameId}:`, error); return true; }
        return data !== false;
    } catch (error) {
        console.error(`Error renewing bot lease for ${gameId}:`, error);
        return true;
    }
};

type CpuAcct = { computeMs: number; cycles: number; maxMs: number };

/** One drive segment. Continuation across segments is the heartbeat's (a fresh request, a fresh CPU budget). */
export const lockedBotLoop = async (gameId: string): Promise<void> => {
    const table = await serverTable();
    console.log(`[MEM] lockedBotLoop start: ${memLine(table.memoryBytes())}`);
    const lease = await acquireBotLease(gameId);
    if (!lease) {
        console.log('bot lease held by another loop, skipping');
        return;
    }
    const cpu: CpuAcct = { computeMs: 0, cycles: 0, maxMs: 0 };
    const started = Date.now();
    try {
        // Bots-only tables have no concurrent writer under the lease, so the
        // session log this segment read once stays exactly logs_packed if the
        // segment appends what it commits.
        let resident: Uint8Array | null = null;
        for (let cycle = 0; ; cycle++) {
            if (cycle > 0) {
                if (Date.now() - started > WALL_CEILING_MS) return;
                if (cpu.cycles > 0) {
                    const predicted = Math.max((cpu.computeMs / cpu.cycles) * CPU_PREDICT_FACTOR, cpu.maxMs);
                    if (cpu.computeMs + predicted > CPU_SOFT_BUDGET_MS) return;
                }
                if (!(await renewBotLease(gameId, lease))) return;
            }
            const next = await runCycle(gameId, cycle, cpu, resident);
            if (!next) return;
            resident = next.resident;
            if (next.delayMs > 0) await new Promise((r) => setTimeout(r, next.delayMs));
        }
    } finally {
        await releaseBotLease(gameId, lease);
    }
};

/**
 * One cycle outside the lease and the loop's budget, for the bot latency bench
 * (e2e/bench_bot_e2e.ts): exactly the work a cycle does, with nothing around it.
 */
export async function __botCycle(gameId: string): Promise<void> {
    await runCycle(gameId, 0, { computeMs: 0, cycles: 0, maxMs: 0 }, null);
}

const refusal = (gameId: string, what: string, rc: number) =>
    new Error(`Game ${gameId}: ${what} refused: ${tableCodeName(rc, ['TABLE_E_', 'GAME_INVALID_', 'REPLAY_E'])} (${rc})`);

/** One cycle, committed. Null when nothing was driven (no bot work, or the game ended). */
async function runCycle(
    gameId: string, cycle: number, cpu: CpuAcct, resident: Uint8Array | null,
): Promise<{ delayMs: number; resident: Uint8Array | null } | null> {
    const reqId = `bot-${cycle}-${gameId.substring(0, 6)}`;
    const table = await serverTable();
    // The moves an attempt that lost the CAS already chose, offered back: the
    // kernel replays one only while the reloaded state still makes it legal, so
    // a retry does not pay for a second Monte-Carlo search.
    let prefs: Uint8Array | null = null;

    for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        const row = await loadRow(gameId, false);

        // ---- kernel section A ----
        let rc = table.load(row.state, row.roster);
        if (rc < 0) throw refusal(gameId, 'load', rc);
        const needsBots = table.needsBots();
        const needsLogs = table.botsNeedLogs();
        const botsOnly = table.seats().every((s) => s.brain !== '');
        // ---- end of section A ----
        if (!needsBots) return null;

        let sessionLog: Uint8Array | null = null;
        if (needsLogs) {
            sessionLog = botsOnly && resident ? resident : await readSessionLog(gameId);
        }

        // ---- kernel section B ----
        rc = table.load(row.state, row.roster);
        if (rc < 0) throw refusal(gameId, 'load', rc);
        rc = table.setDealSeed(row.gameSeed);
        if (rc < 0) throw refusal(gameId, 'deal seed', rc);
        if (sessionLog) {
            rc = table.importSessionLog(sessionLog);
            if (rc < 0) throw refusal(gameId, 'session log', rc);
        }
        const t0 = Date.now();
        const drive = table.botDrive(prefs);
        const driveMs = Date.now() - t0;
        if (typeof drive === 'number') throw refusal(gameId, 'bot drive', drive);
        let products: TableProducts | null = null;
        let seats: TableSeat[] = [];
        const pushes: { viewer: number; bytes: Uint8Array }[] = [];
        let delayMs = 0;
        if (drive.n > 0) {
            const p = table.commit(gameId, row.version + 1, Date.now());
            if (typeof p === 'number') throw refusal(gameId, 'commit products', p);
            products = p;
            seats = table.seats();
            if (p.nEvents > 0) {
                for (const viewer of [...seats.flatMap((s, i) => (s.brain ? [] : [i])), -1]) {
                    const push = table.push(gameId, viewer);
                    if (typeof push === 'number') throw refusal(gameId, 'push', push);
                    pushes.push({ viewer, bytes: push });
                }
            }
            delayMs = table.cycleDelayMs();
            prefs = table.drivePrefs();
        }
        // ---- end of section B ----

        cpu.computeMs += driveMs;
        cpu.cycles += 1;
        if (driveMs > cpu.maxMs) cpu.maxMs = driveMs;
        console.log(`[${reqId}] drove ${drive.n} action(s) by seats [${drive.seats.join(', ')}] in ${driveMs}ms `
            + `(stop ${drive.stop}, delay ${delayMs}ms) ${memLine(table.memoryBytes())}`);

        if (!products) return null;
        const version = await commitProducts(gameId, row, products, seats, null);
        if (version === null) continue;   // somebody else committed: reload and drive again

        if (products.ended) {
            const { finalizeEndedGame } = await finalizeMod();
            await finalizeEndedGame(gameId, products.state, products.roster, reqId);
        }
        if (pushes.length > 0) {
            broadcastPushes(gameId, version, seats, pushes, reqId)
                .catch((err) => console.error(`[${reqId}] broadcast failed:`, err));
        }
        if (products.ended) return null;

        // The session log carried forward: what this segment read, plus what it
        // just appended - exactly logs_packed when nobody else writes.
        const nextResident = botsOnly && sessionLog
            ? (products.logs ? concat(sessionLog, products.logs) : sessionLog)
            : null;
        return { delayMs, resident: nextResident };
    }
    throw new Error(`Could not commit game ${gameId} after ${MAX_ATTEMPTS} attempts - write contention`);
}

async function readSessionLog(gameId: string): Promise<Uint8Array | null> {
    try {
        const { data } = await supabaseClient.from('games').select('logs_packed').eq('id', gameId).single();
        return data?.logs_packed ? columnHexToBytes(data.logs_packed) : null;
    } catch (e) {
        console.error(`[BELIEF] session log read failed for ${gameId}:`, e);
        return null;
    }
}

function concat(a: Uint8Array, b: Uint8Array): Uint8Array {
    const out = new Uint8Array(a.length + b.length);
    out.set(a, 0);
    out.set(b, a.length);
    return out;
}
