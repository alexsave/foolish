import { Game, PrivatePlayer, Bot, GAME_STATUS } from './types.ts';
import { loadCompleteGame, saveCompleteGame, broadcastToGameUsers, executeWithGameLock } from './utils.ts';
import { calculateLegalMoves, getBotStrategy, LegalMove } from './bot_strategy.ts';
import { createClient } from 'jsr:@supabase/supabase-js';

// Import shared action handlers
import { executeAttack } from './actions/attack.ts';
import { executeCover } from './actions/cover.ts';
import { executePass } from './actions/pass.ts';
import { executePickup } from './actions/pickup.ts';
import { executeGood, handleGood } from './actions/good.ts';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// Process bot responses for a game after a user action
export async function processBotActions(game_id: string, cycle: number = 0): Promise<void> {
    //try {
        // Small delay to ensure user action is fully processed
        
        // Process bot actions for this game - each bot will load its own fresh game state
        //await processBotActionsForGame(game_id, cycle);
        
    //} catch (error) {
        //console.error('Error processing bot actions:', error);
    //}
//}

// Process bot actions for a specific game
//async function processBotActionsForGame(game_id: string, cycle: number = 0): Promise<void> {
    await new Promise(resolve => setTimeout(resolve, 1000));
    // Load initial game state to get bot and game info
    let game: Game | null = null;
    let players: any[] = [];
    let gameStatus: any = null;
    
    try {
        await executeWithGameLock(game_id, async () => {
            game = await loadCompleteGame(game_id);
            
            // Only process bot actions if game is in a state where bots can act
            if (game.status === GAME_STATUS.WAITING) {
                return; // No bot actions needed in waiting state
            }
            
            players = game.players;
            gameStatus = game.status;
        });
    } catch (error) {
        console.error('Error loading game state for bot processing:', error);
        return;
    }
    
    if (gameStatus === GAME_STATUS.WAITING || players.filter(p => p.is_ai).length === 0) {
        return; // No bot actions needed
    }
    
    // Find all bots that should potentially act this round
    const eligibleBots: any[] = [];
    
    // Add bots based on game state
    players.forEach((player, index) => {
        if (!player.is_ai) return;
        
        // Check if this bot should potentially act based on game state
        let shouldConsider = false;
        
        switch (gameStatus) {
            case GAME_STATUS.FIRST_ATTACKER:
                // Only the first attacker bot should act
                shouldConsider = index === game!.first_attacker;
                break;
                
            case GAME_STATUS.FREE_PLAY:
                // Defender should always be considered, attackers only if awaiting_attack = true and not done attacking
                if (index === game!.defender) {
                    shouldConsider = true;
                } else {
                    shouldConsider = player.awaiting_attack && !player.done_attacking_this_round;
                }
                break;
                
            case GAME_STATUS.ONLY_DEFEND:
                // Only the defender bot should act
                shouldConsider = index === game!.defender;
                break;
                
            case GAME_STATUS.WAIT_FOR_ATTACKERS:
                // Only attackers with awaiting_attack = true should be considered
                shouldConsider = index !== game!.defender && player.awaiting_attack;
                break;
        }
        
        if (shouldConsider) {
            eligibleBots.push(player);
        }
    });
    
    if (eligibleBots.length === 0) {
        console.log(`No eligible bots found for game ${game_id} in status ${gameStatus}`);
        return; // No eligible bots
    }
    
    console.log(`Found ${eligibleBots.length} eligible bots for game ${game_id}:`, eligibleBots.map(bot => ({
        name: bot.name,
        awaiting_attack: bot.awaiting_attack,
        done_attacking_this_round: bot.done_attacking_this_round
    })));
    
    // Randomize the order of eligible bots for this round
    // This simulates real-life "first come, first serve" gameplay
    const shuffledBots = [...eligibleBots];
    for (let i = shuffledBots.length - 1; i > 0; i--) {
        const j = Math.floor(Math.random() * (i + 1));
        [shuffledBots[i], shuffledBots[j]] = [shuffledBots[j], shuffledBots[i]];
    }
    
    console.log(`Processing ${shuffledBots.length} bots in randomized order: ${shuffledBots.map(b => b.name).join(', ')}`);
    
    // Process bots in randomized order
    const processedBots = new Set<string>();
    
    for (const player of shuffledBots) {
        // Skip if already processed (shouldn't happen but safety check)
        if (processedBots.has(player.player_id)) {
            continue;
        }
        
        processedBots.add(player.player_id);
        
        // Each bot gets its own game lock to allow human players to act between bots
        try {
            await executeWithGameLock(game_id, async () => {
                // Reload game state to get latest state before bot action
                const currentGame = await loadCompleteGame(game_id);
                const currentBot = currentGame.players.find(p => p.player_id === player.player_id);
                
                if (!currentBot) {
                    console.log(`Bot ${player.name} not found in current game state`);
                    return;
                }
                
                const shouldAct = await shouldBotAct(currentGame, currentBot);
                
                if (shouldAct) {
                    await processBotAction(currentGame, currentBot);
                }
            });
            
            // Small delay between bot actions to make it feel more natural
            await new Promise(resolve => setTimeout(resolve, 2000));
            
        } catch (error) {
            console.error(`Error processing bot action for ${player.name}:`, error);
        }
    }
    
    // After processing all bots in this cycle, check if any more bot actions are needed
    // This check needs to be done within a lock to ensure safe game state access
    try {
        await executeWithGameLock(game_id, async () => {
            const currentGame = await loadCompleteGame(game_id);
            const anyBotHasLegalMoves = checkIfAnyBotHasLegalMoves(currentGame);
            
            if (anyBotHasLegalMoves) {
                console.log(`Bot cycle completed, scheduling next bot action cycle for game ${game_id}`);
                
                // Log bot states for debugging
                const bots = currentGame.players.filter(p => p.is_ai);
                const botStates = bots.map(bot => `${bot.name}(${bot.awaiting_attack ? 'awaiting' : 'not awaiting'}, ${bot.done_attacking_this_round ? 'done' : 'continuing'}, ${bot.hand.length} cards)`).join(', ');
                console.log(`Bot states: ${botStates}`);
                
                scheduleBotActions(game_id, cycle + 1);
            } else {
                console.log(`Bot cycle completed, no more bot moves available for game ${game_id}`);
            }
        });
    } catch (error) {
        console.error('Error checking for additional bot actions:', error);
    }
}

// Determine if a bot should act given current game state
async function shouldBotAct(game: Game, bot: PrivatePlayer): Promise<boolean> {
    const botIndex = game.players.indexOf(bot);
    let shouldAct = false;
    let reason = '';
    
    switch (game.status) {
        case GAME_STATUS.FIRST_ATTACKER:
            // Bot should act if it's the first attacker
            shouldAct = botIndex === game.first_attacker;
            reason = shouldAct ? 'is first attacker' : 'not first attacker';
            break;
            
        case GAME_STATUS.FREE_PLAY:
            // Bot should act if it's the defender OR if it's an attacker with awaiting_attack = true and not done attacking this round
            if (botIndex === game.defender) {
                shouldAct = true;
                reason = 'is defender';
            } else {
                // For attackers, check if they're awaiting attack and not done attacking this round
                shouldAct = bot.awaiting_attack && !bot.done_attacking_this_round;
                reason = shouldAct ? 'awaiting attack and not done' : `awaiting_attack=${bot.awaiting_attack}, done_attacking=${bot.done_attacking_this_round}`;
            }
            break;
            
        case GAME_STATUS.ONLY_DEFEND:
            // Bot should act if it's the defender
            shouldAct = botIndex === game.defender;
            reason = shouldAct ? 'is defender' : 'not defender';
            break;
            
        case GAME_STATUS.WAIT_FOR_ATTACKERS:
            // Bot should act if it's an attacker with awaiting_attack = true
            shouldAct = botIndex !== game.defender && bot.awaiting_attack;
            reason = shouldAct ? 'is attacker awaiting attack' : `is_defender=${botIndex === game.defender}, awaiting_attack=${bot.awaiting_attack}`;
            break;
            
        default:
            shouldAct = false;
            reason = 'unknown game status';
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
        
        // Save the updated game state after the bot action
        await saveCompleteGame(game);
        
        // Broadcast the updated game state to all players
        await broadcastToGameUsers(game, 'game_update', {
            type: 'bot_action',
            message: `Bot ${bot.name} performed ${chosenMove.type} action`,
            bot_name: bot.name,
            action_type: chosenMove.type
        });
        
        console.log(`Bot ${bot.name} completed ${chosenMove.type} action`);
        
    } catch (error) {
        console.error(`Error processing bot action for ${bot.name}:`, error);
    }
}

// Execute a bot's chosen move using shared action handlers
async function executeBotMove(game: Game, bot: PrivatePlayer, move: LegalMove): Promise<void> {
    try {
        switch (move.type) {
            case 'attack':
                // Capture the game status before executing the attack
                const statusBeforeAttack = game.status;
                await executeAttack(game, bot.player_id, move.cards!);
                // Set done_attacking_this_round flag based on the move's choice
                if (move.done_attacking_this_round !== undefined) {
                    bot.done_attacking_this_round = move.done_attacking_this_round;
                    // If bot is done attacking this round, set awaiting_attack = false
                    if (move.done_attacking_this_round) {
                        bot.awaiting_attack = false;
                        console.log(`Bot ${bot.name} is done attacking this round`);
                        
                        // Only try to execute "good" logic if game is now in WAIT_FOR_ATTACKERS mode
                        // Use handleGood for proper validation
                        if (game.status === GAME_STATUS.WAIT_FOR_ATTACKERS) {
                            try {
                                console.log(`Bot ${bot.name} attempting good logic`);
                                await handleGood(game, bot.player_id);
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
                await executeCover(game, bot.player_id, move.cards!, move.attack_cards!);
                break;
                
            case 'pass':
                await executePass(game, bot.player_id, move.cards!);
                break;
                
            case 'pickup':
                await executePickup(game, bot.player_id);
                break;
                
            case 'good':
                await handleGood(game, bot.player_id);
                break;
        }
        
        console.log(`Bot ${bot.name} performed ${move.type} action`);
        
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
            console.error('Error fetching bot data:', error);
            return null;
        }
        
        return data;
    } catch (error) {
        console.error('Error in getBotData:', error);
        return null;
    }
}

// Check if any bots in the game have legal moves available
function checkIfAnyBotHasLegalMoves(game: Game): boolean {
    // Find all bots in the game
    const bots = game.players.filter(player => player.is_ai);
    
    if (bots.length === 0) {
        return false; // No bots to check
    }
    
    // Check each bot to see if they should act and have legal moves
    for (const bot of bots) {
        const botIndex = game.players.indexOf(bot);
        let shouldAct = false;
        
        // Determine if this bot should act in current game state
        switch (game.status) {
            case GAME_STATUS.FIRST_ATTACKER:
                shouldAct = botIndex === game.first_attacker;
                break;
                
            case GAME_STATUS.FREE_PLAY:
                // Bot should act if it's the defender OR if it's an attacker with awaiting_attack = true
                if (botIndex === game.defender) {
                    shouldAct = true;
                } else {
                    // For attackers, check if they're awaiting attack and not done attacking this round
                    shouldAct = bot.awaiting_attack && !bot.done_attacking_this_round;
                }
                break;
                
            case GAME_STATUS.ONLY_DEFEND:
                shouldAct = botIndex === game.defender;
                break;
                
            case GAME_STATUS.WAIT_FOR_ATTACKERS:
                // Bot should act if it's an attacker with awaiting_attack = true
                shouldAct = botIndex !== game.defender && bot.awaiting_attack;
                break;
                
            default:
                shouldAct = false;
        }
        
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

// Schedule bot actions to run after user action (called from wrap400)
export function scheduleBotActions(game_id: string, cycle: number = 0): void {
    // Check if we've had too many bot action cycles recently
    if (cycle >= 100) {
        console.warn(`Bot action cycle limit reached for game ${game_id}, stopping to prevent infinite loop`);
        return;
    }
    
    // Run bot actions in the background with a delay
    setTimeout(() => {
        processBotActions(game_id, cycle).catch(error => {
            console.error('Error in scheduled bot actions:', error);
        });
    }, 5000); // 5 second delay as requested
} 