import { Game, PrivatePlayer, Bot, GAME_STATUS, PLAYER_STATUS } from './types.ts';
import { check_win, executeWithGameLock, broadcastAnimationEvents, animationEvents } from './utils.ts';
import { calculateLegalMoves, getBotStrategy, LegalMove } from './bot_strategy.ts';
import { createClient } from 'jsr:@supabase/supabase-js';

// Import shared action handlers
import { executeAttack } from './actions/attack.ts';
import { executeCover } from './actions/cover.ts';
import { executePass } from './actions/pass.ts';
import { executePickup } from './actions/pickup.ts';
import { handleGood } from './actions/good.ts';
import { AnimationEvent } from './types.ts';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// Bot timing constants
const BOT_PROCESSING_DELAY = 3500; // Delay before processing bot actions in a cycle (ms)

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
        return;
    }

    await new Promise(resolve => setTimeout(resolve, BOT_PROCESSING_DELAY));

    let botProcessed = false;
    let shouldAutoTransition = false;

    // Do everything within a single lock: find eligible bots, choose one, execute action
    try {
        const { game } = await executeWithGameLock(game_id, async (game) => {
            // Capture game state for broadcasting later
            // Only process bot actions if game is in a state where bots can act
            if (game.status === GAME_STATUS.WAITING || game.status === GAME_STATUS.GAME_OVER) {
                return { game, events: [] };
            }

            // Safety check: if there's only one player left, the game should have ended
            const in_players = game.players.filter(player => player.status === PLAYER_STATUS.IN);
            if (in_players.length <= 1) {
                console.warn(`Bot processing stopped - only ${in_players.length} player(s) left, ending game`);
                await check_win(game);
                return { game, events: [] };
            }

            // Check if there are any bots
            if (game.players.filter(p => p.is_ai).length === 0) {
                return { game, events: [] };
            }

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

            // If we have eligible bots, randomly choose one and execute its action
            if (eligibleBots.length > 0) {
                const randomIndex = Math.floor(Math.random() * eligibleBots.length);
                const selectedBot = eligibleBots[randomIndex];

                console.log(`Selected bot ${selectedBot.bot.name} from ${eligibleBots.length} eligible bots in game ${game_id}`);

                // Process the selected bot's action
                await processBotAction(game, selectedBot.bot);
                botProcessed = true;

                console.log(`Bot ${selectedBot.bot.name} completed action`);
            } else {
                // No eligible bots - check for auto-transition
                if (game.status === GAME_STATUS.PLAYING &&
                    game.table_battles.length > 0 && game.table_battles.every(battle => battle.defense !== null)) {
                    console.log('Checking if game should auto-transition from WAIT_FOR_ATTACKERS');

                    // Use the same logic as good.ts to check if all attackers are done
                    const playable_players = game.players.filter(player =>
                        player.player_id !== game.players[game.defender].player_id &&
                        player.status !== PLAYER_STATUS.OUT &&
                        player.hand.some(card => game.table_battles.some(battle => battle.attack.value === card.value || (battle.defense && battle.defense.value === card.value))) &&
                        player.awaiting_attack &&
                        !player.done_attacking_this_round);

                    if (playable_players.length === 0) {
                        console.log(`Auto-transitioning game ${game_id} from WAIT_FOR_ATTACKERS - all attackers done`);

                        // Execute the same logic as good.ts
                        // Find any non-defender player to trigger the good logic
                        const anyNonDefender = game.players.find((p, index) => index !== game.defender);
                        if (anyNonDefender) {
                            const goodEvents = await handleGood(game, anyNonDefender.player_id);
                            // Add the good events to the animation manager
                            for (const event of goodEvents) {
                                animationEvents.addEvent(event);
                            }

                            // Add animation event for auto-transition
                            animationEvents.addMagicTransitionEvent('All attackers finished - automatically proceeding to next round');

                            // Check if we should continue after auto-transition
                            if (game.status === GAME_STATUS.PLAYING && game.players[game.first_attacker].is_ai) {
                                shouldAutoTransition = true;
                            }
                        }
                    } else {
                        console.log(`${playable_players.length} players still have moves available, not auto-transitioning`);
                    }
                } else {
                    console.log(`No eligible bots found for game ${game_id}, ending bot processing cycle`);
                }
            }

            return { game, events: [] };
        });

        // Broadcast animation events AFTER releasing the lock (no lock needed for broadcasting)
        const events = animationEvents.getEvents();
        if (events.length > 0 && game) {
            try {
                // Broadcasting doesn't require a lock since we're not mutating game state
                // Use the captured game state from the main lock
                await broadcastAnimationEvents(game, events);
            } catch (error) {
                console.error('Error broadcasting bot animation events:', error);
            }
            animationEvents.clear();
        }
    } catch (error) {
        console.error('Error in bot processing:', error);
        return;
    }

    // Continue the loop if a bot was processed or auto-transition occurred
    if (botProcessed || shouldAutoTransition) {
        return await processBotActions(game_id, cycle + 1);
    }
}

// Determine if a bot should act given current game state
function shouldBotActCore(game: Game, bot: PrivatePlayer, botIndex: number): boolean {
    if (game.status !== GAME_STATUS.PLAYING) {
        console.log(`Bot ${bot.name} should act: false (not in playing state)`);
        return false;
    }

    const isFirstAttack = game.table_battles.length === 0;
    const isDefender = botIndex === game.defender;
    const allAttacksCovered = game.table_battles.every(battle => battle.defense !== null);

    let shouldAct = false;
    let reason = '';

    if (isFirstAttack) {
        // First attack: only first attacker can act
        shouldAct = botIndex === game.first_attacker;
        reason = shouldAct ? 'is first attacker' : 'not first attacker';
    } else if (isDefender) {
        // Defender can always act when there are attacks
        shouldAct = true;
        reason = 'is defender';
    } else if (allAttacksCovered) {
        // All attacks covered: attackers can attack or say good
        shouldAct = bot.awaiting_attack && !bot.done_attacking_this_round;
        reason = shouldAct ? 'awaiting attack and not done' : `awaiting_attack=${bot.awaiting_attack}, done_attacking=${bot.done_attacking_this_round}`;
    } else {
        // Some attacks uncovered: attackers can still attack
        shouldAct = bot.awaiting_attack && !bot.done_attacking_this_round;
        reason = shouldAct ? 'awaiting attack and not done' : `awaiting_attack=${bot.awaiting_attack}, done_attacking=${bot.done_attacking_this_round}`;
    }

    console.log(`Bot ${bot.name} should act: ${shouldAct} (${reason})`);
    return shouldAct;
}

// Process a single bot's action
async function processBotAction(game: Game, bot: PrivatePlayer): Promise<void> {
    try {
        // Get bot's strategy
        const botData = await getBotData(bot.player_id);
        if (!botData) {
            console.error(`Bot data not found for ${bot.player_id}`);
            return;
        }

        console.log(`Bot ${bot.name} using strategy ${botData.strategy_key}`);
        const strategy = getBotStrategy(botData.strategy_key);

        // Calculate legal moves for this bot
        const legalMoves = calculateLegalMoves(game, bot.player_id);

        if (legalMoves.length === 0) {
            console.log(`No legal moves for bot ${bot.name}`);
            return;
        }

        // Let the strategy choose a move
        const chosenMove = await strategy.chooseMove(game, bot.player_id, legalMoves);
        console.log(`Chosen move: ${JSON.stringify(chosenMove)}`);

        // Execute the chosen move using shared actions (skip validation since bots choose valid moves)
        await executeBotMove(game, bot, chosenMove);

        console.log(`Bot ${bot.name} completed ${chosenMove.type} action`);

    } catch (error) {
        console.error(`Error processing bot action for ${bot.name}:`, error);
    }
}

// Execute a bot's chosen move using shared action handlers
async function executeBotMove(game: Game, bot: PrivatePlayer, move: LegalMove): Promise<void> {
    try {
        let specialMessage: string | undefined;
        let actionEvents: AnimationEvent[] = [];

        switch (move.type) {
            case 'attack':
                // Capture the game status before executing the attack
                const statusBeforeAttack = game.status;
                actionEvents = await executeAttack(game, bot.player_id, move.cards!);
                // Set done_attacking_this_round flag based on the move's choice
                if (move.done_attacking_this_round !== undefined) {
                    bot.done_attacking_this_round = move.done_attacking_this_round;
                    // If bot is done attacking this round, set awaiting_attack = false
                    if (move.done_attacking_this_round) {
                        bot.awaiting_attack = false;
                        console.log(`Bot ${bot.name} is done attacking this round`);

                        // Only try to execute "good" logic if all attacks are covered
                        // Use handleGood for proper validation
                        if (game.table_battles.length > 0 && game.table_battles.every(battle => battle.defense !== null)) {
                            try {
                                console.log(`Bot ${bot.name} attempting good logic`);
                                const goodEvents = await handleGood(game, bot.player_id);
                                actionEvents.push(...goodEvents);
                                console.log(`Bot ${bot.name} successfully executed good logic`);
                            } catch (error) {
                                console.log(`Bot ${bot.name} good logic failed validation: ${error.message}`);
                                // Bot is still marked as done attacking this round, which is correct
                            }
                        }
                    }
                }
                break;

            case 'cover':
                actionEvents = await executeCover(game, bot.player_id, move.cards!, move.attack_cards!, true);
                // Check if this cover completed the round (all attacks covered)
                const all_attacks_covered = game.table_battles.every(battle => battle.defense !== null);
                if (all_attacks_covered) {
                    // Add special message for successful cover that ends the round
                    specialMessage = `Bot ${bot.name} successfully covered and ended the round`;
                }
                break;

            case 'pass':
                actionEvents = await executePass(game, bot.player_id, move.cards!);
                // Set done_attacking_this_round flag based on the move's choice
                if (move.done_attacking_this_round !== undefined) {
                    bot.done_attacking_this_round = move.done_attacking_this_round;
                    // If bot is done attacking this round, set awaiting_attack = false
                    if (move.done_attacking_this_round) {
                        bot.awaiting_attack = false;
                        console.log(`Bot ${bot.name} is done attacking this round after pass`);

                        // Only try to execute "good" logic if all attacks are covered
                        // Use handleGood for proper validation
                        if (game.table_battles.length > 0 && game.table_battles.every(battle => battle.defense !== null)) {
                            try {
                                console.log(`Bot ${bot.name} attempting good logic after pass`);
                                const goodEvents = await handleGood(game, bot.player_id);
                                actionEvents.push(...goodEvents);
                                console.log(`Bot ${bot.name} successfully executed good logic after pass`);
                            } catch (error) {
                                console.log(`Bot ${bot.name} good logic failed validation after pass: ${error.message}`);
                                // Bot is still marked as done attacking this round, which is correct
                            }
                        }
                    }
                }
                break;

            case 'pickup':
                actionEvents = await executePickup(game, bot.player_id);
                break;

            case 'good':
                actionEvents = await handleGood(game, bot.player_id);
                break;
        }

        console.log(`Bot ${bot.name} performed ${move.type} action`);

        // Add all events from the action to the animation manager
        for (const event of actionEvents) {
            animationEvents.addEvent(event);
        }

    } catch (error) {
        console.error(`Error executing bot move for ${bot.name}:`, error);
    }
}

// Get bot data from database
async function getBotData(botId: string): Promise<Bot | null> {
    try {
        const { data, error } = await supabaseClient
            .from('bots')
            .select('*')
            .eq('id', botId)
            .single();

        if (error) {
            return null;
        }

        return data;
    } catch (error) {
        return null;
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

