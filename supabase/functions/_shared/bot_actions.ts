import { PrivatePlayer, GAME_STATUS, PLAYER_STATUS, GAME_MOVE_TYPE } from './types.ts';
import { executeWithGameLock, loadSessionLogBytes, PackedOpProducts } from './utils.ts';
import { calculateLegalMoves, strategyUsesLogs, LegalMove } from './bot_strategy.ts';
import { createClient } from 'jsr:@supabase/supabase-js';
import { processBotActionPacked, executeBotMovePacked, shouldBotActCore, PackedBotMove } from './pure_bot_actions.ts';
import { __botsWasmMB, __ensureBots } from './wasm/bots.ts';
import { __kernelWasmMB } from './wasm/engine.ts';
import { bytesToHex, hexToBytes } from './replay/codec.ts';
import { bytesToBareHex } from './wire/bytes.ts';
import { logsFromKernelExport } from './wire/logwire.ts';

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

// Bot timing constants
// Inter-bot pacing so humans can follow the moves. 4500ms felt sluggish but 1500ms
// was too fast to follow — 3000ms is the middle ground (the freeze that made it
// feel even slower was a separate bug, now fixed). Tune here if needed.
const BOT_PROCESSING_DELAY_WITH_HUMANS = 3000;
const BOT_PROCESSING_DELAY_BOTS_ONLY = 300; // Delay when only bots remain (ms)

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
    // first bot decision (see __ensureBots).
    __ensureBots();
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
    // Pacing for THIS cycle, derived from THIS game's players. Must not be
    // module state: one warm isolate can drive several games (heartbeat SCAN),
    // and a bots-only game writing 300ms there leaked into a concurrent
    // humans game expecting 3000ms, and vice versa.
    let cycleDelay = BOT_PROCESSING_DELAY_WITH_HUMANS;
    // Strategy decisions carried across CAS attempts within this cycle. On a
    // version conflict executeWithGameLock re-runs the whole operation; without
    // this a bot recomputes its move from scratch each attempt — for cordite's
    // Monte-Carlo search that can be seconds of CPU, and up to 5 attempts blows
    // the ~2s budget and gets the isolate killed holding the lease. A cached
    // move is replayed iff it is still LEGAL in the reloaded state (a legal,
    // slightly-stale choice beats a CPU kill); otherwise we recompute.
    const movesFromFailedAttempts = new Map<string, LegalMove>();
    const canonMove = (m: LegalMove) => JSON.stringify({ t: m.type, c: m.cards ?? null, a: m.attack_cards ?? null });

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
            // Pacing based on whether humans are still playing in THIS game
            const humanPlayersStillIn = game.players.filter(player =>
                !player.is_ai && player.status === PLAYER_STATUS.IN
            ).length;
            cycleDelay = humanPlayersStillIn > 0 ? BOT_PROCESSING_DELAY_WITH_HUMANS : BOT_PROCESSING_DELAY_BOTS_ONLY;

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


            // Find all bots that can currently move
            const eligibleBots: { bot: PrivatePlayer; index: number }[] = [];
            for (let index = 0; index < game.players.length; index++) {
                const player = game.players[index];
                if (!player.is_ai) continue;

                // Check if this bot should act based on current game state
                const shouldAct = shouldBotActCore(game, player, index);
                if (shouldAct) {
                    // Double-check that they have legal moves
                    const legalMoves = calculateLegalMoves(game, player.player_id);
                    if (legalMoves.length > 0) {
                        eligibleBots.push({ bot: player, index });
                    }
                }
            }

            // If we have eligible bots, try them until one succeeds
            if (eligibleBots.length > 0) {
                console.log(`Found ${eligibleBots.length} eligible bots: ${eligibleBots.map(b => b.bot.name).join(', ')}`);

                // Belief bots (octogen/semtex/cordite/fulminate/espresso) deduce
                // hidden cards from the whole current session, but the hot-path
                // loader (loadCompleteGame) leaves game.logs empty. Hand them the
                // persisted, DRAW-masked session log as its RAW PACKED BYTES —
                // once per cycle, only when a belief bot is actually eligible so
                // the beliefless bots keep the fast path. The kernel importer
                // splices these bytes in directly (no JS-object decode/marshal;
                // see importLogsPacked). It stays off game.logs on purpose: the
                // commit path re-encodes game.logs, so putting the whole session
                // there would re-append it every move. Without this the belief
                // bots play blind (the octogen regression from the
                // loadCompleteGame log-load removal).
                if (eligibleBots.some(b => strategyUsesLogs(b.bot.strategy_key))) {
                    // Bots-only games have no concurrent writer under the lease, so
                    // a resident log carried from the previous cycle is still exactly
                    // logs_packed — reuse it and skip the DB read. Human games might
                    // be written between cycles, so always reload fresh.
                    botsOnlyCycle = humanPlayersStillIn === 0;
                    if (residentBelief && botsOnlyCycle) {
                        game.belief_log_bytes = residentBelief;
                    } else {
                        game.belief_log_bytes = (await loadSessionLogBytes(game_id)) ?? undefined;
                    }
                    hydratedBelief = game.belief_log_bytes;
                    console.log(`[BELIEF] ${residentBelief && botsOnlyCycle ? 'resident' : 'loaded'} ${game.belief_log_bytes?.length ?? 0} session-log bytes for belief bots`);
                }

                // Fisher-Yates shuffle. A comparator-based shuffle
                // (sort(() => Math.random() - 0.5)) violates V8 TimSort's
                // transitivity contract and can livelock the sort on certain
                // arrays — confirmed via offline stress tests at 3+ players.
                const shuffledBots = [...eligibleBots];
                for (let i = shuffledBots.length - 1; i > 0; i--) {
                    const j = Math.floor(Math.random() * (i + 1));
                    [shuffledBots[i], shuffledBots[j]] = [shuffledBots[j], shuffledBots[i]];
                }
                
                // Track if we processed any passive actions without animations
                let anyPassiveProcessed = false;

                for (const selectedBot of shuffledBots) {
                    console.log(`[ACTION] Trying bot ${selectedBot.bot.name} from ${eligibleBots.length} eligible bots`);
                    const actionStartTime = Date.now();

                    // Replay a move computed in a failed CAS attempt if it is
                    // still legal against the reloaded state; else run the
                    // strategy and remember its choice for a possible retry.
                    let botActionResult: false | PackedBotMove = false;
                    const cached = movesFromFailedAttempts.get(selectedBot.bot.player_id);
                    if (cached) {
                        const stillLegal = calculateLegalMoves(game, selectedBot.bot.player_id)
                            .some((m) => canonMove(m) === canonMove(cached));
                        if (stillLegal) {
                            const replayed = executeBotMovePacked(game, selectedBot.bot, cached);
                            if (replayed) {
                                console.log(`[ACTION] Replayed ${selectedBot.bot.name}'s cached ${cached.type} from a conflicted attempt`);
                                botActionResult = replayed;
                            }
                        }
                        if (!botActionResult) movesFromFailedAttempts.delete(selectedBot.bot.player_id);
                    }
                    if (!botActionResult) {
                        console.log(`[MEM] before ${selectedBot.bot.name} (${selectedBot.bot.strategy_key}): ${memLine()}`);
                        const fresh = await processBotActionPacked(game, selectedBot.bot);
                        console.log(`[MEM] after  ${selectedBot.bot.name} (${selectedBot.bot.strategy_key}): ${memLine()}`);
                        if (fresh) {
                            movesFromFailedAttempts.set(selectedBot.bot.player_id, fresh.move);
                            botActionResult = fresh;
                        }
                    }

                    const actionDuration = Date.now() - actionStartTime;
                    // Feed the CPU predictor (count every attempt — failed ones burn CPU too).
                    cpu.computeMs += actionDuration;
                    cpu.decisions += 1;
                    if (actionDuration > cpu.maxMs) cpu.maxMs = actionDuration;
                    if (botActionResult) {
                        const moveEvents = botActionResult.run?.nEvents ?? 0;
                        if (botActionResult.run) {
                            const r = botActionResult.run;
                            // Bundle: the logwire records concatenate; the
                            // state blob is cumulative so the last one wins;
                            // at most one bundled move carries events
                            // (passives only bundle when they have none).
                            packedLogsHex += bytesToBareHex(logsFromKernelExport(r.logsWire, Date.now()));
                            packedStateHex = bytesToHex(r.stateBlob);
                            packedEnded = r.ended;
                            if (r.nEvents > 0) {
                                packedEvents = r.events;
                                packedNEvents = r.nEvents;
                            }
                        }

                        const isPassiveAction = botActionResult.moveType === GAME_MOVE_TYPE.GOOD || botActionResult.moveType === GAME_MOVE_TYPE.WAIT;

                        console.log(`[ACTION] ✓ Bot ${selectedBot.bot.name} completed ${botActionResult.moveType} action in ${actionDuration}ms`);

                        // For passive actions (good/wait) without animations, continue to try more bots
                        // This bundles multiple passive actions together for snappier feel
                        // Exception: if good causes round transition, it will have events (animations)
                        if (isPassiveAction && moveEvents === 0) {
                            console.log(`[ACTION] Passive action without animations, bundling with next bot action`);
                            anyPassiveProcessed = true;
                            continue;
                        }
                        
                        botProcessed = true;
                        // If we break here, we have either:
                        // - A non-passive action (attack, cover, etc.) that needs delay for humans to see
                        // - A passive action WITH animations (round transition) that also needs delay
                        // So we never skip delay when breaking
                        break;
                    } else {
                        console.log(`[ACTION] ✗ Bot ${selectedBot.bot.name} move failed after ${actionDuration}ms, trying next bot`);
                    }
                }

                // Handle case where we only processed passive actions without animations
                if (!botProcessed && anyPassiveProcessed) {
                    botProcessed = true;
                    console.log(`[ACTION] Only passive actions processed this cycle`);
                }

                if (!botProcessed) {
                    console.log(`[ACTION] No eligible bots could make valid moves in game ${game_id}`);
                }
            } else {
                console.log(`No eligible bots found for game ${game_id}, ending bot processing cycle`);
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
        // Skip pacing when there are no humans watching AND this cycle produced no
        // animations — bots churning through silent goods shouldn't feel padded.
        const skipDelay = packedNEvents === 0 && cycleDelay === BOT_PROCESSING_DELAY_BOTS_ONLY;

        if (skipDelay) {
            console.log(`[CYCLE ${cycle}] Cycle took ${totalCycleTime}ms, skipping ${cycleDelay}ms delay (no events, no humans)`);
        } else if (cycleDelay > 0) {
            console.log(`[CYCLE ${cycle}] Cycle took ${totalCycleTime}ms, waiting ${cycleDelay}ms to maintain ${cycleDelay}ms interval`);
            await new Promise(resolve => setTimeout(resolve, cycleDelay));
        } else {
            console.log(`[CYCLE ${cycle}] Cycle took ${totalCycleTime}ms (>= ${cycleDelay}ms target), continuing immediately`);
        }

        return await processBotActions(game_id, cycle + 1, loopStartTime, cpu, leaseToken, nextResident);
    } else {
        // No bot could act — waiting on a human, or the game is over. Stop; the cron
        // heartbeat (or the next human move) re-drives if there's still bot work.
        console.log(`[CYCLE ${cycle}] No more bot actions needed, ending bot loop for game ${game_id}`);
    }
}
