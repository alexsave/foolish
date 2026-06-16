import { PrivatePlayer, AnimationEvent, GAME_STATUS, PLAYER_STATUS, GAME_MOVE_TYPE } from './types.ts';
import { executeWithGameLock } from './utils.ts';
import { calculateLegalMoves } from './bot_strategy.ts';
import { createClient } from 'jsr:@supabase/supabase-js';
import { processBotAction, shouldBotActCore } from './pure_bot_actions.ts';

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

// Global variable to track current bot processing delay
let currentBotDelay = BOT_PROCESSING_DELAY_WITH_HUMANS;

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
const processBotActions = async (game_id: string, cycle: number = 0, loopStartTime?: number, cpu: CpuAcct = { computeMs: 0, decisions: 0, maxMs: 0 }, leaseToken: string = ''): Promise<void> => {

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
    let actionEvents: AnimationEvent[] = [];

    // Do everything within a single lock: find eligible bots, choose one, execute action
    try {
        const lockStartTime = Date.now();
        const reqId = `bot-${cycle}-${game_id.substring(0, 6)}`;
        console.log(`[${reqId}][TIMING] Acquiring game lock...`);
        const { game } = await executeWithGameLock(game_id, async (game) => {
            console.log(`[TIMING] Lock acquired in ${Date.now() - lockStartTime}ms`);
            // CRITICAL: executeWithGameLock re-invokes this operation on a CAS
            // conflict. actionEvents/botProcessed live in the OUTER (cycle) scope, so
            // without resetting them here a retried bot move would push its events a
            // SECOND time → the duplicate animation on the client. Reset per attempt.
            actionEvents = [];
            botProcessed = false;
            const lockWorkStartTime = Date.now();
            // Update global delay based on whether humans are still playing
            const humanPlayersStillIn = game.players.filter(player =>
                !player.is_ai && player.status === PLAYER_STATUS.IN
            ).length;

            const newDelay = humanPlayersStillIn > 0 ? BOT_PROCESSING_DELAY_WITH_HUMANS : BOT_PROCESSING_DELAY_BOTS_ONLY;
            if (newDelay !== currentBotDelay) {
                console.log(`Bot delay changed from ${currentBotDelay}ms to ${newDelay}ms (humans in game: ${humanPlayersStillIn})`);
                currentBotDelay = newDelay;
            }

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

                    // Try to process this bot's action
                    const botActionResult = await processBotAction(game, selectedBot.bot);

                    const actionDuration = Date.now() - actionStartTime;
                    // Feed the CPU predictor (count every attempt — failed ones burn CPU too).
                    cpu.computeMs += actionDuration;
                    cpu.decisions += 1;
                    if (actionDuration > cpu.maxMs) cpu.maxMs = actionDuration;
                    if (botActionResult) {
                        actionEvents.push(...(botActionResult.events as unknown as AnimationEvent[]));

                        const isPassiveAction = botActionResult.moveType === GAME_MOVE_TYPE.GOOD || botActionResult.moveType === GAME_MOVE_TYPE.WAIT;
                        
                        console.log(`[ACTION] ✓ Bot ${selectedBot.bot.name} completed ${botActionResult.moveType} action in ${actionDuration}ms`);
                        
                        // For passive actions (good/wait) without animations, continue to try more bots
                        // This bundles multiple passive actions together for snappier feel
                        // Exception: if good causes round transition, it will have events (animations)
                        if (isPassiveAction && botActionResult.events.length === 0) {
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
            return { game, events: actionEvents };
        }, reqId);


        // Note: Animation events are now automatically broadcasted by executeWithGameLock

        console.log(`[TIMING] Total time in executeWithGameLock: ${Date.now() - lockStartTime}ms`);
    } catch (error) {
        console.error('Error in bot processing:', error);
        return;
    }

    const totalCycleTime = Date.now() - cycleStartTime;
    console.log(`[TIMING] Total cycle ${cycle} time: ${totalCycleTime}ms`);

    // Continue the loop if a bot was processed or auto-transition occurred
    if (botProcessed) {
        // Skip pacing when there are no humans watching AND this cycle produced no
        // animations — bots churning through silent goods shouldn't feel padded.
        const skipDelay = actionEvents.length === 0 && currentBotDelay === BOT_PROCESSING_DELAY_BOTS_ONLY;

        if (skipDelay) {
            console.log(`[CYCLE ${cycle}] Cycle took ${totalCycleTime}ms, skipping ${currentBotDelay}ms delay (no events, no humans)`);
        } else if (currentBotDelay > 0) {
            console.log(`[CYCLE ${cycle}] Cycle took ${totalCycleTime}ms, waiting ${currentBotDelay}ms to maintain ${currentBotDelay}ms interval`);
            await new Promise(resolve => setTimeout(resolve, currentBotDelay));
        } else {
            console.log(`[CYCLE ${cycle}] Cycle took ${totalCycleTime}ms (>= ${currentBotDelay}ms target), continuing immediately`);
        }

        return await processBotActions(game_id, cycle + 1, loopStartTime, cpu, leaseToken);
    } else {
        // No bot could act — waiting on a human, or the game is over. Stop; the cron
        // heartbeat (or the next human move) re-drives if there's still bot work.
        console.log(`[CYCLE ${cycle}] No more bot actions needed, ending bot loop for game ${game_id}`);
    }
}
