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
export const processBotAction = async (game: Game, bot: PrivatePlayer): Promise<false | { events: AnimationEvent[], moveType: string }> => {
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
        return { events: actionEvents, moveType: chosenMove.type };

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
                    actionEvents = handleAttack(game, bot.player_id, move.cards!);
                    break;

                case 'cover':
                    actionEvents = handleCover(game, bot.player_id, move.cards!, move.attack_cards!);
                    // Check if this cover completed the round (all attacks covered)
                    // Note: every() returns true for empty arrays, but we should have battles after covering
                    const all_attacks_covered = game.table_battles.length > 0 && 
                        game.table_battles.every(battle => battle.defense !== null);
                    if (all_attacks_covered) {
                        // Add special message for successful cover that ends the round
                        specialMessage = `Bot ${bot.name} successfully covered and ended the round`;
                    }
                    break;

                case 'pass':
                    actionEvents = handlePass(game, bot.player_id, move.cards!);
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
    // Note: every() returns true for empty arrays, so check length first
    const allAttacksCovered = game.table_battles.length > 0 && 
        game.table_battles.every(battle => battle.defense !== null);

    let shouldAct = false;
    let reason = '';

    if (isFirstAttack) {
        // First attack: only first attacker can act
        shouldAct = botIndex === game.first_attacker;
        reason = shouldAct ? 'is first attacker' : 'not first attacker';
    } else if (isDefender) {
        // Defender can act when there are attacks
        // EXCEPT: if all attacks are covered AND all attackers have said "good"
        // In that case, auto_discard_loop will handle round transition
        if (allAttacksCovered) {
            const allAttackers = game.players.filter((p, index) => 
                index !== game.defender && p.status === PLAYER_STATUS.IN
            );
            // Note: every() returns true for empty arrays, so check length first
            const allAttackersSaidGood = allAttackers.length > 0 && allAttackers.every(attacker => 
                game.good_players?.includes(attacker.player_id)
            );
            
            if (allAttackersSaidGood) {
                // All attackers said good and all attacks covered - auto_discard_loop will handle this
                shouldAct = false;
                reason = 'all attackers said good and attacks covered - waiting for auto-discard';
            } else {
                // Some attackers haven't said good yet, or attacks not all covered
                shouldAct = true;
                reason = 'is defender';
            }
        } else {
            // Not all attacks covered - defender can act
            shouldAct = true;
            reason = 'is defender with uncovered attacks';
        }
    } else {
        // Attacker: check if they've already said "good"
        const hasPlayerSaidGood = game.good_players?.includes(bot.player_id) || false;
        
        if (hasPlayerSaidGood) {
            shouldAct = false;
            reason = 'already said good';
        } else if (allAttacksCovered) {
            // All attacks covered: attackers can attack or say good
            shouldAct = bot.awaiting_attack;
            reason = shouldAct ? 'awaiting attack' : `awaiting_attack=${bot.awaiting_attack}`;
        } else {
            // Some attacks uncovered: attackers can still attack
            shouldAct = bot.awaiting_attack;
            reason = shouldAct ? 'awaiting attack' : `awaiting_attack=${bot.awaiting_attack}`;
        }
    }

    console.log(`Bot ${bot.name} should act: ${shouldAct} (${reason})`);
    return shouldAct;
}