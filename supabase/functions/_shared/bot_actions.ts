import { Game, PrivatePlayer, AnimationEvent, GAME_STATUS, PLAYER_STATUS } from './types.ts';
import { executeWithGameLock } from './utils.ts';
import { calculateLegalMoves, getBotStrategy, LegalMove } from './bot_strategy.ts';
import { createClient } from 'jsr:@supabase/supabase-js';
import { processBotAction, shouldBotActCore } from './pure_bot_actions.ts';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// Bot timing constants
const BOT_PROCESSING_DELAY_WITH_HUMANS = 4500; // Delay when humans are still playing (ms)
const BOT_PROCESSING_DELAY_BOTS_ONLY = 800; // Delay when only bots remain (ms)

// Global variable to track current bot processing delay
let currentBotDelay = BOT_PROCESSING_DELAY_WITH_HUMANS;

const acquireBotLoopLock = async (game_id: string): Promise<boolean> => {
    try {
        // Generate a random lock ID for this instance
        const lockId = crypto.randomUUID();

        const { error } = await supabaseClient
            .from('bot_locks')
            .insert({ game_id, lock_id: lockId });

        if (error) {
            // Handle non-unique constraint errors
            if (error.code !== '23505') {
                return false;
            }

            // Check if existing lock is older than 150 seconds
            const { data: existingLock } = await supabaseClient
                .from('bot_locks')
                .select('acquired_at')
                .eq('game_id', game_id)
                .single();

            if (!existingLock) {
                return false;
            }

            const lockAge = Date.now() - new Date(existingLock.acquired_at).getTime();
            if (lockAge <= 150000) { // 150 seconds in milliseconds
                return false;
            }

            // Delete the stale lock
            await supabaseClient
                .from('bot_locks')
                .delete()
                .eq('game_id', game_id);

            // Try to insert again
            const { error: retryError } = await supabaseClient
                .from('bot_locks')
                .insert({ game_id, lock_id: lockId });

            if (retryError) {
                return false;
            }
        }

        // Verify we actually got the lock by checking the lock_id
        const { data, error: selectError } = await supabaseClient
            .from('bot_locks')
            .select('lock_id')
            .eq('game_id', game_id)
            .single();

        if (selectError || !data || data.lock_id !== lockId) {
            return false;
        }

        return true;
    } catch (error) {
        return false;
    }
};

const releaseBotLoopLock = async (game_id: string): Promise<void> => {
    try {
        // Only delete if we have the correct lock_id
        const { error } = await supabaseClient
            .from('bot_locks')
            .delete()
            .eq('game_id', game_id)
        //.eq('lock_id', lockId);

        if (error) {
            console.error(`Failed to release lock for game ${game_id}:`, error);
        }

    } catch (error) {
        console.error(`Error releasing lock for game ${game_id}:`, error);
    }
};

export const lockedBotLoop = async (game_id: string): Promise<void> => {
    if (!(await acquireBotLoopLock(game_id))) {
        return;         // another cycle has the baton
    }

    try {
        await processBotActions(game_id);
    } finally {
        await releaseBotLoopLock(game_id);
    }
}


// New improved bot processing that fixes eligibility drift
// Uses one-bot-per-iteration approach to prevent race conditions
const processBotActions = async (game_id: string, cycle: number = 0): Promise<void> => {
    if (cycle > 1000) {
        console.log(`Bot processing stopped - max cycles reached for game ${game_id}`);
        return;
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
                
                // Shuffle the eligible bots to try them in random order
                const shuffledBots = [...eligibleBots].sort(() => Math.random() - 0.5);

                for (const selectedBot of shuffledBots) {
                    console.log(`[ACTION] Trying bot ${selectedBot.bot.name} from ${eligibleBots.length} eligible bots`);
                    const actionStartTime = Date.now();

                    // Try to process this bot's action
                    const botActionEvents = await processBotAction(game, selectedBot.bot);

                    const actionDuration = Date.now() - actionStartTime;
                    if (botActionEvents) {
                        actionEvents.push(...(botActionEvents as unknown as AnimationEvent[]));
                        botProcessed = true;
                        console.log(`[ACTION] ✓ Bot ${selectedBot.bot.name} completed action in ${actionDuration}ms`);
                        break; // Exit the loop since we successfully processed a bot
                    } else {
                        console.log(`[ACTION] ✗ Bot ${selectedBot.bot.name} move failed after ${actionDuration}ms, trying next bot`);
                    }
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
        // Calculate remaining delay to ensure consistent timing between cycle starts
        const remainingDelay = Math.max(0, currentBotDelay - totalCycleTime);
        
        if (remainingDelay > 0) {
            console.log(`[CYCLE ${cycle}] Cycle took ${totalCycleTime}ms, waiting ${remainingDelay}ms to maintain ${currentBotDelay}ms interval`);
            await new Promise(resolve => setTimeout(resolve, remainingDelay));
        } else {
            console.log(`[CYCLE ${cycle}] Cycle took ${totalCycleTime}ms (>= ${currentBotDelay}ms target), continuing immediately`);
        }
        
        return await processBotActions(game_id, cycle + 1);
    } else {
        console.log(`[CYCLE ${cycle}] No more bot actions needed, ending bot loop for game ${game_id}`);
    }
}


// Check if any bots in the game have legal moves available
function checkIfAnyBotHasLegalMoves(game: Game): boolean {
    // Don't process bots if game is not in a playable state
    if (game.status === GAME_STATUS.WAITING || game.status === GAME_STATUS.GAME_OVER) {
        return false;
    }

    // Find all bots in the game
    const bots = game.players.filter(player => player.is_ai);

    if (bots.length === 0) {
        return false; // No bots to check
    }

    // Check each bot to see if they should act and have legal moves
    for (const bot of bots) {
        const botIndex = game.players.indexOf(bot);
        let shouldAct = false;

        // Determine if this bot should act in current game state using logical checks
        shouldAct = shouldBotActCore(game, bot, botIndex);

        if (shouldAct) {
            // Check if this bot has any legal moves
            const legalMoves = calculateLegalMoves(game, bot.player_id);
            if (legalMoves.length > 0) {
                return true; // Found a bot with legal moves
            }
        }
    }

    return false; // No bots have legal moves
}

