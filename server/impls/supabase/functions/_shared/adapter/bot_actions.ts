import { GAME_STATUS, PLAYER_STATUS } from '@api/core/types.ts';
import { executeWithGameLock, loadSessionLogBytes, PackedOpProducts } from './utils.ts';
import { strategyUsesLogs, LegalMove } from '@api/common/bot_strategy.ts';
import { createClient } from 'jsr:@supabase/supabase-js';
import {
    __botsWasmMB, ensureBotsAsync, wasmBotDrive, wasmBotEligibleMask, wasmBotCycleDelayMs,
    BotDrivePref,
} from '@sdk/ts/wasm/bots.ts';
import { __kernelWasmMB } from '@sdk/ts/wasm/engine.ts';
import { bytesToHex, hexToBytes } from '@api/common/replay/codec.ts';
import { bytesToBareHex } from '@sdk/ts/wire/bytes.ts';
import { logsFromKernelExport } from '@sdk/ts/wire/logwire.ts';

// One-line memory snapshot against the edge limits (150MB heap + 150MB
// external, where wasm linear memory counts as external). Logged around every
// bot decision to localize "Memory limit exceeded" kills.
const memLine = (): string => {
    let d = '';
    try {
        const m = (globalThis as { Deno?: { memoryUsage?: () => { rss: number; heapTotal: number; heapUsed: number; external: number } } }).Deno?.memoryUsage?.();
        if (m) d = `heap=${Math.round(m.heapUsed / 1048576)}/${Math.round(m.heapTotal / 1048576)}MB ext=${Math.round(m.external / 1048576)}MB rss=${Math.round(m.rss / 1048576)}MB `;
    } catch { /* memoryUsage unavailable */ }
    return `${d}kernelWasm=${__kernelWasmMB()}MB botsWasm=${__botsWasmMB()}MB`;
};

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// Bot timing is NOT a constant here any more: what a move is worth pausing for
// is one table in the kernel (bot_pacing_ms, c/src/bot_drive.c), reached
// through wasmBotCycleDelayMs and shared with every other client
// (docs/C_CORE_CONSOLIDATION.md F3). Its values are the ones this file used to
// hold — 3000ms with a human watching, 300ms bots-only — so the feel is
// unchanged; tune them there and the phone changes with the site.

// --- Adaptive CPU budgeting (instead of a hardcoded wall cap) ---------------
// Supabase caps CPU at ~2s per request (async I/O / setTimeout sleeps don't count;
// cordite's Monte-Carlo search does). Rather than guess a fixed loop length, the
// loop MEASURES each decision's cost and bails when the PREDICTED next decision
// would risk the cap — so a cheap bot runs many cycles while an expensive one stops
// after a few. None of these are bot times; they're the platform cap + safety.
const CPU_HARD_CAP_MS = 2000;          // Supabase per-request CPU limit
const CPU_SOFT_BUDGET_MS = 1700;       // bail target — margin for non-bot CPU + tail
const CPU_PREDICT_FACTOR = 1.5;        // next-decision estimate = avg * this (variance margin)

// Secondary WALL ceiling: a cheap loop that never approaches the CPU budget still
// releases before the ~150s isolate wall-clock kill. The lease is RENEWED each cycle
// (renew_bot_lease), so this is NOT bounded by the lease TTL — a cheap bot can drive
// for up to this long in one segment.
const WALL_CEILING_MS = 120_000;

// Bot-loop lease lifetime (games.bot_lease_*). Auto-expiring; the loop renews it each
// cycle, so it stays SHORT regardless of how long the loop runs. RECOVERY knob: a
// hard-killed loop blocks its game only until this expires (it was renewed <1 cycle
// ago), so recovery is fast even though loops can run long.
const BOT_LEASE_TTL_MS = 25_000;

// Claim the bot-loop lease atomically (replaces the bot_locks baton). The RPC
// returns a token if no live lease exists, else null. Auto-expiring → nothing to
// leak even if this isolate dies mid-loop.
const acquireBotLease = async (game_id: string): Promise<string | null> => {
    try {
        const { data, error } = await supabaseClient.rpc('try_acquire_bot_lease', {
            p_game_id: game_id,
            p_ttl_ms: BOT_LEASE_TTL_MS,
        });
        if (error) {
            console.error(`Failed to acquire bot lease for ${game_id}:`, error);
            return null;
        }
        return (data as string | null) ?? null;
    } catch (error) {
        console.error(`Error acquiring bot lease for ${game_id}:`, error);
        return null;
    }
};

// Best-effort early release (fenced on our token in SQL). If we never get here —
// reaped isolate — the lease just expires on its own.
const releaseBotLease = async (game_id: string, token: string): Promise<void> => {
    try {
        await supabaseClient.rpc('release_bot_lease', { p_game_id: game_id, p_token: token });
    } catch (error) {
        console.error(`Error releasing bot lease for ${game_id}:`, error);
    }
};

// Extend our lease (called each cycle). Returns false if we no longer hold it (the
// RPC's fence didn't match) — i.e. another loop took over — so we should stop. On a
// transport error, assume we still hold it (don't drop a healthy loop over a blip).
const renewBotLease = async (game_id: string, token: string): Promise<boolean> => {
    try {
        const { data, error } = await supabaseClient.rpc('renew_bot_lease', {
            p_game_id: game_id, p_token: token, p_ttl_ms: BOT_LEASE_TTL_MS,
        });
        if (error) {
            console.error(`Error renewing bot lease for ${game_id}:`, error);
            return true;
        }
        return data !== false;
    } catch (error) {
        console.error(`Error renewing bot lease for ${game_id}:`, error);
        return true;
    }
};

export const lockedBotLoop = async (game_id: string): Promise<void> => {
    // ONE drive segment. Continuation across segments is handled by the pg_cron
    // bot-heartbeat (each tick is a fresh request => fresh CPU budget) — we do NOT
    // self-continue in-isolate, since chained segments would share one 2s CPU budget.
    // Instantiate bots.wasm up front: it adopts the engine slot, so this worker
    // never builds the rules.wasm instance it would otherwise abandon on the
    // first bot decision. Use the ASYNC loader: on Deno it seeds the byte cache
    // via Deno.readFile so the sync bots() below hits the cache — calling the
    // sync loadWasmGz here would run Deno.readFileSync inside this async handler,
    // which Deno warns against and will disallow.
    await ensureBotsAsync();
    console.log(`[MEM] lockedBotLoop start: ${memLine()}`);
    const leaseToken = await acquireBotLease(game_id);
    if (!leaseToken) {
        console.log('bot lease held by another loop, skipping');
        return;         // another loop / driver holds the lease
    }

    // Adaptive CPU accounting: cumulative bot compute (wall ms ~= CPU ms, compute-bound),
    // decision count and worst single decision — the loop predicts from these when to bail.
    const cpu: CpuAcct = { computeMs: 0, decisions: 0, maxMs: 0 };
    try {
        await processBotActions(game_id, 0, undefined, cpu, leaseToken);
    } finally {
        await releaseBotLease(game_id, leaseToken);
    }
}


type CpuAcct = { computeMs: number; decisions: number; maxMs: number };

// New improved bot processing that fixes eligibility drift
// Uses one-bot-per-iteration approach to prevent race conditions
// concat two byte buffers (resident belief log + this move's appended records).
const concatBytes = (a: Uint8Array, b: Uint8Array): Uint8Array => {
    const out = new Uint8Array(a.length + b.length);
    out.set(a, 0); out.set(b, a.length);
    return out;
};

// `residentBelief`: the current session's packed log bytes carried across cycles
// of ONE drive segment, so a belief bot's per-cycle DB read (loadSessionLogBytes)
// happens once instead of every cycle. Only used for bots-only games — under the
// bot lease there is no concurrent writer, so appending our own committed records
// keeps it byte-identical to games.logs_packed. Human games can be written
// concurrently, so they reload each cycle (null carried forward).
const processBotActions = async (game_id: string, cycle: number = 0, loopStartTime?: number, cpu: CpuAcct = { computeMs: 0, decisions: 0, maxMs: 0 }, leaseToken: string = '', residentBelief: Uint8Array | null = null): Promise<void> => {

    if (loopStartTime === undefined) {
        loopStartTime = Date.now();
    } else {
        const elapsed = Date.now() - loopStartTime;

        // (1) Wall ceiling — release before the ~150s isolate wall-clock kill.
        // Caught by a cheap loop that never approaches the CPU budget.
        if (elapsed > WALL_CEILING_MS) {
            return;
        }

        // (2) CPU prediction — estimate the next decision from observed cost and bail
        // BEFORE we'd risk the ~2s CPU cap (a CPU-kill would hold the lease until it
        // expires). predict = max(avg * factor, worst-seen): honours the average but
        // stays tail-aware. Cheap bots (small avg) run many cycles; expensive ones
        // bail after a few. No hardcoded bot times — all measured.
        if (cpu.decisions > 0) {
            const avg = cpu.computeMs / cpu.decisions;
            const predictedNext = Math.max(avg * CPU_PREDICT_FACTOR, cpu.maxMs);
            if (cpu.computeMs + predictedNext > CPU_SOFT_BUDGET_MS) {
                return;
            }
        }

        // (3) We're continuing — keep our (short-TTL) lease alive for this cycle.
        if (leaseToken && !(await renewBotLease(game_id, leaseToken))) {
            return; // another loop took over our lease
        }
    }

    const cycleStartTime = Date.now();
    console.log(`[CYCLE ${cycle}] Starting bot processing for game ${game_id}`);

    let botProcessed = false;
    // The cycle's kernel products (docs/PACKED_WIRE_CUTOVER.md): every bot
    // move applies inside the kernel, so the commit takes the last move's
    // state blob, the concatenated logwire records of every bundled move,
    // and the (single — bundled passives are zero-event by definition)
    // per-viewer event streams. No JS AnimationEvents on this path.
    let packedStateHex: string | null = null;
    let packedLogsHex = '';
    let packedEnded = false;
    let packedNEvents = 0;
    let packedEvents: Map<number, Uint8Array> | null = null;
    // Belief-log bytes actually used this cycle + whether the game was bots-only
    // (set inside the lock), so the recursion can carry a resident log forward.
    let hydratedBelief: Uint8Array | undefined;
    let botsOnlyCycle = false;
    // Pacing for THIS cycle, from the kernel's table. Must not be module state:
    // one warm isolate can drive several games (heartbeat SCAN), and a bots-only
    // game writing 300ms there leaked into a concurrent humans game expecting
    // 3000ms, and vice versa.
    let cycleDelay = 0;
    // Strategy decisions carried across CAS attempts within this cycle. On a
    // version conflict executeWithGameLock re-runs the whole operation; without
    // this a bot recomputes its move from scratch each attempt — for cordite's
    // Monte-Carlo search that can be seconds of CPU, and up to 5 attempts blows
    // the ~2s budget and gets the isolate killed holding the lease. The kernel
    // replays a cached move iff it is still LEGAL in the reloaded state (a
    // legal, slightly-stale choice beats a CPU kill) and otherwise re-chooses,
    // so the legality re-check that used to live here is gone — it was a rule,
    // and rules are the kernel's (see BotDrivePref).
    const movesFromFailedAttempts = new Map<number, LegalMove>();

    // Do everything within a single lock: find eligible bots, choose one, execute action
    try {
        const lockStartTime = Date.now();
        const reqId = `bot-${cycle}-${game_id.substring(0, 6)}`;
        console.log(`[${reqId}][TIMING] Acquiring game lock...`);
        const { game } = await executeWithGameLock(game_id, async (game) => {
            console.log(`[TIMING] Lock acquired in ${Date.now() - lockStartTime}ms`);
            // CRITICAL: executeWithGameLock re-invokes this operation on a CAS
            // conflict. The packed accumulators/botProcessed live in the OUTER
            // (cycle) scope, so without resetting them here a retried bot move
            // would append its logs/events a SECOND time. Reset per attempt.
            packedStateHex = null;
            packedLogsHex = '';
            packedEnded = false;
            packedNEvents = 0;
            packedEvents = null;
            botProcessed = false;
            const lockWorkStartTime = Date.now();
            cycleDelay = 0;

            // Capture game state for broadcasting later
            // Only process bot actions if game is in a state where bots can act
            if (game.status === GAME_STATUS.WAITING || game.status === GAME_STATUS.GAME_OVER) {
                console.log(`Bot processing skipped - game status is ${game.status}`);
                return { game, events: [] };
            }

            // Safety check: if there's only one player left, the game should have ended
            const in_players = game.players.filter(player => player.status === PLAYER_STATUS.IN);
            if (in_players.length <= 1) {
                console.warn(`Bot processing stopped - only ${in_players.length} player(s) left, ending game`);
                return { game, events: [] };
            }

            // Check if there are any bots
            const botCount = game.players.filter(p => p.is_ai).length;
            if (botCount === 0) {
                console.log(`Bot processing skipped - no bots in game`);
                return { game, events: [] };
            }
            console.log(`Found ${botCount} bots in game`);


            // The cycle itself is the kernel's (docs/C_CORE_CONSOLIDATION.md
            // F2): eligibility, the fair shuffle among simultaneously-eligible
            // bots, the roster dispatch, the apply, the bundling of silent
            // actions and the stop conditions were all game logic in a TS coat,
            // and are now one call — the same call the phone makes. What stays
            // here is what the kernel cannot do: the lease, the CAS commit, the
            // broadcast, the CPU budget, and the two I/O reads below.
            //
            // human_mask is "seats the kernel must not drive". It is NOT "seats
            // to wait for": a human being able to act is not a stop condition
            // (owner decision) — the site has always thrown bots in while a
            // human deliberates, and yielding would stall a bout on an idle
            // player, since the defender and every attacker are eligible at once.
            let humanMask = 0, aiMask = 0;
            const humanSeats: number[] = [];
            game.players.forEach((p, i) => {
                if (p.is_ai) aiMask |= 1 << i;
                else { humanMask |= 1 << i; humanSeats.push(i); }
            });

            const eligible = wasmBotEligibleMask(game, humanMask);
            if (eligible === 0) {
                console.log(`No eligible bots found for game ${game_id}, ending bot processing cycle`);
                return { game, events: [] };
            }
            const eligibleSeats = game.players.map((_, i) => i).filter(i => eligible & (1 << i));
            console.log(`Found ${eligibleSeats.length} eligible bots: ${eligibleSeats.map(i => game.players[i].name).join(', ')}`);

            // Belief bots (octogen/semtex/cordite/fulminate/espresso) deduce
            // hidden cards from the whole current session, but the hot-path
            // loader (loadCompleteGame) leaves game.logs empty. Hand them the
            // persisted, DRAW-masked session log as its RAW PACKED BYTES —
            // once per cycle, only when a belief bot is actually eligible so
            // the beliefless bots keep the fast path. This is the I/O the
            // kernel cannot do, which is why the drive is asked who is eligible
            // BEFORE it is asked to drive. The kernel importer splices these
            // bytes in directly (no JS-object decode/marshal; see
            // importLogsPacked). It stays off game.logs on purpose: the commit
            // path re-encodes game.logs, so putting the whole session there
            // would re-append it every move. Without this the belief bots play
            // blind (the octogen regression from the loadCompleteGame log-load
            // removal).
            const usesLogs = eligibleSeats.some(i => strategyUsesLogs(game.players[i].strategy_key!));
            if (usesLogs) {
                // Bots-only games have no concurrent writer under the lease, so
                // a resident log carried from the previous cycle is still exactly
                // logs_packed — reuse it and skip the DB read. Human games might
                // be written between cycles, so always reload fresh.
                botsOnlyCycle = game.players.every(p => p.is_ai || p.status !== PLAYER_STATUS.IN);
                if (residentBelief && botsOnlyCycle) {
                    game.belief_log_bytes = residentBelief;
                } else {
                    game.belief_log_bytes = (await loadSessionLogBytes(game_id)) ?? undefined;
                }
                hydratedBelief = game.belief_log_bytes;
                console.log(`[BELIEF] ${residentBelief && botsOnlyCycle ? 'resident' : 'loaded'} ${game.belief_log_bytes?.length ?? 0} session-log bytes for belief bots`);
            }

            // Moves this cycle already chose in an attempt that then lost the
            // CAS. Offered back, not trusted: the kernel replays one only while
            // the reloaded state still makes it legal.
            const prefs: BotDrivePref[] = [...movesFromFailedAttempts]
                .map(([seat, move]) => ({ seat, move }));

            console.log(`[MEM] before drive: ${memLine()}`);
            const driveStartTime = Date.now();
            const drive = wasmBotDrive(game, {
                humanMask, aiMask, humanSeats, logs: usesLogs, prefs,
            });
            const driveDuration = Date.now() - driveStartTime;
            console.log(`[MEM] after  drive: ${memLine()}`);

            // Feed the CPU predictor. The unit is now the CYCLE, which is also
            // the unit the loop bails in — one drive can deliberate for several
            // seats, so predicting from per-move costs would under-estimate the
            // next iteration and risk the ~2s cap.
            cpu.computeMs += driveDuration;
            cpu.decisions += 1;
            if (driveDuration > cpu.maxMs) cpu.maxMs = driveDuration;

            for (const a of drive.actions) movesFromFailedAttempts.set(a.seat, a.move);

            if (drive.run) {
                // The whole cycle's products, straight from the kernel: the
                // final state blob, every bundled action's logwire records, and
                // the per-viewer streams of the one event-bearing action
                // (passives only bundle when they have none).
                const r = drive.run;
                packedLogsHex = bytesToBareHex(logsFromKernelExport(r.logsWire, Date.now()));
                packedStateHex = bytesToHex(r.stateBlob);
                packedEnded = r.ended;
                if (r.nEvents > 0) {
                    packedEvents = r.events;
                    packedNEvents = r.nEvents;
                }
                botProcessed = true;
            }

            // What the cycle is worth pausing for. One kernel call: it reduces
            // the cycle's actions to their most visible pacing class, prices
            // that from its one table, and reduces it again for a human still
            // IN. All three of those are facts about the game, so none of them
            // is re-derived here any more.
            cycleDelay = wasmBotCycleDelayMs(humanMask);

            console.log(`[ACTION] ${drive.actions.length ? '✓' : '✗'} drove ${drive.actions.length} action(s) in ${driveDuration}ms: `
                + `${drive.actions.map(a => `${game.players[a.seat].name}:${a.move.type}`).join(', ') || 'none'}`
                + ` (stop ${drive.stop}, delay ${cycleDelay}ms)`);
            if (!botProcessed) {
                console.log(`[ACTION] No eligible bots could make valid moves in game ${game_id}`);
            }

            console.log(`[TIMING] Lock work completed in ${Date.now() - lockWorkStartTime}ms`);
            const packed: PackedOpProducts | undefined = packedStateHex ? {
                ended: packedEnded,
                stateHex: packedStateHex,
                logsHex: packedLogsHex || null,
                nEvents: packedNEvents,
                events: packedEvents ?? new Map<number, Uint8Array>(),
            } : undefined;
            return { game, events: [], packed };
        }, reqId);


        // Note: Animation events are now automatically broadcasted by executeWithGameLock

        console.log(`[TIMING] Total time in executeWithGameLock: ${Date.now() - lockStartTime}ms`);
    } catch (error) {
        console.error('Error in bot processing:', error);
        return;
    }

    const totalCycleTime = Date.now() - cycleStartTime;
    console.log(`[TIMING] Total cycle ${cycle} time: ${totalCycleTime}ms`);

    // Carry the session log forward for the next cycle WITHOUT another DB read:
    // this cycle's belief bytes + the logwire records this move just committed
    // (packedLogsHex is exactly what commit_game appended to logs_packed). Only
    // for bots-only cycles, where no other actor could have written in between.
    let nextResident: Uint8Array | null = null;
    if (botsOnlyCycle && hydratedBelief) {
        const moveBytes = packedLogsHex ? hexToBytes(packedLogsHex) : new Uint8Array(0);
        nextResident = moveBytes.length ? concatBytes(hydratedBelief, moveBytes) : hydratedBelief;
    }

    // Continue the loop if a bot was processed or auto-transition occurred
    if (botProcessed) {
        // How long to wait was decided in the lock, by the kernel's pacing
        // table (F3). Zero means nothing became visible — a cycle of silent
        // goods, which bundling exists to make free. NOTE this is the one
        // deliberate behavior change of the port: this loop used to skip its
        // wait only in bots-only games and otherwise pause the full 3000ms for
        // a cycle that changed nothing on screen.
        if (cycleDelay > 0) {
            console.log(`[CYCLE ${cycle}] Cycle took ${totalCycleTime}ms, waiting ${cycleDelay}ms to maintain ${cycleDelay}ms interval`);
            await new Promise(resolve => setTimeout(resolve, cycleDelay));
        } else {
            console.log(`[CYCLE ${cycle}] Cycle took ${totalCycleTime}ms, nothing to watch — continuing immediately`);
        }

        return await processBotActions(game_id, cycle + 1, loopStartTime, cpu, leaseToken, nextResident);
    } else {
        // No bot could act — waiting on a human, or the game is over. Stop; the cron
        // heartbeat (or the next human move) re-drives if there's still bot work.
        console.log(`[CYCLE ${cycle}] No more bot actions needed, ending bot loop for game ${game_id}`);
    }
}
