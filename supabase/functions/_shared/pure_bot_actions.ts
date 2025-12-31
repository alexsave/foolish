// version of bot_actions that doesn't use util.ts
// that has a whole lot of baggage in it
// Import shared action handlers with validation
import { Game, PrivatePlayer, Bot, GAME_STATUS, PLAYER_STATUS } from './types.ts';
import { calculateLegalMoves, getBotStrategy, LegalMove } from './bot_strategy.ts';
import { handleAttack } from './actions/attack.ts';
import { handleCover } from './actions/cover.ts';
import { handlePass } from './actions/pass.ts';
import { handlePickup } from './actions/pickup.ts';
import { handleGood } from './actions/good.ts';
import { AnimationEvent } from './types.ts';

// Process a single bot's action
export const processBotAction = async (game: Game, bot: PrivatePlayer): Promise<false | AnimationEvent[]> => {
    try {
        const botActionStartTime = Date.now();
        
        // Get bot's strategy
        console.log(`Bot ${bot.name} using strategy ${bot.strategy_key}`);
        const strategy = getBotStrategy(bot.strategy_key);

        // Calculate legal moves for this bot
        const legalMovesStart = Date.now();
        const legalMoves = calculateLegalMoves(game, bot.player_id);
        //console.log(`[TIMING] calculateLegalMoves took ${Date.now() - legalMovesStart}ms (${legalMoves.length} moves)`);

        if (legalMoves.length === 0) {
            console.log(`No legal moves for bot ${bot.name}`);
            return false;
        }

        // Let the strategy choose a move
        const chooseMoveStart = Date.now();
        // This is ok to keep async, we might be calling LLMs later
        const chosenMove = await strategy.chooseMove(game, bot.player_id, legalMoves);
        //console.log(`[TIMING] strategy.chooseMove took ${Date.now() - chooseMoveStart}ms`);
        console.log(`Chosen move: ${JSON.stringify(chosenMove)}`);

        // Execute the chosen move using shared actions (with validation to handle race conditions)
        const executeMoveStart = Date.now();
        const actionEvents = executeBotMove(game, bot, chosenMove);
        //console.log(`[TIMING] executeBotMove took ${Date.now() - executeMoveStart}ms`);

        if (!actionEvents) {
            return false;
        }

        const totalBotActionTime = Date.now() - botActionStartTime;
        //console.log(`[TIMING] Total bot action time for ${bot.name}: ${totalBotActionTime}ms`);
        console.log(`Bot ${bot.name} completed ${chosenMove.type} action`);
        return actionEvents;

    } catch (error) {
        console.error(`Error processing bot action for ${bot.name}:`, error);
        return false;
    }
}

// Execute a bot's chosen move using shared action handlers
export const executeBotMove = (game: Game, bot: PrivatePlayer, move: LegalMove): false | AnimationEvent[] => {
    try {
        let specialMessage: string | undefined;
        let actionEvents: AnimationEvent[] = [];

        try {
            const actionHandlerStart = Date.now();
            //console.log(`[TIMING] Executing ${move.type} action...`);
            
            switch (move.type) {
                case 'attack':
                    // Capture the game status before executing the attack
                    const statusBeforeAttack = game.status;
                    actionEvents = handleAttack(game, bot.player_id, move.cards!);
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
                                    const goodEvents = handleGood(game, bot.player_id);
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
                    actionEvents = handleCover(game, bot.player_id, move.cards!, move.attack_cards!);
                    // Check if this cover completed the round (all attacks covered)
                    const all_attacks_covered = game.table_battles.every(battle => battle.defense !== null);
                    if (all_attacks_covered) {
                        // Add special message for successful cover that ends the round
                        specialMessage = `Bot ${bot.name} successfully covered and ended the round`;
                    }
                    break;

                case 'pass':
                    actionEvents = handlePass(game, bot.player_id, move.cards!);
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
                                    const goodEvents = handleGood(game, bot.player_id);
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
                    actionEvents = handlePickup(game, bot.player_id);
                    break;

                case 'good':
                    actionEvents = handleGood(game, bot.player_id);
                    break;

                case 'wait':
                    // Bot chooses to wait - no action needed, just log it
                    console.log(`Bot ${bot.name} chose to wait`);
                    break;
            }

            //console.log(`[TIMING] Action handler (${move.type}) completed in ${Date.now() - actionHandlerStart}ms`);
            //console.log(`Bot ${bot.name} performed ${move.type} action`);

            // Add all events from the action to the animation manager
            const addEventsStart = Date.now();
            for (const event of actionEvents) {
                // FUCK. i don't know what to do with this.
                // TODO uncomment and return up to bot_actions.ts
                // animationEvents.addEvent(event);
            }
            //console.log(`[TIMING] Adding ${actionEvents.length} events took ${Date.now() - addEventsStart}ms`);

        } catch (error) {
            // Handle validation failures gracefully (e.g., due to race conditions)
            console.log(`Bot ${bot.name} move validation failed: ${error.stack} - this can happen due to race conditions and is normal`);
            return false;
        }

        return actionEvents;

    } catch (error) {
        console.error(`Error executing bot move for ${bot.name}:`, error);
        return false;
    }
}
// Determine if a bot should act given current game state
export const shouldBotActCore = (game: Game, bot: PrivatePlayer, botIndex: number): boolean => {
    if (game.status !== GAME_STATUS.PLAYING) {
        console.log(`Bot ${bot.name} should act: false (not in playing state)`);
        return false;
    }

    // Check if bot is out - they should never act
    if (bot.status !== PLAYER_STATUS.IN) {
        console.log(`Bot ${bot.name} should act: false (status is ${bot.status}, not IN)`);
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