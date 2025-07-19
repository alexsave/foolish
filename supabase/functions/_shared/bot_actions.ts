import { Game, PrivatePlayer, Bot, GAME_STATUS, PLAYER_STATUS } from './types.ts';
import { check_win, broadcastToGameUsers, executeWithGameLock, broadcastAnimationEvents, animationEvents } from './utils.ts';
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
                console.error(`Failed to acquire lock for game ${game_id}:`, error);
                return false;
            }
            
            // Check if existing lock is older than 150 seconds
            const { data: existingLock } = await supabaseClient
                .from('bot_locks')
                .select('acquired_at')
                .eq('game_id', game_id)
                .single();
            
            if (!existingLock) {
                console.log(`Bot loop already running for game ${game_id}`);
                return false;
            }
            
            const lockAge = Date.now() - new Date(existingLock.acquired_at).getTime();
            if (lockAge <= 150000) { // 150 seconds in milliseconds
                console.log(`Bot loop already running for game ${game_id}`);
                return false;
            }
            
            console.log(`Removing stale lock for game ${game_id} (${Math.round(lockAge/1000)}s old)`);
            
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
                console.log(`Failed to acquire lock after stale cleanup for game ${game_id}:`, retryError);
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
            console.log(`Lock verification failed for game ${game_id} - another instance won the race`);
            return false;
        }
        
        console.log(`Acquired bot loop lock for game ${game_id} with lock_id ${lockId}`);
        return true;
    } catch (error) {
        console.error(`Error acquiring lock for game ${game_id}:`, error);
        return false;
    }
};

const releaseBotLoopLock = async (game_id: string): Promise<void> => {
    try {
        console.log(`Releasing bot loop lock for game ${game_id}`);

        // Only delete if we have the correct lock_id
        const { error } = await supabaseClient
            .from('bot_locks')
            .delete()
            .eq('game_id', game_id)
            //.eq('lock_id', lockId);
        
        if (error) {
            console.error(`Failed to release lock for game ${game_id}:`, error);
        } else {
            console.log(`Released bot loop lock for game ${game_id}`);
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
        await processBotActions(game_id, 0);   
    } finally {
        await releaseBotLoopLock(game_id);
    }
}


// Process bot responses for a game after a user action
//export async function processBotActions(game_id: string, cycle: number = 0): Promise<void> {
const processBotActions = async (game_id: string, cycle: number = 0): Promise<void> => {
    if (cycle > 100) {
        return;
    }

    let shouldLoop = false;

    await new Promise(resolve => setTimeout(resolve, 1500));
    // Load initial game state to get bot and game info
    let localGame: Game | null = null;
    let players: any[] = [];
    let gameStatus: any = null;
    
    try {
        const result = await executeWithGameLock(game_id, async (game) => {
            localGame = game;
            
            // Only process bot actions if game is in a state where bots can act
            if (game.status === GAME_STATUS.WAITING || game.status === GAME_STATUS.GAME_OVER) {
                return { game, events: [] }; // No bot actions needed in waiting state or game over
            }
            
            // Safety check: if there's only one player left, the game should have ended
            const in_players = game.players.filter(player => player.status === PLAYER_STATUS.IN);
            if (in_players.length <= 1) {
                console.warn(`Bot processing stopped - only ${in_players.length} player(s) left, ending game`);
                await check_win(game);
                return { game, events: [] };
            }
            
            players = game.players;
            gameStatus = game.status;
            return { game, events: [] };
        });
        
        localGame = result.game;
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
    //players.forEach((player, index) => {
    for (let index = 0; index < players.length; index++){
        const player = players[index];
        if (!player.is_ai) continue;
        
        // Check if this bot should potentially act based on game state
        let shouldConsider = false;
        
        if (gameStatus === GAME_STATUS.FIRST_ATTACKER){
            shouldConsider = index === localGame!.first_attacker;
        } else if (gameStatus === GAME_STATUS.FREE_PLAY){
            if (index === localGame!.defender) {
                shouldConsider = true;
            } else {
                shouldConsider = player.awaiting_attack && !player.done_attacking_this_round;
            }
        } else if (gameStatus === GAME_STATUS.ONLY_DEFEND){
            shouldConsider = index === localGame!.defender;
        } else if (gameStatus === GAME_STATUS.WAIT_FOR_ATTACKERS){
            shouldConsider = index !== localGame!.defender && player.awaiting_attack;
        }

        if (shouldConsider) {
            eligibleBots.push(player);
        }
    }
    
    if (eligibleBots.length === 0) {
        console.log(`No eligible bots found for game ${game_id} in status ${gameStatus}`);


        //let shouldLoop = false;
        
        // Special case: if we're in WAIT_FOR_ATTACKERS state and no attackers are awaiting,
        // check if the game should automatically transition to the next phase
        if (gameStatus === GAME_STATUS.WAIT_FOR_ATTACKERS) {
            console.log('Checking if game should auto-transition from WAIT_FOR_ATTACKERS');
            
            try {
                await executeWithGameLock(game_id, async (currentGame) => {
                    // Use the same logic as good.ts to check if all attackers are done
                    const playable_players = currentGame.players.filter(player => 
                        player.player_id !== currentGame.players[currentGame.defender].player_id && 
                        player.status !== PLAYER_STATUS.OUT &&
                        player.hand.some(card => currentGame.table_battles.some(battle => battle.attack.value === card.value || (battle.defense && battle.defense.value === card.value))) &&
                        player.awaiting_attack &&
                        !player.done_attacking_this_round);
                    
                    if (playable_players.length !== 0) {
                        console.log(`${playable_players.length} players still have moves available, not auto-transitioning`);
                        return { game: currentGame, events: [] };
                    }
                    console.log(`Auto-transitioning game ${game_id} from WAIT_FOR_ATTACKERS - all attackers done`);
                    
                    // Execute the same logic as good.ts
                    
                    // Find any non-defender player to trigger the good logic
                    // (the good logic doesn't actually use the player_id for the transition)
                    const anyNonDefender = currentGame.players.find((p, index) => index !== currentGame.defender);
                    if (anyNonDefender) {
                        await executeGood(currentGame, anyNonDefender.player_id);
                        
                        // Add animation event for auto-transition
                        animationEvents.addMagicTransitionEvent('All attackers finished - automatically proceeding to next round');
                        
                        // Schedule bot actions for the new game state
                        if (currentGame.status === GAME_STATUS.FIRST_ATTACKER && currentGame.players[currentGame.first_attacker].is_ai) {
                            // yeah we need to loop
                            shouldLoop = true;
                        }
                    }
                    
                    return { game: currentGame, events: [] };
                });
            } catch (error) {
                console.error('Error in auto-transition check:', error);
            }
        }
        
        if (shouldLoop) {
            // No eligible bots
            return await processBotActions(game_id, cycle+1); 
        }
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
    
    // Clear animation events before processing bots
    animationEvents.clear();
    
    for (const player of shuffledBots) {
        // Skip if already processed (shouldn't happen but safety check)
        if (processedBots.has(player.player_id)) {
            continue;
        }
        
        processedBots.add(player.player_id);
        
        // Each bot gets its own game lock to allow human players to act between bots
        try {
            await executeWithGameLock(game_id, async (currentGame) => {
                // Reload game state to get latest state before bot action
                const currentBot = currentGame.players.find(p => p.player_id === player.player_id);
                
                if (!currentBot) {
                    console.log(`Bot ${player.name} not found in current game state`);
                    return { game: currentGame, events: [] };
                }
                
                const shouldAct = await shouldBotAct(currentGame, currentBot);
                
                if (shouldAct) {
                    await processBotAction(currentGame, currentBot);
                }
                
                return { game: currentGame, events: [] };
            });
            
            // Small delay between bot actions to make it feel more natural
            await new Promise(resolve => setTimeout(resolve, 200));
            
        } catch (error) {
            console.error(`Error processing bot action for ${player.name}:`, error);
        }
    }
    
    // After processing all bots in this cycle, broadcast animation events
    const events = animationEvents.getEvents();
    if (events.length > 0) {
        try {
            await executeWithGameLock(game_id, async (currentGame) => {
                await broadcastAnimationEvents(currentGame, events);
                return { game: currentGame, events: [] };
            });
        } catch (error) {
            console.error('Error broadcasting bot animation events:', error);
        }
    }
    
    // After processing all bots in this cycle, check if any more bot actions are needed
    // This check needs to be done within a lock to ensure safe game state access
    try {
        await executeWithGameLock(game_id, async (currentGame) => {
            const anyBotHasLegalMoves = checkIfAnyBotHasLegalMoves(currentGame);
            
            if (anyBotHasLegalMoves) {
                console.log(`Bot cycle completed, scheduling next bot action cycle for game ${game_id}`);
                
                // Log bot states for debugging
                const bots = currentGame.players.filter(p => p.is_ai);
                const botStates = bots.map(bot => `${bot.name}(${bot.awaiting_attack ? 'awaiting' : 'not awaiting'}, ${bot.done_attacking_this_round ? 'done' : 'continuing'}, ${bot.hand.length} cards)`).join(', ');
                console.log(`Bot states: ${botStates}`);
                
                shouldLoop = true;
            } else {
                shouldLoop = false;
                console.log(`Bot cycle completed, no more bot moves available for game ${game_id}`);
            }
            return { game: currentGame, events: [] };
        });
    } catch (error) {
        console.error('Error checking for additional bot actions:', error);
    }

    if (shouldLoop) {
        // massive chain of promises
        return await processBotActions(game_id, cycle+1); 
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
            shouldAct = botIndex !== game.defender && bot.awaiting_attack && !bot.done_attacking_this_round;
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
        
        console.log(`Bot ${bot.name} completed ${chosenMove.type} action`);
        
    } catch (error) {
        console.error(`Error processing bot action for ${bot.name}:`, error);
    }
}

// Execute a bot's chosen move using shared action handlers
async function executeBotMove(game: Game, bot: PrivatePlayer, move: LegalMove): Promise<void> {
    try {
        let specialMessage: string | undefined;
        
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
                await executeCover(game, bot.player_id, move.cards!, move.attack_cards!, true);
                // Check if this cover completed the round (all attacks covered)
                const all_attacks_covered = game.table_battles.every(battle => battle.defense !== null);
                if (all_attacks_covered) {
                    // Add special message for successful cover that ends the round
                    specialMessage = `Bot ${bot.name} successfully covered and ended the round`;
                }
                break;
                
            case 'pass':
                await executePass(game, bot.player_id, move.cards!);
                // Set done_attacking_this_round flag based on the move's choice
                if (move.done_attacking_this_round !== undefined) {
                    bot.done_attacking_this_round = move.done_attacking_this_round;
                    // If bot is done attacking this round, set awaiting_attack = false
                    if (move.done_attacking_this_round) {
                        bot.awaiting_attack = false;
                        console.log(`Bot ${bot.name} is done attacking this round after pass`);
                        
                        // Only try to execute "good" logic if game is now in WAIT_FOR_ATTACKERS mode
                        // Use handleGood for proper validation
                        if (game.status === GAME_STATUS.WAIT_FOR_ATTACKERS) {
                            try {
                                console.log(`Bot ${bot.name} attempting good logic after pass`);
                                await handleGood(game, bot.player_id);
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
                await executePickup(game, bot.player_id);
                break;
                
            case 'good':
                await handleGood(game, bot.player_id);
                break;
        }
        
        console.log(`Bot ${bot.name} performed ${move.type} action`);
        
        // Add animation event for the move based on move type
        switch (move.type) {
            case 'attack':
                animationEvents.addAttackEvent(bot.player_id, move.cards!);
                break;
            case 'pass':
                animationEvents.addPassEvent(bot.player_id, move.cards!);
                break;
            case 'cover':
                // For cover moves, we need to add events for each card being covered
                for (let i = 0; i < move.cards!.length; i++) {
                    animationEvents.addCoverEvent(bot.player_id, move.cards![i], move.attack_cards![i], i);
                }
                break;
            case 'pickup':
                // Get all cards from table battles for pickup animation
                const allTableCards = game.table_battles.flatMap(battle => 
                    battle.defense ? [battle.attack, battle.defense] : [battle.attack]
                );
                animationEvents.addPickupEvent(bot.player_id, allTableCards);
                break;
            case 'good':
                animationEvents.addMagicTransitionEvent(`Bot ${bot.name} said good - proceeding to next round`);
                break;
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
                shouldAct = botIndex !== game.defender && bot.awaiting_attack && !bot.done_attacking_this_round;
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

