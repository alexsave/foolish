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
import { cardDisplay } from './common_utils.ts';

// Process a single bot's action
export const processBotAction = async (game: Game, bot: PrivatePlayer): Promise<false | { events: AnimationEvent[], moveType: string }> => {
    try {
        const botActionStartTime = Date.now();
        
        // Get bot's strategy
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

        // Execute the chosen move using shared actions (with validation to handle race conditions)
        const executeMoveStart = Date.now();
        const actionEvents = executeBotMove(game, bot, chosenMove);
        //console.log(`[TIMING] executeBotMove took ${Date.now() - executeMoveStart}ms`);

        if (!actionEvents) {
            return false;
        }

        const totalBotActionTime = Date.now() - botActionStartTime;
        //console.log(`[TIMING] Total bot action time for ${bot.name}: ${totalBotActionTime}ms`);
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
                    console.log(`⚔️  ${bot.name} attacked with ${move.cards!.map(c => cardDisplay(c)).join(', ')}`);
                    break;

                case 'cover':
                    actionEvents = handleCover(game, bot.player_id, move.cards!, move.attack_cards!);
                    console.log(`🛡️  ${bot.name} covered ${move.attack_cards!.map(c => cardDisplay(c)).join(', ')} with ${move.cards!.map(c => cardDisplay(c)).join(', ')}`);
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
                    console.log(`↪️  ${bot.name} passed with ${move.cards!.map(c => cardDisplay(c)).join(', ')}`);
                    break;

                case 'pickup':
                    actionEvents = handlePickup(game, bot.player_id);
                    console.log(`📥 ${bot.name} picked up`);
                    break;

                case 'good':
                    actionEvents = handleGood(game, bot.player_id);
                    // Log is already in handleGood with waiting conditions details
                    break;

                case 'wait':
                    // Bot chooses to wait - no action needed
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
        return false;
    }

    // Check if bot is out - they should never act
    if (bot.status !== PLAYER_STATUS.IN) {
        return false;
    }

    const isFirstAttack = game.table_battles.length === 0;
    const isDefender = botIndex === game.defender;
    // Note: every() returns true for empty arrays, so check length first
    const allAttacksCovered = game.table_battles.length > 0 && 
        game.table_battles.every(battle => battle.defense !== null);

    if (isFirstAttack) {
        // First attack: only first attacker can act
        return botIndex === game.first_attacker;
    }
    
    if (isDefender) {
        // Defender can only act when there are uncovered attacks
        // If all attacks are covered, defender just waits for attackers to add more or say "good"
        return !allAttacksCovered;
    }
    
    // Attacker: check if they've already said "good"
    const hasPlayerSaidGood = game.good_players?.includes(bot.player_id) || false;
    if (hasPlayerSaidGood) {
        return false;
    }
    // Attacker can act if awaiting attack
    return bot.awaiting_attack;
}