import { corsHeaders, handleCors } from './cors.ts';
import { get_next_player_index } from './common_utils.ts';
import { Card, Game, GAME_STATUS, PLAYER_STATUS, PersonalGame, SERVER_EVENT_TYPE, PRIVATE_EVENT_TYPE, PrivatePlayer, PublicGame, PublicPlayer, PlayerHand, UserEloRating, BotCard } from './types.ts';
import { ACE_VALUE, CARDS_PER_PLAYER, SUITS, VALUE_MAP, SUIT_MAP } from './constants.ts';
import { createClient, User } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"
import { getAuthenticatedUser } from './auth.ts';
import { scheduleBotActions } from './bot_actions.ts';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// Database-level game locking using PostgreSQL advisory locks
export const acquireGameLock = async (game_id: string): Promise<boolean> => {
    const { data, error } = await supabaseClient.rpc('pg_try_advisory_lock_string', { key: game_id });
    
    if (error) {
        console.error(`Failed to acquire lock for game ${game_id}:`, error);
        return false;
    }
    
    return data as boolean;
};

export const releaseGameLock = async (game_id: string): Promise<void> => {
    const { error } = await supabaseClient.rpc('pg_advisory_unlock_string', { key: game_id });
    
    if (error) {
        console.error(`Failed to release lock for game ${game_id}:`, error);
    }
};

// Sequential operation execution with database-level locking
export const executeWithGameLock = async (game_id: string, operation: () => Promise<any>): Promise<any> => {
    // Try to acquire database lock with retry logic
    const maxRetries = 5;
    let lockAcquired = false;
    
    for (let attempt = 1; attempt <= maxRetries; attempt++) {
        lockAcquired = await acquireGameLock(game_id);
        if (lockAcquired) break;
        
        await new Promise(resolve => setTimeout(resolve, 100 * attempt)); // Exponential backoff
    }
    
    if (!lockAcquired) {
        throw new Error(`Could not acquire lock for game ${game_id} - too many concurrent operations`);
    }
    
    try {
        return operation();
    } finally {
        await releaseGameLock(game_id);
    }
};

export const createId = (): string => crypto.randomUUID().slice(0, 6);

export const wrap400 = (execute: (user: User, user_name: string, body: any) => Promise<any>) => {
    const handler = async (req: Request): Promise<Response> => {
        try {
            // Handle CORS
            const corsResponse = handleCors(req);
            if (corsResponse) return corsResponse;

            // Get authenticated user
            const user: User = await getAuthenticatedUser(req);

            // Get user name from email
            const user_name = user.user_metadata.username;

            // Parse JSON body
            let body = {};
            try {
                body = await req.json();
            } catch (e) {}
            // If JSON parsing fails, keep empty object

            // Extract game_id from body for lock management
            const game_id = (body as any).game_id;
            
            let result: any;
            
            if (game_id) {
                // Execute operation with database lock for this specific game
                result = await executeWithGameLock(game_id, () => execute(user, user_name, body));
            } else {
                // No game_id, execute immediately (for operations that don't involve games)
                result = await execute(user, user_name, body);
            }

            // Schedule bot actions if this was a game operation
            if (game_id) {
                scheduleBotActions(game_id);
            }

            // Create standardized response
            return new Response(JSON.stringify(result), {
                headers: {
                    ...corsHeaders,
                    'Content-Type': 'application/json'
                }
            });
        } catch (e: any) {
            console.error('Error processing request:', {
                name: e.name,
                message: e.message,
                stack: e.stack,
                cause: e.cause
            });

            return new Response(
                JSON.stringify({ error: e.message }),
                { status: 400, headers: { ...corsHeaders, 'Content-Type': 'application/json' } }
            );
        }
    };

    // Serve the handler
    serve(handler);
    
    // Return the handler (though it won't be used after serve() is called)
    return handler;
};

export const verify_player_in_game = (game: Game, player_id: string): void => {
    if (!game.players.find(player => player.player_id === player_id)) {
        throw new Error(`Player ${player_id} not in game ${game.id}`);
    }
}

const other_player = (player: PrivatePlayer): PublicPlayer => {
    return { 
        name: player.name, 
        player_id: player.player_id, 
        status: player.status,
        hand_length: player.hand.length,
        is_ai: player.is_ai,
    };
}

// Come to think of it, if we are broadcasting to the game, we can't be sending "self"
export const personalize_game = (game: Game, player_id: string): PersonalGame => {
    // everything except game_decks , added self
    const self = game.players.find(player => player.player_id === player_id)!;
    const personalGame: PersonalGame = {
        id: game.id,
        name: game.name,
        deck_length: game.deck.length,
        flipped: game.flipped,
        players: game.players.map(player => other_player(player)),
        status: game.status,
        power_suit: game.power_suit,
        first_attacker: game.first_attacker,
        defender: game.defender,
        table_battles: game.table_battles,
        elimination_order: game.elimination_order,

        //self: {
            //...self,
            //player_id: player_id,
            //hand: game.players.find(player => player.player_id === player_id)!.hand,
        //}
        self: self
    }
    return personalGame;
}

// =============================================================================
// DATABASE HELPER FUNCTIONS FOR SEPARATED SCHEMA - Using JOINs
// =============================================================================

// Load complete game state from separated tables using efficient JOINs
export const loadCompleteGame = async (game_id: string): Promise<Game> => {
    //converter of SQL to useful game object
    // Use JOIN to get all data in one query
    const { data, error } = await supabaseClient
        .from('games')
        .select(`
            *,
            game_decks(deck),
            player_hands(player_id, hand, awaiting_attack),
            bot_cards(bot_id, hand, awaiting_attack)
        `)
        .eq('id', game_id)
        .single();

    if (error) {
        console.error('Error loading complete game', error);
        throw new Error(`Game ${game_id} not found`);
    }

    console.log(JSON.stringify(data));

    const players: PrivatePlayer[] = data.players.map((player: any) => {
        console.log(JSON.stringify(data.player_hands) + " data.player_hands");
        console.log(JSON.stringify(data.bot_cards) + " data.bot_cards");
        console.log(JSON.stringify(player) + " player");
        
        let hand, awaiting_attack;
        
        if (player.is_ai) {
            // Look up in bot_cards table
            const botCard = data.bot_cards.find(card => card.bot_id === player.player_id)!;
            hand = botCard.hand;
            awaiting_attack = botCard.awaiting_attack;
        } else {
            // Look up in player_hands table
            const playerHand = data.player_hands.find(hand => hand.player_id === player.player_id)!;
            hand = playerHand.hand;
            awaiting_attack = playerHand.awaiting_attack;
        }
        
        return {
            player_id: player.player_id,
            name: player.name,
            status: player.status,
            is_ai: player.is_ai,
            hand: hand,
            awaiting_attack: awaiting_attack,
            hand_length: hand.length,
        } as PrivatePlayer;
    });

    const game: Game = {
        id: data.id,
        name: data.name,
        deck: data.game_decks.deck,
        // unused but necessary for type
        deck_length: data.game_decks.deck.length,
        flipped: data.flipped,
        players: players,
        status: data.status,
        power_suit: data.power_suit,
        first_attacker: data.first_attacker,
        defender: data.defender,
        table_battles: data.table_battles,
        elimination_order: data.elimination_order,
    }
    
    return game;
    /*

    data looks like:
    {
  "id": "9c4b97",
  "name": "alex603's Game",
  "flipped": null,
  "players": [
    {
      "id": "72c6ac7d-5017-4ff6-86f8-df411d7035dd",
      "name": "alex603",
      "status": "idle"
    }
  ],
  "status": "waiting",
  "power_suit": 0,
  "first_attacker": 0,
  "defender": 0,
  "table_battles": [],
  "created_at": "2025-07-01T06:51:13.74066+00:00",
  "updated_at": "2025-07-01T06:51:13.74066+00:00",
  "game_decks": {
    "deck": []
  },
  "player_hands": [
    {
      "hand": [],
      "player_id": "72c6ac7d-5017-4ff6-86f8-df411d7035dd"
      "awaiting_attack": false,
    }
  ]
}
    */
};

// Save complete game state to separated tables using efficient upserts
// TODO This could easily return public state for later use. we calculate lengths here so its very useful
export const saveCompleteGame = async (game: Game): Promise<any> => {
    // converter of game to SQL
    // Update lengths here too
    // Update public game data (remove deck and hands from players)
    const publicPlayers: PublicPlayer[] = game.players.map(player => ({
        name: player.name,
        player_id: player.player_id,
        status: player.status,
        hand_length: player.hand.length,
        is_ai: player.is_ai
    }));

    const publicGame: PublicGame = {
        id: game.id,
        name: game.name || 'Untitled Game',
        deck_length: game.deck.length,
        flipped: game.flipped,
        players: publicPlayers,
        status: game.status,
        power_suit: game.power_suit,
        first_attacker: game.first_attacker,
        defender: game.defender,
        table_battles: game.table_battles,
        elimination_order: game.elimination_order
    };

    await supabaseClient
        .from('games')
        .update(publicGame)
        .eq('id', game.id);

    // Update deck efficiently
    await supabaseClient
        .from('game_decks')
        .upsert({
            game_id: game.id,
            deck: game.deck
        });

    // Batch update human player hands
    const humanPlayers = game.players.filter(player => !player.is_ai);
    const handUpdates: PlayerHand[] = humanPlayers.map(player => ({
        game_id: game.id,
        player_id: player.player_id,
        hand: player.hand,
        awaiting_attack: player.awaiting_attack,
    }));

    if (handUpdates.length > 0) {
        await supabaseClient
            .from('player_hands')
            .upsert(handUpdates);
    }

    // Batch update bot cards
    const botPlayers = game.players.filter(player => player.is_ai);
    const botCardUpdates: BotCard[] = botPlayers.map(player => ({
        game_id: game.id,
        bot_id: player.player_id,
        hand: player.hand,
        awaiting_attack: player.awaiting_attack,
    }));

    if (botCardUpdates.length > 0) {
        await supabaseClient
            .from('bot_cards')
            .upsert(botCardUpdates);
    }

    // dumb? maybe
    const game_utils = {
        broadcast: async (messageType: string, baseMessage: any) => {
            for (const player of game.players) {
                // Create personalized game state by adding player's self data
                const personalizedGame: PersonalGame = {
                    ...publicGame,
                    self: player
                };

                const channel = supabaseClient.channel(`gu-${game.id}-${player.player_id}`, {
                    config: { private: true }
                });
                await channel.send({
                    type: 'broadcast',
                    event: messageType,
                    payload: {...baseMessage, game: personalizedGame}
                });
    
                await supabaseClient.removeChannel(channel);
            }

        }, 
        sendToUser: async (messageType: string, baseMessage: any, user_id: string) => {
            const channel = supabaseClient.channel(`gu-${game.id}-${user_id}`, {
                config: { private: true }
            });

            const personalizedGame: PersonalGame = {
                ...publicGame,
                self: game.players.find(player => player.player_id === user_id)!
            }
            await channel.send({
                type: 'broadcast',
                event: messageType,
                payload: {...baseMessage, game: personalizedGame}
            });
            await supabaseClient.removeChannel(channel);
        },
        publicGame: publicGame,
    }

    return game_utils;
};

// Get player's hand for a specific game using direct query
export const getPlayerHand = async (game_id: string, player_id: string): Promise<Card[]> => {
    const { data, error } = await supabaseClient
        .from('player_hands')
        .select('hand')
        .eq('game_id', game_id)
        .eq('player_id', player_id)
        .single();

    if (error) {
        console.error('Error loading player hand', error);
        return [];
    }

    return data?.hand || [];
};

// Update player's hand efficiently
export const updatePlayerHand = async (game_id: string, player_id: string, hand: Card[]): Promise<void> => {
    await supabaseClient
        .from('player_hands')
        .upsert({
            game_id: game_id,
            player_id: player_id,
            hand: hand
        });
};

// =============================================================================
// REALTIME BROADCAST UTILITIES
// =============================================================================

// private message to specific user
export const broadcastToGameUser = async (game: Game, messageType: string, baseMessage: any, user_id: string): Promise<void> => {
    const channel = supabaseClient.channel(`gu-${game.id}-${user_id}`, {
        config: { private: true }
    });
    await channel.send({
        type: 'broadcast',
        event: messageType,
        payload: {
            ...baseMessage,
            game: personalize_game(game, user_id)
        }
    });
    await supabaseClient.removeChannel(channel);
}

// Optimized method that sends personalized messages to each player's game-user channel
export const broadcastToGameUsers = async (game: Game, messageType: string, baseMessage: any): Promise<void> => {
    try {
        // Calculate base game state once (shared for all players)
        const baseGameState: PublicGame = {
            id: game.id,
            name: game.name,
            deck_length: game.deck.length,
            flipped: game.flipped,
            players: game.players.map((player: PrivatePlayer) => ({
                name: player.name,
                player_id: player.player_id,
                status: player.status,
                hand_length: player.hand.length
            }) as PublicPlayer),
            status: game.status,
            power_suit: game.power_suit,
            first_attacker: game.first_attacker,
            defender: game.defender,
            table_battles: game.table_battles,
            elimination_order: game.elimination_order,
        };

        console.log(JSON.stringify(baseGameState) + " baseGameState");

        // Send personalized message to each player
        for (const player of game.players) {
            /*const self: PrivatePlayer = {
                ...player,
                hand: player.hand,
                awaiting_attack: player.awaiting_attack,
                status: player.status,
                name: player.name,
                hand_length: player.hand.length
            };*/
            // Create personalized game state by adding player's self data
            const personalizedGame: PersonalGame = {
                ...baseGameState,
                self: player
            };

            // Create personalized message with filtered game state
            const personalizedMessage = {
                ...baseMessage,
                game: personalizedGame
            };
            const channel = supabaseClient.channel(`gu-${game.id}-${player.player_id}`, {
                config: { private: true }
            });
            await channel.send({
                type: 'broadcast',
                event: messageType,
                payload: personalizedMessage
            });

            await supabaseClient.removeChannel(channel);
        }

        // Send to publicly visible game channel (for spectators)
        const channel = supabaseClient.channel(`game-${game.id}`, {
            config: { private: true }
        });
        await channel.send({
            type: 'broadcast',
            event: messageType,
            payload: {...baseMessage, game: baseGameState}
        });
        await supabaseClient.removeChannel(channel);

    } catch (error) {
        console.error('Error broadcasting to game users:', error);
    }
};

export const start_game = async (game: Game) => {
    // This is the game entry
    game.status = 'playing';
    game.players.forEach(player => {
        player.status = PLAYER_STATUS.IN;
    });

    game.deck = refill_deck(game.players.length);
    game.elimination_order = []; // Initialize elimination order tracking

    const hands: Card[][] = initialize_hands(game);
    for (let i = 0; i < game.players.length; i++) {
        game.players[i].hand = hands[i];
    }

    let flipped_card = draw(game);
    while (flipped_card!.value === ACE_VALUE) {
        // move back to deck
        game.deck.push(flipped_card!);
        flipped_card = draw(game);
    }
    game.flipped = flipped_card;
    game.power_suit = game.flipped!.suit;

    // Notify all players about the flipped card
    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.FLIPPED_CARD,
        message: `Flipped card is ${cardDisplay(game.flipped!)}`
    });

    const lowest_power_index = determine_lowest_power_index(game);
    game.first_attacker = lowest_power_index;
    set_positions(game);
    game.status = GAME_STATUS.FIRST_ATTACKER;

    // Save updated game state
    //await saveCompleteGame(game);

    // Send notifications
    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.FIRST_ATTACKER,
        message: `Player ${game.players[lowest_power_index].name} is the first attacker, wait for them to attack`
    });

    for (let i = 0; i < game.players.length; i++) {
        const hand = game.players[i].hand;
        if (i === game.first_attacker) {
            broadcastToGameUser(game, 'private_message', {
                type: PRIVATE_EVENT_TYPE.REQUEST_FIRST_ATTACK,
                message: `Please choose an attack. Options are ${hand.map(card => cardDisplay(card)).join(', ')}`
            }, game.players[i].player_id);
        } else {
            broadcastToGameUser(game, 'private_message', {
                type: PRIVATE_EVENT_TYPE.PLAYER_HAND,
                message: `Player ${game.players[i].name} hand ${hand.map(card => cardDisplay(card)).join(', ')}`
            }, game.players[i].player_id);
        }
    }

    return game;
}

export const refill_deck = (players: number): Card[] => {
    const deck: Card[] = [];
    // Start at 6 vs 2
    const startValue = players > 4 ? 1 : 5;
    for (let i = 0; i < SUITS.length; i++) {
        for (let j = startValue; j <= ACE_VALUE; j++) {
            deck.push({ suit: SUITS[i], value: j });
        }
    }
    return deck;
}

export const initialize_hands = (game: Game): Card[][] => {
    const result: Card[][] = [];
    for (let j = 0; j < game.players.length; j++) {
        result.push([]);
    }
    for (let i = 0; i < CARDS_PER_PLAYER; i++) {
        result.push([]);
        for (let j = 0; j < game.players.length; j++) {
            //const name = result[j].name;
            const c = draw(game)!;
            //console.log(`Player ${name} draws ${cardDisplay(c)}`);
            result[j].push(c);
        }
    }
    return result;
}

export const cardDisplay = (card: Card) => `${VALUE_MAP[card.value]} of ${SUIT_MAP[card.suit]}`;

export const draw = (game: Game): Card | null => {
    if (game.deck.length === 0) {
        if (game.flipped === null) {
            return null;
        }
        const copy: Card = game.flipped;
        game.flipped = null;
        return copy;
    }
    const index = Math.floor(Math.random() * game.deck.length);
    const card = game.deck.splice(index, 1)[0];
    return card;
};

export const determine_lowest_power_index = (game: Game): number => {
    let lowestPowerValue = ACE_VALUE + 1;
    let lowestPowerPlayer = -1;
    for (let i = 0; i < game.players.length; i++) {
        const hand = game.players[i].hand;
        for (let j = 0; j < hand.length; j++) {
            let card = hand[j];
            if (card.suit === game.power_suit) {
                if (card.value < lowestPowerValue) {
                    lowestPowerValue = card.value;
                    lowestPowerPlayer = i;
                }
            }
        }
    }
    if (lowestPowerPlayer === -1) {
        lowestPowerPlayer = Math.floor(Math.random() * game.players.length);
    }
    return lowestPowerPlayer;
}

export const set_positions = (game: Game) => {
    game.first_attacker = game.first_attacker;
    game.defender = (game.first_attacker + 1) % game.players.length;
}

// Helper method to validate player is/isn't defender
export const validate_defender_status = (game: Game, player_id: string, should_be_defender: boolean) => {
    const isDefender = game.players[game.defender].player_id === player_id;
    if (isDefender !== should_be_defender) {
        throw new Error(`Player ${player_id} is ${should_be_defender ? 'not' : ''} the defender`);
    }
}

export const verify_cards_in_players_hand = (player: PrivatePlayer, cards: Card[]) => {
    for (const card of cards) {
        if (!player.hand.some(handCard => card_comp(handCard, card))) {
            throw new Error(`Card ${cardDisplay(card)} is not in player ${player.player_id}'s hand`);
        }
    }
}

export const no_cards_left = (game: Game) => {
    return game.deck.length === 0 && game.flipped === null;
}

export const check_win = async (game: Game) => {
    const the_fool = game_done(game);
    if (the_fool !== null) {

        // Update ELO ratings before resetting game state
        await updateEloRatings(game);

        game.status = GAME_STATUS.WAITING;
        // set all players to idle
        game.players.forEach((player: PrivatePlayer) => {
            player.status = PLAYER_STATUS.IDLE;
            player.hand = [];
        });
        game.table_battles = [];
        game.deck = refill_deck(game.players.length);
        game.elimination_order = []; // Reset elimination order

        // Clear chat messages for the game (fire and forget)
        supabaseClient
            .from('chat_messages')
            .delete()
            .eq('game_id', game.id);

        broadcastToGameUsers(game, 'game_update', {
            type: 'game_done',
            message: `Game done. Player ${the_fool} ends up the fool`
        });
    }
}

export const card_comp = (card1: Card, card2: Card): boolean => {
    return card1.suit === card2.suit && card1.value === card2.value;
}

const game_done = (game: Game): string | null => {
    // only one 1 left, every0one else is out
    const in_players = game.players.filter(player => player.status === PLAYER_STATUS.IN);
    const out_players = game.players.filter(player => player.status === PLAYER_STATUS.OUT);
    if (in_players.length === 1 && out_players.length === game.players.length - 1) {
        return in_players[0].player_id;
    }
    return null;
}

// TODO: find a better way to communicate refill without interfering with other broadcasts
// timestamps???
export const refill = async (game: Game) => {

    if (no_cards_left(game)) {
        return;
    }

    // If the deck was already empty, defending should've gotten them a win
    // most importantly, check if defender cleared their hand
    const defenseHand = game.players[game.defender].hand;
    if (defenseHand.length === 0) {
        // they draw first
        let cards_drawn = 0;
        while (defenseHand.length < CARDS_PER_PLAYER) {
            const c = draw(game);
            if (c === null) {
                /*broadcastToGameUsers(game, 'game_update', {
                    type: 'deck_ran_out',
                    message: 'Deck ran out'
                });*/
                break;
            }
            defenseHand.push(c);
            cards_drawn++;
        }
        /*broadcastToGameUsers(game, 'game_update', {
            type: 'player_refilled',
            message: `Player ${game.players[game.defender].name} refilled their empty hand with ${cards_drawn} cards`,
            cards_drawn: cards_drawn
        });*/
    }

    // Then go around starting from firstAttacker
    let pIndex = game.first_attacker;
    do {
        const hand = game.players[pIndex].hand;
        let cards_drawn = 0;

        while (hand.length < CARDS_PER_PLAYER) {
            const c = draw(game);
            if (c === null) {
                /*broadcastToGameUsers(game, 'game_update', {
                    type: 'deck_ran_out',
                    message: 'Deck ran out'
                });*/
                break;
            }
            hand.push(c);
            cards_drawn++;
        }
        if (cards_drawn > 0) {
            /*broadcastToGameUsers(game, 'game_update', {
                type: 'player_refilled',
                message: `Player ${game.players[pIndex].name} drew ${cards_drawn} cards`,
                cards_drawn: cards_drawn
            });*/
        } else if (cards_drawn === 0 && hand.length === 0) {
            // no cards were drawn, but if they were still "in", this is where they win
            if (game.players[pIndex].status === PLAYER_STATUS.IN) {
                /*broadcastToGameUsers(game, 'game_update', {
                    type: 'player_wins',
                    message: `Player ${game.players[pIndex].name} got rid of all their cards`
                });*/
                game.players[pIndex].status = PLAYER_STATUS.OUT;
                game.elimination_order.push(game.players[pIndex].player_id); // Track elimination order
                await check_win(game);
            }
        }
        pIndex = get_next_player_index(game, pIndex);
        //pIndex = (pIndex + 1) % game.players.length;
    } while (pIndex !== game.first_attacker/* && !no_cards_left(game)*/);
};

// =============================================================================
// ELO RATING SYSTEM
// =============================================================================

// Standard ELO rating calculation
const calculateEloChange = (playerRating: number, opponentRating: number, actualScore: number, kFactor: number = 32): number => {
    const expectedScore = 1 / (1 + Math.pow(10, (opponentRating - playerRating) / 400));
    return Math.round(kFactor * (actualScore - expectedScore));
};

// Get or create ELO rating for a user
export const getOrCreateEloRating = async (userId: string): Promise<UserEloRating> => {
    const { data, error } = await supabaseClient
        .from('user_elo_ratings')
        .select('*')
        .eq('user_id', userId)
        .single();

    if (error && error.code === 'PGRST116') {
        // User doesn't have ELO rating, create one
        const newRating = {
            user_id: userId,
            elo_rating: 1000,
            games_played: 0
        };

        const { data: insertData, error: insertError } = await supabaseClient
            .from('user_elo_ratings')
            .insert(newRating)
            .select()
            .single();

        if (insertError) {
            console.error('Error creating ELO rating:', insertError);
            throw new Error('Failed to create ELO rating');
        }

        return insertData;
    }

    if (error) {
        console.error('Error fetching ELO rating:', error);
        throw new Error('Failed to fetch ELO rating');
    }

    return data;
};

// Get ELO rating for a bot
export const getBotEloRating = async (botId: string): Promise<{elo_rating: number, games_played: number}> => {
    const { data, error } = await supabaseClient
        .from('bots')
        .select('elo_rating, games_played')
        .eq('id', botId)
        .single();

    if (error) {
        console.error('Error fetching bot ELO rating:', error);
        throw new Error('Failed to fetch bot ELO rating');
    }

    return data;
};

// Update ELO ratings for all players after game completion
export const updateEloRatings = async (game: Game): Promise<void> => {
    if (game.players.length < 2) {
        return; // No ELO updates for single player games
    }

    try {
        // Get all player ELO ratings (both human and bot)
        const playerRatings = new Map<string, {elo_rating: number, games_played: number}>();
        const humanPlayers: string[] = [];
        const botPlayers: string[] = [];
        
        for (const player of game.players) {
            if (player.is_ai) {
                const rating = await getBotEloRating(player.player_id);
                playerRatings.set(player.player_id, rating);
                botPlayers.push(player.player_id);
            } else {
                const rating = await getOrCreateEloRating(player.player_id);
                playerRatings.set(player.player_id, rating);
                humanPlayers.push(player.player_id);
            }
        }

        // Determine final rankings based on elimination order
        // elimination_order contains players in order they WON (got rid of cards)
        // The first player in elimination_order is the winner (1st place)
        // The player NOT in elimination_order is the fool (last place)
        const rankings: string[] = [];
        
        // Add winners in order they got rid of cards (elimination_order[0] = 1st place, etc.)
        for (let i = 0; i < game.elimination_order.length; i++) {
            rankings.push(game.elimination_order[i]);
        }
        
        // Add the fool (player not in elimination_order) as last place
        const fool = game.players.find(p => !game.elimination_order.includes(p.player_id));
        if (fool) {
            rankings.push(fool.player_id); // Fool is last place
        }

        // Calculate ELO changes for each player
        const ratingChanges = new Map<string, number>();
        
        for (let i = 0; i < rankings.length; i++) {
            const playerId = rankings[i];
            const playerRating = playerRatings.get(playerId)!;
            let totalChange = 0;

            // For each other player, calculate 1v1 ELO change
            for (let j = 0; j < rankings.length; j++) {
                if (i === j) continue;
                
                const opponentId = rankings[j];
                const opponentRating = playerRatings.get(opponentId)!;
                
                // Determine score: 1 if player finished better, 0 if worse, 0.5 if tie
                let score: number;
                if (i < j) {
                    score = 1; // Player finished better
                } else if (i > j) {
                    score = 0; // Player finished worse
                } else {
                    score = 0.5; // Tie (shouldn't happen in our ranking system)
                }

                const change = calculateEloChange(playerRating.elo_rating, opponentRating.elo_rating, score);
                totalChange += change;
            }

            ratingChanges.set(playerId, totalChange);
        }

        // Update human player ratings
        const humanRatingUpdates: Array<{user_id: string, elo_rating: number, games_played: number}> = [];
        for (const playerId of humanPlayers) {
            const change = ratingChanges.get(playerId) || 0;
            const currentRating = playerRatings.get(playerId)!;
            const newRating = Math.max(0, currentRating.elo_rating + change); // Prevent negative ratings
            
            humanRatingUpdates.push({
                user_id: playerId,
                elo_rating: newRating,
                games_played: currentRating.games_played + 1
            });
        }

        if (humanRatingUpdates.length > 0) {
            await supabaseClient
                .from('user_elo_ratings')
                .upsert(humanRatingUpdates);
        }

        // Update bot ratings
        const botRatingUpdates: Array<{id: string, elo_rating: number, games_played: number}> = [];
        for (const playerId of botPlayers) {
            const change = ratingChanges.get(playerId) || 0;
            const currentRating = playerRatings.get(playerId)!;
            const newRating = Math.max(0, currentRating.elo_rating + change); // Prevent negative ratings
            
            botRatingUpdates.push({
                id: playerId,
                elo_rating: newRating,
                games_played: currentRating.games_played + 1
            });
        }

        if (botRatingUpdates.length > 0) {
            await supabaseClient
                .from('bots')
                .upsert(botRatingUpdates);
        }

        console.log('ELO ratings updated successfully for game:', game.id);
    } catch (error) {
        console.error('Error updating ELO ratings:', error);
        // Don't throw error to prevent breaking game completion
    }
};
