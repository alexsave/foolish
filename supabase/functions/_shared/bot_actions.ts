import { PrivatePlayer, AnimationEvent, GAME_STATUS, PLAYER_STATUS, GAME_MOVE_TYPE } from './types.ts';
import { executeWithGameLock } from './utils.ts';
import { calculateLegalMoves } from './bot_strategy.ts';
import { createClient } from 'jsr:@supabase/supabase-js';
import { processBotAction, shouldBotActCore } from './pure_bot_actions.ts';
import { botDiag } from './diag.ts';

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

// How long one loop runs before self-releasing its lease. Kept SHORT on purpose:
// cordite is CPU-bound and Supabase caps CPU at ~2s/request, so a long single loop
// can't help anyway — continuation is driven by the pg_cron bot-heartbeat (each tick
// is a fresh request => fresh CPU budget). A short loop + short lease means fast
// recovery if an isolate is CPU-killed mid-run. MUST stay below BOT_LEASE_TTL_MS.
const MAX_LOOP_RUNTIME = 20_000;

// Bot-loop lease lifetime (games.bot_lease_*). Auto-expiring, so a reaped isolate's
// lease simply lapses — no finally-release to leak. This is the RECOVERY knob: if an
// isolate is killed (CPU/wall) mid-loop, the game is blocked until the lease expires.
// Kept short (just above MAX_LOOP_RUNTIME) so the next heartbeat retakes quickly.
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

export const lockedBotLoop = async (game_id: string): Promise<void> => {
    // ONE drive segment. Continuation across segments is handled by the pg_cron
    // bot-heartbeat (each tick is a fresh request => fresh CPU budget) — we do NOT
    // self-continue in-isolate, since chained segments would share one 2s CPU budget.
    const leaseToken = await acquireBotLease(game_id);
    if (!leaseToken) {
        console.log('bot lease held by another loop, skipping');
        return;         // another loop / driver holds the lease
    }

    const loopId = crypto.randomUUID().split('-')[0];
    const loopStart = Date.now();
    // Cumulative cordite/bot compute (wall ms ~= CPU ms, since it's compute-bound)
    // so we can see how close a single drive gets to the ~2s CPU cap.
    const cpu = { computeMs: 0 };
    await botDiag(game_id, 'T1_BATON', { event: 'lease_acquired', loopId });

    let outcome: LoopOutcome = 'idle-wait';
    try {
        outcome = await processBotActions(game_id, 0, undefined, loopId, cpu);
    } finally {
        await releaseBotLease(game_id, leaseToken);
        await botDiag(game_id, 'T1_BATON', {
            event: 'loop_ended', loopId, outcome,
            cpuComputeMs: cpu.computeMs, runtimeMs: Date.now() - loopStart,
        });
    }
}


// Outcome of a bot-processing run, used by lockedBotLoop to decide self-continuation:
//   maxruntime    cut short at MAX_LOOP_RUNTIME with bot work still pending -> continue
//   idle-botsonly no bot could act, but game is PLAYING with NO human left in -> bots
//                 must keep going on their own (no client will bump us) -> continue
//   idle-wait     waiting on a human / game over / error -> stop (their move re-triggers)
type LoopOutcome = 'maxruntime' | 'idle-botsonly' | 'idle-wait';

// New improved bot processing that fixes eligibility drift
// Uses one-bot-per-iteration approach to prevent race conditions
const processBotActions = async (game_id: string, cycle: number = 0, loopStartTime?: number, loopId: string = '?', cpu: { computeMs: number } = { computeMs: 0 }): Promise<LoopOutcome> => {

    if(loopStartTime === undefined) {
        loopStartTime = Date.now();
    } else {
        const now = Date.now();
        const elapsed = now - loopStartTime;
        if (elapsed > MAX_LOOP_RUNTIME) {
            // Self-abort: stop and let the finally release our lease, even if more
            // bot moves remain. The next bump (client poll / next action) re-claims
            // the lease and continues. The lease auto-expires regardless, so an
            // abort vs a reaped isolate are both harmless now.
            await botDiag(game_id, 'T1_BATON', {
                event: 'max_runtime_abort', loopId, cycle, elapsedMs: elapsed,
            });
            return 'maxruntime';

        }
    }

    const cycleStartTime = Date.now();
    // T1: how deep into the 65s budget this cycle is STARTING. A cycle starting at,
    // say, 61000ms then doing ~7s of work blows past the ~70s platform hard-kill —
    // exactly the leak window. The last row before a freeze should show a high value.
    await botDiag(game_id, 'T1_BATON', {
        event: 'cycle_start', loopId, cycle, sinceLoopStartMs: cycleStartTime - loopStartTime,
    });
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
                // T6: currentBotDelay is a MODULE-GLOBAL shared by every game served
                // by this warm isolate. If two games (one with humans, one bots-only)
                // interleave here, they clobber each other's pacing. Rapid flip-flop
                // rows for DIFFERENT game_ids = the global is being corrupted.
                await botDiag(game_id, 'T6_DELAYGLOBAL', {
                    loopId, cycle, from: currentBotDelay, to: newDelay,
                    humanPlayersStillIn,
                });
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
                    cpu.computeMs += actionDuration; // track cumulative bot compute (CPU-cap proximity)
                    if (botActionResult) {
                        actionEvents.push(...(botActionResult.events as unknown as AnimationEvent[]));

                        // T3: the single biggest "slow as fuck" lever. computeMs is the
                        // bot's think time (cordite's maxMillis=2000 caps it); delayMs is
                        // the artificial inter-bot pacing added AFTER. Per-decision rows
                        // let us sum "real compute" vs "padding" across a whole round.
                        await botDiag(game_id, 'T3_PACE', {
                            loopId, cycle, bot: selectedBot.bot.name,
                            strategy: (selectedBot.bot as any).strategy_key ?? null,
                            moveType: botActionResult.moveType,
                            computeMs: actionDuration,
                            plannedDelayMs: currentBotDelay,
                            pc: game.players.length,
                        });

                        // T9: a SINGLE decision that overran the 10s game_lock stale
                        // window. maxMillis (2000) is only checked between worlds, so one
                        // world's exact-endgame solve can blow far past it — and while we
                        // overrun, another request can steal the lock we still hold.
                        if (actionDuration > 8000) {
                            await botDiag(game_id, 'T9_RUNAWAY', {
                                loopId, cycle, bot: selectedBot.bot.name,
                                strategy: (selectedBot.bot as any).strategy_key ?? null,
                                moveType: botActionResult.moveType,
                                computeMs: actionDuration, pc: game.players.length,
                            });
                        }

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
                    // T4: the engine said these bots SHOULD act (shouldBotActCore true +
                    // legal moves existed) yet every processBotAction failed. That is a
                    // genuine logic stall — capture enough state to reconstruct why.
                    await botDiag(game_id, 'T4_NOPROGRESS', {
                        loopId, cycle,
                        status: game.status, defender: game.defender,
                        firstAttacker: game.first_attacker,
                        tableBattles: game.table_battles.length,
                        coveredBattles: game.table_battles.filter((b: any) => b.defense !== null).length,
                        goodPlayers: game.good_players?.length ?? 0,
                        eligible: eligibleBots.map(b => ({
                            name: b.bot.name, index: b.index,
                            strategy: (b.bot as any).strategy_key ?? null,
                            legalMoves: calculateLegalMoves(game, b.bot.player_id).length,
                        })),
                    });
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
        return 'idle-wait';
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

        return await processBotActions(game_id, cycle + 1, loopStartTime, loopId, cpu);
    } else {
        console.log(`[CYCLE ${cycle}] No more bot actions needed, ending bot loop for game ${game_id}`);

        const isPlaying = !!game && game.status === GAME_STATUS.PLAYING;
        const humanInCount = isPlaying
            ? game.players.filter(p => !p.is_ai && p.status === PLAYER_STATUS.IN).length
            : -1;

        // T7: the loop is ending while the game is still PLAYING — i.e. we are now
        // waiting on a HUMAN (or, if humanInCount===0, it's bots-only and we'll
        // self-continue). Log who we're blocked on either way.
        try {
            if (isPlaying) {
                const defender = game.players[game.defender];
                await botDiag(game_id, 'T7_GHOSTHUMAN', {
                    loopId, cycle,
                    defenderIndex: game.defender,
                    defender: defender
                        ? { name: defender.name, is_ai: defender.is_ai, status: defender.status }
                        : null,
                    humanInCount,
                    tableBattles: game.table_battles.length,
                    coveredBattles: game.table_battles.filter((b: any) => b.defense !== null).length,
                    goodPlayers: game.good_players?.length ?? 0,
                });
            }
        } catch (_e) { /* diagnostics must not break the loop */ }

        // No bot could act. If the game is still PLAYING but NO human remains in,
        // the bots must keep advancing it themselves (nobody will bump us) — signal
        // self-continuation. Otherwise we're correctly waiting on a human (their
        // action re-triggers the loop), or the game is over.
        if (isPlaying && humanInCount === 0) return 'idle-botsonly';
        return 'idle-wait';
    }
}
