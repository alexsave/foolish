import { Game, PrivatePlayer, Bot, GAME_STATUS } from './types.ts';
import { loadCompleteGame, saveCompleteGame, broadcastToGameUsers, executeWithGameLock } from './utils.ts';
import { calculateLegalMoves, getBotStrategy, LegalMove } from './bot_strategy.ts';
import { createClient } from 'jsr:@supabase/supabase-js';

// Import shared action handlers
import { executeAttack } from './actions/attack.ts';
import { executeCover } from './actions/cover.ts';
import { executePass } from './actions/pass.ts';
import { executePickup } from './actions/pickup.ts';
import { executeGood } from './actions/good.ts';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// Process bot responses for a game after a user action
export async function processBotActions(game_id: string): Promise<void> {
    try {
        // Small delay to ensure user action is fully processed
        await new Promise(resolve => setTimeout(resolve, 1000));
        
        // Process bot actions for this game - each bot will load its own fresh game state
        await processBotActionsForGame(game_id);
        
    } catch (error) {
        console.error('Error processing bot actions:', error);
    }
}

// Process bot actions for a specific game
async function processBotActionsForGame(game_id: string): Promise<void> {
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
                shouldConsider = index === game!.first_attacker; // First bot found
                break;
                
            case GAME_STATUS.FREE_PLAY:
            case GAME_STATUS.ONLY_DEFEND:
            case GAME_STATUS.WAIT_FOR_ATTACKERS:
                // All bots could potentially act (defender or attackers)
                shouldConsider = true;
                break;
        }
        
        if (shouldConsider) {
            eligibleBots.push(player);
        }
    });
    
    if (eligibleBots.length === 0) {
        return; // No eligible bots
    }
    
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
                console.log(`Bot cycle completed, scheduling next bot action cycle`);
                scheduleBotActions(game_id);
            } else {
                console.log(`Bot cycle completed, no more bot moves available`);
            }
        });
    } catch (error) {
        console.error('Error checking for additional bot actions:', error);
    }
}

// Determine if a bot should act given current game state
async function shouldBotAct(game: Game, bot: PrivatePlayer): Promise<boolean> {
    const botIndex = game.players.indexOf(bot);
    
    // If bot is done attacking this round, they should not act as an attacker
    if (bot.done_attacking_this_round && botIndex !== game.defender) {
        return false;
    }
    
    switch (game.status) {
        case GAME_STATUS.FIRST_ATTACKER:
            // Bot should act if it's the first attacker
            return botIndex === game.first_attacker;
            
        case GAME_STATUS.FREE_PLAY:
            // Bot should act if it's the defender or an attacker (unless done for this round)
            return botIndex === game.defender || botIndex !== game.defender;
            
        case GAME_STATUS.ONLY_DEFEND:
            // Bot should act if it's the defender
            return botIndex === game.defender;
            
        case GAME_STATUS.WAIT_FOR_ATTACKERS:
            // Bot should act if it's an attacker (can attack or confirm done)
            return botIndex !== game.defender;
            
        default:
            return false;
    }
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
        console.log(`Calculating legal moves for bot ${bot.name}`);
        const legalMoves = calculateLegalMoves(game, bot.player_id);
        console.log(`Legal moves: ${legalMoves.length}`);
        console.log(`Legal moves: ${JSON.stringify(legalMoves)}`);
        
        if (legalMoves.length === 0) {
            console.log(`No legal moves for bot ${bot.name}`);
            return;
        }
        
        // Let the strategy choose a move
        const chosenMove = await strategy.chooseMove(game, bot.player_id, legalMoves);
        console.log(`Chosen move: ${chosenMove.type}`);
        
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
                await executeAttack(game, bot.player_id, move.cards!);
                // Set done_attacking_this_round flag based on the move's choice
                if (move.done_attacking_this_round !== undefined) {
                    bot.done_attacking_this_round = move.done_attacking_this_round;
                }
                break;
                
            case 'cover':
                await executeCover(game, bot.player_id, move.cards!, move.attack_cards!);
                break;
                
            case 'pass':
                await executePass(game, bot.player_id, move.cards!);
                break;
                
            case 'pickup':
                executePickup(game, bot.player_id);
                break;
                
            case 'good':
                executeGood(game, bot.player_id);
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
                shouldAct = true; // Both defenders and attackers can act
                break;
                
            case GAME_STATUS.ONLY_DEFEND:
                shouldAct = botIndex === game.defender;
                break;
                
            case GAME_STATUS.WAIT_FOR_ATTACKERS:
                shouldAct = botIndex !== game.defender;
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
export function scheduleBotActions(game_id: string): void {
    // Run bot actions in the background with a delay
    setTimeout(() => {
        processBotActions(game_id).catch(error => {
            console.error('Error in scheduled bot actions:', error);
        });
    }, 5000); // 5 second delay as requested
} 