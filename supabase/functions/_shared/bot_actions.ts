import { Game, PrivatePlayer, Bot, GAME_STATUS, Card, SERVER_EVENT_TYPE, PLAYER_STATUS } from './types.ts';
import { loadCompleteGame, saveCompleteGame, broadcastToGameUsers, acquireGameLock, releaseGameLock, verify_player_in_game, cardDisplay, validate_defender_status, verify_cards_in_players_hand, no_cards_left, check_win, card_comp, refill, broadcastToGameUser } from './utils.ts';
import { calculateLegalMoves, getBotStrategy, LegalMove } from './bot_strategy.ts';
import { canCover, get_next_player_index } from './common_utils.ts';
import { createClient } from 'jsr:@supabase/supabase-js';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// Process bot responses for a game after a user action
export async function processBotActions(game_id: string): Promise<void> {
    try {
        // Small delay to ensure user action is fully processed
        await new Promise(resolve => setTimeout(resolve, 1000));
        
        // Try to acquire game lock - if we can't, another operation is in progress
        const lockAcquired = await acquireGameLock(game_id);
        if (!lockAcquired) {
            console.log(`Could not acquire lock for bot actions in game ${game_id}`);
            return;
        }
        
        try {
            // Load current game state
            const game = await loadCompleteGame(game_id);
            
            // Process bot actions for this game
            await processBotActionsForGame(game);
            
        } finally {
            await releaseGameLock(game_id);
        }
        
    } catch (error) {
        console.error('Error processing bot actions:', error);
    }
}

// Process bot actions for a specific game
async function processBotActionsForGame(game: Game): Promise<void> {
    // Only process bot actions if game is in a state where bots can act
    if (game.status === GAME_STATUS.WAITING) {
        return; // No bot actions needed in waiting state
    }
    
    // Find all bots in the game
    const bots = game.players.filter(player => player.is_ai);
    
    if (bots.length === 0) {
        return; // No bots to process
    }
    
    // Process bots in order starting from first attacker to keep it fair
    const playerCount = game.players.length;
    const processedBots = new Set<string>();
    
    for (let i = 0; i < playerCount; i++) {
        const playerIndex = (game.first_attacker + i) % playerCount;
        const player = game.players[playerIndex];
        
        // Skip if not a bot or already processed
        if (!player.is_ai || processedBots.has(player.player_id)) {
            continue;
        }
        
        processedBots.add(player.player_id);
        
        const shouldAct = await shouldBotAct(game, player);
        
        if (shouldAct) {
            await processBotAction(game, player);
            
            // Small delay between bot actions to make it feel more natural
            await new Promise(resolve => setTimeout(resolve, 2000));
        }
    }
}

// Determine if a bot should act given current game state
async function shouldBotAct(game: Game, bot: PrivatePlayer): Promise<boolean> {
    const botIndex = game.players.indexOf(bot);
    
    switch (game.status) {
        case GAME_STATUS.FIRST_ATTACKER:
            // Bot should act if it's the first attacker
            return botIndex === game.first_attacker;
            
        case GAME_STATUS.FREE_PLAY:
            // Bot should act if it's the defender or an attacker
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
        
        const strategy = getBotStrategy(botData.strategy_key);
        
        // Calculate legal moves for this bot
        const legalMoves = calculateLegalMoves(game, bot.player_id);
        
        if (legalMoves.length === 0) {
            console.log(`No legal moves for bot ${bot.name}`);
            return;
        }
        
        // Let the strategy choose a move
        const chosenMove = strategy.chooseMove(game, bot.player_id, legalMoves);
        
        // Execute the chosen move
        await executeBotMove(game, bot, chosenMove);
        
        // Save the updated game state after the bot action
        await saveCompleteGame(game);
        
    } catch (error) {
        console.error(`Error processing bot action for ${bot.name}:`, error);
    }
}

// Execute a bot's chosen move by calling handler functions directly
async function executeBotMove(game: Game, bot: PrivatePlayer, move: LegalMove): Promise<void> {
    try {
        switch (move.type) {
            case 'attack':
                await handle_attack(game, game.id, bot.player_id, move.cards!);
                break;
                
            case 'cover':
                await handle_cover(game, game.id, bot.player_id, move.cards!, move.attack_cards!);
                break;
                
            case 'pass':
                await handle_pass(game, game.id, bot.player_id, move.cards!);
                break;
                
            case 'pickup':
                handle_pickup(game, game.id, bot.player_id);
                break;
                
            case 'good':
                handle_good(game, game.id, bot.player_id);
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

// =============================================================================
// HANDLER FUNCTIONS - Direct implementations of game actions for bots
// =============================================================================

// Handle attack action
async function handle_attack(game: Game, game_id: string, player_id: string, cards: Card[]): Promise<void> {
    if (!cards) {
        throw new Error(`No cards provided`);
    }

    // check no duplicates
    if (new Set(cards).size !== cards.length) {
        throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // Find which player this is
    const player: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;

    // also the attacker cannot be the defender
    validate_defender_status(game, player_id, false);

    // check if every card is in hand
    verify_cards_in_players_hand(player, cards);

    // make sure there are enough cards in the defenders hand
    let uncovered_cards = game.table_battles.filter(battle => battle.defense === null).length;
    const defender: PrivatePlayer = game.players[game.defender];
    let defender_cards = defender.hand.length;

    if (uncovered_cards + cards.length > defender_cards) {
        throw new Error(`Player ${player_id} does not have enough cards in their hand to cover ${cards.map(card => cardDisplay(card)).join(', ')}`);
    }

    if (game.status === GAME_STATUS.FIRST_ATTACKER) {
        // check if cards all have same value
        if (!cards.every(card => card.value === cards[0].value)) {
            throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} are not all the same value`);
        }

        // check if player is first attacker
        if (game.players[game.first_attacker].player_id !== player_id) {
            throw new Error(`Player ${player_id} is not the first attacker`);
        }

        // remove from hand, put on table
        player.hand = player.hand.filter(card =>
            !cards.some(mCard => mCard.suit === card.suit && mCard.value === card.value));

        for (const card of cards) {
            game.table_battles.push({
                attack: card,
                defense: null
            });
        }

        if (no_cards_left(game) && player.hand.length === 0) {
            player.status = PLAYER_STATUS.OUT;
            game.elimination_order.push(player.player_id);
            await check_win(game);
        }

        game.status = GAME_STATUS.FREE_PLAY;

    } else if (game.status === GAME_STATUS.FREE_PLAY || game.status === GAME_STATUS.WAIT_FOR_ATTACKERS) {
        // every value has to be on the table
        if (!cards.every(card => game.table_battles.some(battle => battle.attack.value === card.value || battle.defense?.value === card.value))) {
            throw new Error(`Some card values of ${cards.map(card => cardDisplay(card)).join(', ')} are not on the table`);
        }

        // a valid attack will move us out of wait_for_attackers
        game.players.forEach(player => {
            if (player.awaiting_attack) {
                player.status = PLAYER_STATUS.IN;
            }
        });
        game.status = GAME_STATUS.FREE_PLAY;

        player.hand = player.hand.filter(card =>
            !cards.some(mCard => mCard.suit === card.suit && mCard.value === card.value));
        for (const card of cards) {
            game.table_battles.push({
                attack: card,
                defense: null
            });
        }

        if (no_cards_left(game) && player.hand.length === 0) {
            player.status = PLAYER_STATUS.OUT;
            game.elimination_order.push(player.player_id);
            await check_win(game);
        }

        uncovered_cards = game.table_battles.filter(battle => battle.defense === null).length;
        defender_cards = defender.hand.length;

        if (uncovered_cards === defender_cards) {
            game.status = GAME_STATUS.ONLY_DEFEND;
        } else if (uncovered_cards > defender_cards) {
            throw new Error('SEVERE: Uncovered cards > defender_cards');
        }
    } else {
        throw new Error(`Player ${player_id} tried to attack but game is not in valid state`);
    }
}

// Handle cover action
async function handle_cover(game: Game, game_id: string, player_id: string, cover_cards: Card[], attack_cards: Card[]): Promise<void> {
    if (game.status !== GAME_STATUS.FREE_PLAY && game.status !== GAME_STATUS.ONLY_DEFEND) {
        throw new Error(`Game ${game_id} is not in free_play or only_defend mode`);
    }

    // check if player is the defender
    validate_defender_status(game, player_id, true);

    const defender: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;

    // verify cards in hand
    verify_cards_in_players_hand(defender, cover_cards);

    // check no duplicates
    if (new Set(cover_cards).size !== cover_cards.length) {
        throw new Error(`Cards ${cover_cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // ensure that each of the attack cards are on the table AND uncovered
    for (const card of attack_cards) {
        if (!game.table_battles.some(battle => battle.attack.value === card.value && battle.defense === null)) {
            throw new Error(`Card ${cardDisplay(card)} is not on the table`);
        }
    }

    // check no duplicates
    if (new Set(attack_cards).size !== attack_cards.length) {
        throw new Error(`Cards ${attack_cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // can they cover?
    for (let i = 0; i < cover_cards.length; i++) {
        const cover_card = cover_cards[i];
        const attack_card = attack_cards[i];
        if (!canCover(attack_card, cover_card, game.power_suit)) {
            throw new Error(`Card ${cardDisplay(cover_card)} cannot cover ${cardDisplay(attack_card)}`);
        }
    }

    // assert same size of arrays
    if (cover_cards.length !== attack_cards.length) {
        throw new Error(`Cover cards ${cover_cards.map(card => cardDisplay(card)).join(', ')} and attack cards ${attack_cards.map(card => cardDisplay(card)).join(', ')} have different sizes`);
    }

    // now cover the cards
    for (let i = 0; i < cover_cards.length; i++) {
        const cover_card = cover_cards[i];
        const attack_card = attack_cards[i];
        const attack_card_index = game.table_battles.findIndex(battle => card_comp(battle.attack, attack_card) && battle.defense === null);
        if (attack_card_index === -1) {
            throw new Error('SEVERE: Card not found on table');
        }
        game.table_battles[attack_card_index].defense = cover_card;
    }

    // remove the cards from the hand
    defender.hand = defender.hand.filter(card => !cover_cards.some(cover_card => card_comp(card, cover_card)));

    // If defender has no cards left, they may win
    if (defender.hand.length === 0) {
        game.table_battles = [];
        refill(game);
        game.first_attacker = game.defender;
        if (defender.hand.length === 0) {
            game.players[game.first_attacker].status = PLAYER_STATUS.OUT;
            game.elimination_order.push(game.players[game.first_attacker].player_id);
            await check_win(game);
            game.first_attacker = get_next_player_index(game, game.first_attacker);
        }
        game.defender = get_next_player_index(game, game.first_attacker);
        return;
    }

    // Check if all attacks are covered
    const all_attacks_covered = game.table_battles.every(battle => battle.defense !== null);
    if (all_attacks_covered) {
        game.status = GAME_STATUS.WAIT_FOR_ATTACKERS;

        // Check who can still play cards
        const playable_values = new Set<number>();
        for (const battle of game.table_battles) {
            playable_values.add(battle.attack.value)
            if (battle.defense !== null) {
                playable_values.add(battle.defense.value);
            }
        }

        const playable_players = game.players.filter(player => player.player_id !== player_id && player.hand.some(card => playable_values.has(card.value))).map(player => player.player_id);

        if (playable_players.length === 0) {
            // No one can play, end the round
            setTimeout(async () => {
                game.table_battles = [];
                refill(game);
                game.first_attacker = game.defender;
                game.defender = get_next_player_index(game, game.first_attacker);
                game.status = GAME_STATUS.FIRST_ATTACKER;
                await saveCompleteGame(game);
            }, 1000 + Math.random() * 5000);
        } else {
            // Someone can play cards
            game.players.forEach(player => {
                if (playable_players.includes(player.player_id)) {
                    player.awaiting_attack = true;
                }
            });
        }
    }
}

// Handle pass action
async function handle_pass(game: Game, game_id: string, player_id: string, cards: Card[]): Promise<void> {
    if (!cards) {
        throw new Error(`No cards provided`);
    }

    // check if cards all have same value
    if (!cards.every(card => card.value === cards[0].value)) {
        throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} are not all the same value`);
    }

    // check no duplicates
    if (new Set(cards).size !== cards.length) {
        throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // check if player is the defender
    validate_defender_status(game, player_id, true);

    const defender: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;
    verify_cards_in_players_hand(defender, cards);

    // check if there are cards on the table
    if (game.table_battles.length === 0) {
        throw new Error(`No cards on the table`);
    }

    // check passability
    if (game.table_battles.some(battle => battle.defense !== null)) {
        throw new Error(`Cover present, cannot pass`);
    }

    if (!game.table_battles.every(battle => battle.attack.value === cards[0].value)) {
        throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} do not match the values on the table`);
    }

    const next_player_index = get_next_player_index(game, game.defender);
    const next_player = game.players[next_player_index];
    if (next_player.hand.length < cards.length + game.table_battles.length) {
        throw new Error(`Player ${next_player.name} does not have enough cards in their hand to cover ${cards.map(card => cardDisplay(card)).join(', ')}`);
    }

    // Add to table and remove from hand
    for (const card of cards) {
        game.table_battles.push({
            attack: card,
            defense: null
        });
    }
    defender.hand = defender.hand.filter(card => !cards.some(mCard => card_comp(card, mCard)));

    // If the deck is empty, they can get out here
    if (no_cards_left(game) && defender.hand.length === 0) {
        defender.status = PLAYER_STATUS.OUT;
        game.elimination_order.push(defender.player_id);
        await check_win(game);
        game.defender = next_player_index;
    } else {
        game.defender = next_player_index;
    }

    const uncovered_cards = game.table_battles.filter(battle => battle.defense === null).length;
    const new_defender: PrivatePlayer = game.players[game.defender];
    const defender_cards = new_defender.hand.length;

    // Check game status
    if (uncovered_cards === defender_cards) {
        game.status = GAME_STATUS.ONLY_DEFEND;
    } else if (uncovered_cards > defender_cards) {
        throw new Error('Uncovered cards > defender_cards');
    } else if (uncovered_cards < defender_cards) {
        game.status = GAME_STATUS.FREE_PLAY;
    }
}

// Handle pickup action
function handle_pickup(game: Game, game_id: string, player_id: string): void {
    if (game.status !== GAME_STATUS.FREE_PLAY && game.status !== GAME_STATUS.ONLY_DEFEND) {
        throw new Error(`Game ${game_id} is not in free_play or only_defend mode`);
    }

    // check if player is the defender
    validate_defender_status(game, player_id, true);

    // check if there are cards on the table
    if (game.table_battles.length === 0) {
        throw new Error(`No cards on the table`);
    }

    const defender: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;

    // add cards from table to hand
    game.table_battles.forEach(battle => {
        defender.hand.push(battle.attack);
        if (battle.defense) {
            defender.hand.push(battle.defense);
        }
    });

    // clear table
    game.table_battles = [];

    // Draw cards and shift positions
    refill(game);
    game.first_attacker = get_next_player_index(game, game.defender);
    game.defender = get_next_player_index(game, game.first_attacker);
    game.status = GAME_STATUS.FIRST_ATTACKER;
}

// Handle good action
function handle_good(game: Game, game_id: string, player_id: string): void {
    if (game.status !== GAME_STATUS.WAIT_FOR_ATTACKERS) {
        throw new Error(`Game ${game_id} is not in wait_for_attackers mode`);
    }

    const player = game.players.find(player => player.player_id === player_id)!;
    if (player.status !== PLAYER_STATUS.IN) {
        throw new Error(`Player ${player_id} is not ready to attack`);
    }

    // set them to done attacking
    player.awaiting_attack = false;

    // check if all players are done attacking
    const playable_players = game.players.filter(player => 
        player.player_id !== game.players[game.defender].player_id && 
        player.hand.some(card => game.table_battles.some(battle => battle.attack.value === card.value || (battle.defense && battle.defense.value === card.value))) &&
        player.awaiting_attack);

    if (playable_players.length !== 0) {
        return;
    }

    // we are done attacking, shift positions
    game.table_battles = [];
    refill(game);
    game.first_attacker = game.defender;
    game.defender = get_next_player_index(game, game.first_attacker);
    game.status = GAME_STATUS.FIRST_ATTACKER;
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