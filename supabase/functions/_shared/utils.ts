import { corsHeaders, handleCors } from './cors.ts';
import {
    cardDisplay,
    refill_deck,
    draw,
    personalize_game,
    calculateEloChange,
    calculateGameRankings,
    determine_lowest_power_index,
    set_positions,
    initialize_hands,
    get_next_player_index,
    no_cards_left,
    game_done,
    other_player,
} from './common_utils.ts';
import { Card, Game, GAME_STATUS, PLAYER_STATUS, PersonalGame, SERVER_EVENT_TYPE, PRIVATE_EVENT_TYPE, PrivatePlayer, PublicGame, PublicPlayer, PlayerHand, UserEloRating, BotHand, AnimationEvent, PublicAnimationEvent, PersonalAnimationEvent, ANIMATION_EVENT_TYPE } from './types.ts';
import { ACE_VALUE, CARDS_PER_PLAYER } from './constants.ts';
import { createClient, User } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"
import { getAuthenticatedUser } from './auth.ts';
import { lockedBotLoop } from './bot_actions.ts';
import { AnimationEventManager } from './animation_event_manager.ts';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// Database-level game locking using table-based locks
export const acquireGameLock = async (game_id: string): Promise<boolean> => {
    try {
        // Generate a random lock ID for this instance
        const lockId = crypto.randomUUID();

        const { error } = await supabaseClient
            .from('game_locks')
            .insert({ game_id, lock_id: lockId });

        if (error) {
            // Handle non-unique constraint errors
            if (error.code !== '23505') {
                return false;
            }

            // Check if existing lock is older than 150 seconds
            const { data: existingLock } = await supabaseClient
                .from('game_locks')
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
                .from('game_locks')
                .delete()
                .eq('game_id', game_id);

            // Try to insert again
            const { error: retryError } = await supabaseClient
                .from('game_locks')
                .insert({ game_id, lock_id: lockId });

            if (retryError) {
                return false;
            }
        }

        // Verify we actually got the lock by checking the lock_id
        const { data, error: selectError } = await supabaseClient
            .from('game_locks')
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

export const releaseGameLock = async (game_id: string): Promise<void> => {
    try {
        const { error } = await supabaseClient
            .from('game_locks')
            .delete()
            .eq('game_id', game_id);

        if (error) {
            console.error(`Failed to release lock for game ${game_id}:`, error);
        }

    } catch (error) {
        console.error(`Error releasing lock for game ${game_id}:`, error);
    }
};

// Sequential operation execution with database-level locking
export const executeWithGameLock = async (game_id: string, operation: (game: Game) => Promise<{ game: Game, events: AnimationEvent[] }>): Promise<{ game: Game, events: AnimationEvent[] }> => {
    // Try to acquire database lock with retry logic
    const maxRetries = 9;
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
        const loadedGame: Game = await loadCompleteGame(game_id);
        const result = await operation(loadedGame);

        // Always save the game state
        await saveCompleteGame(result.game);

        return { game: result.game, events: result.events };
    } finally {
        await releaseGameLock(game_id);
    }
};

// Helper function to convert full Game to PublicGame (removes private data)
const gameToPublicGame = (game: Game): PublicGame => {
    return {
        id: game.id,
        name: game.name,
        deck_length: game.deck.length,
        discard_pile_length: game.discard_pile_length,
        flipped: game.flipped,
        players: game.players.map(other_player),
        status: game.status,
        power_suit: game.power_suit,
        first_attacker: game.first_attacker,
        defender: game.defender,
        table_battles: game.table_battles,
        elimination_order: game.elimination_order,
    };
};

// Helper function to create PersonalGame from Game and player
const gameToPersonalGame = (game: Game, player: PrivatePlayer): PersonalGame => {
    return {
        ...gameToPublicGame(game),
        self: player
    };
};

// Helper function to create card backs for sanitization
const createCardBacks = (count: number): Card[] => {
    return Array(count).fill({ suit: -1, value: -1 });
};

// Helper function to check if event should have cards sanitized
const shouldSanitizeCards = (event: AnimationEvent): boolean => {
    return (event.type === ANIMATION_EVENT_TYPE.REFILL || event.type === ANIMATION_EVENT_TYPE.DEAL) && !!event.cards;
};

// Convert server AnimationEvents to PublicAnimationEvents for spectators
export const convertToPublicAnimationEvents = (events: AnimationEvent[]): PublicAnimationEvent[] => {
    return events.map(event => {
        // Extract game_state separately to avoid type conflicts with spread
        const { game_state, ...baseEvent } = event;
        const publicEvent: PublicAnimationEvent = { ...baseEvent };

        // Sanitize REFILL and DEAL events - hide all cards going to players' hands
        if (shouldSanitizeCards(event)) {
            publicEvent.cards = createCardBacks(event.cards!.length);
        }

        // Convert game_state from full Game to PublicGame
        if (game_state) {
            publicEvent.game_state = gameToPublicGame(game_state);
        }

        return publicEvent;
    });
};

// Convert server AnimationEvents to PersonalAnimationEvents for a specific player
export const convertToPersonalAnimationEvents = (events: AnimationEvent[], forPlayerId: string): PersonalAnimationEvent[] => {
    return events.map(event => {
        // Extract game_state separately to avoid type conflicts with spread
        const { game_state, ...baseEvent } = event;
        const personalEvent: PersonalAnimationEvent = { ...baseEvent };

        // Sanitize REFILL and DEAL events from OTHER players - hide their cards going to hands
        if (shouldSanitizeCards(event) && event.player_id && event.player_id !== forPlayerId) {
            personalEvent.cards = createCardBacks(event.cards!.length);
        }
        // Note: If it's the current player's REFILL/DEAL event, keep their cards visible

        // Convert game_state from full Game to PersonalGame
        if (game_state) {
            const baseGameState = gameToPublicGame(game_state);

            // Create personalized game state by adding player's self data
            const playerSelf = game_state.players.find(p => p.player_id === forPlayerId);
            if (!playerSelf) {
                console.warn(`Player ${forPlayerId} not found in game state, removing game_state from animation event`);
            } else {
                personalEvent.game_state = {
                    ...baseGameState,
                    self: playerSelf
                };
            }
        }

        return personalEvent;
    });
};

// Export a global instance for use across the application
export const animationEvents = new AnimationEventManager();

// Broadcast animation events to all players and spectators
export const broadcastAnimationEvents = async (game: Game, events: AnimationEvent[]): Promise<void> => {
    if (events.length === 0) {
        return;
    }

    console.log(`[BROADCAST DEBUG] broadcastAnimationEvents called for game ${game.id} with ${events.length} events`);

    // Calculate base game state once (shared for all players) - removes private information
    const baseGameState = gameToPublicGame(game);

    // Send personalized events to each player individually
    for (const player of game.players) {
        const personalEvents = convertToPersonalAnimationEvents(events, player.player_id);

        const payload = {
            type: 'animation_sequence',
            events: personalEvents,
            sequence_id: crypto.randomUUID(),
            timestamp: Date.now()
        };

        console.log(`[SECURITY] Sending ${personalEvents.length} personal events to player ${player.name} (${player.player_id})`);

        // Log any cards that were sanitized for this player
        personalEvents.forEach((event, index) => {
            if (event.cards && event.player_id && event.player_id !== player.player_id) {
                const cardCount = event.cards.length;
                const hasSanitizedCards = event.cards.some(card => card.suit === -1 && card.value === -1);
                if (hasSanitizedCards) {
                    console.log(`[SECURITY] Event ${index + 1} (${event.type}) for player ${player.name}: ${cardCount} cards from ${event.player_id} sanitized to card backs`);
                }
            }
        });

        // Create personalized game state by adding player's self data
        const personalizedGame = gameToPersonalGame(game, player);

        const channel = supabaseClient.channel(`gu-${game.id}-${player.player_id}`, {
            config: { private: true }
        });

        await channel.send({
            type: 'broadcast',
            event: 'animation_events',
            payload: {
                ...payload,
                game: personalizedGame
            }
        });

        await supabaseClient.removeChannel(channel);
    }

    // Send public events to spectator channel
    const publicEvents = convertToPublicAnimationEvents(events);

    const spectatorPayload = {
        type: 'animation_sequence',
        events: publicEvents,
        sequence_id: crypto.randomUUID(),
        timestamp: Date.now()
    };

    console.log(`[SECURITY] Sending ${publicEvents.length} public events to spectator channel`);

    const spectatorChannel = supabaseClient.channel(`game-${game.id}`, {
        config: { private: true }
    });

    await spectatorChannel.send({
        type: 'broadcast',
        event: 'animation_events',
        payload: {
            ...spectatorPayload,
            game: baseGameState
        }
    });

    await supabaseClient.removeChannel(spectatorChannel);
};

export interface ExecutionParams {
    user: User;
    user_name: string;
    body: any;
    game: Game;
}

export const wrap400 = (execute: (params: ExecutionParams) => Promise<{ game: Game, events: AnimationEvent[] }>, run_bots: boolean = false) => {
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
            } catch (e) { }
            // If JSON parsing fails, keep empty object

            // Extract game_id from body for lock management
            const game_id = (body as any).game_id;

            let result: any;
            let events: AnimationEvent[] = [];

            if (game_id) {
                // Execute operation with database lock for this specific game
                const { game, events: operationEvents } = await executeWithGameLock(game_id, (game) => execute({ user, user_name, body, game }));
                result = game;
                events = operationEvents;

                // Broadcast animation events if any were collected
                if (events.length > 0) {
                    await broadcastAnimationEvents(result, events);
                }
            } else {
                // No game_id, execute immediately (for operations that don't involve games)
                // pretty much only create
                const operationResult = await execute({ user, user_name, body, game: {} as Game });

                result = operationResult.game;
                events = operationResult.events;

                // Broadcast for game creation
                if (result && result.id && events.length > 0) {
                    await broadcastAnimationEvents(result, events);
                }
            }

            // Schedule bot actions if this was a game operation
            if (game_id/* && run_bots*/) {
                // TODO: not quite. Only after start/attack/cover/pass/pickup/good 
                lockedBotLoop(game_id);
            }

            // handle spectating here too 
            const personalized_result = personalize_game(result, user.id);

            // Create standardized response
            return new Response(JSON.stringify(personalized_result), {
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
            bot_hands(bot_id, hand, awaiting_attack, done_attacking_this_round)
        `)
        .eq('id', game_id)
        .single();

    if (error) {
        console.error('Error loading complete game', error);
        throw new Error(`Game ${game_id} not found`);
    }

    console.log(JSON.stringify(data));

    const players: PrivatePlayer[] = data.players.map((player: any) => {
        let hand, awaiting_attack, done_attacking_this_round;

        if (player.is_ai) {
            // Look up in bot_hands table
            const botHand = data.bot_hands.find(hand => hand.bot_id === player.player_id)!;
            if (botHand) {
                hand = botHand.hand;
                awaiting_attack = botHand.awaiting_attack;
                done_attacking_this_round = botHand.done_attacking_this_round;
            }
        } else {
            // Look up in player_hands table
            const playerHand = data.player_hands.find(hand => hand.player_id === player.player_id)!;
            hand = playerHand.hand;
            awaiting_attack = playerHand.awaiting_attack;
            done_attacking_this_round = false; // Human players don't use this flag, just set it to false for type simplicity
        }

        return {
            player_id: player.player_id,
            name: player.name,
            status: player.status,
            is_ai: player.is_ai,
            hand: hand,
            awaiting_attack: awaiting_attack,
            done_attacking_this_round: done_attacking_this_round,
            hand_length: hand.length,
        } as PrivatePlayer;
    });

    const game: Game = {
        id: data.id,
        name: data.name,
        deck: data.game_decks.deck,
        // unused but necessary for type
        deck_length: data.game_decks.deck.length,
        discard_pile_length: data.discard_pile_length,
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
    const publicGame: PublicGame = gameToPublicGame(game);

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

    // Batch update bot hands
    const botPlayers = game.players.filter(player => player.is_ai);
    const botHandUpdates: BotHand[] = botPlayers.map(player => ({
        game_id: game.id,
        bot_id: player.player_id,
        hand: player.hand,
        awaiting_attack: player.awaiting_attack,
        done_attacking_this_round: player.done_attacking_this_round,
    }));

    if (botHandUpdates.length > 0) {
        await supabaseClient
            .from('bot_hands')
            .upsert(botHandUpdates);
    }

    // dumb? maybe
    const game_utils = {
        broadcast: async (messageType: string, baseMessage: any) => {
            for (const player of game.players) {
                // Create personalized game state by adding player's self data
                const personalizedGame = gameToPersonalGame(game, player);

                const channel = supabaseClient.channel(`gu-${game.id}-${player.player_id}`, {
                    config: { private: true }
                });
                await channel.send({
                    type: 'broadcast',
                    event: messageType,
                    payload: { ...baseMessage, game: personalizedGame }
                });

                await supabaseClient.removeChannel(channel);
            }

        },
        sendToUser: async (messageType: string, baseMessage: any, user_id: string) => {
            const channel = supabaseClient.channel(`gu-${game.id}-${user_id}`, {
                config: { private: true }
            });

            const playerSelf = game.players.find(player => player.player_id === user_id)!;
            const personalizedGame = gameToPersonalGame(game, playerSelf);
            await channel.send({
                type: 'broadcast',
                event: messageType,
                payload: { ...baseMessage, game: personalizedGame }
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

export const start_game = async (game: Game) => {
    // Guard against starting game if it's already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return game;
    }

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

    // Flipped card event will be included in the start game animation sequence
    animationEvents.addFlippedEvent(game.flipped!);

    const lowest_power_index = determine_lowest_power_index(game);
    game.first_attacker = lowest_power_index;
    set_positions(game);

    // First attacker notification will be included in the start game animation sequence
    animationEvents.addMagicTransitionEvent(`Player ${game.players[lowest_power_index].name} is the first attacker, wait for them to attack`);

    // Send private messages to players (these don't go through animation events)
    for (let i = 0; i < game.players.length; i++) {
        const hand = game.players[i].hand;
        if (i === game.first_attacker) {
            await broadcastToGameUser(game, 'private_message', {
                type: PRIVATE_EVENT_TYPE.REQUEST_FIRST_ATTACK,
                message: `Please choose an attack. Options are ${hand.map(card => cardDisplay(card)).join(', ')}`
            }, game.players[i].player_id);
        } else {
            await broadcastToGameUser(game, 'private_message', {
                type: PRIVATE_EVENT_TYPE.PLAYER_HAND,
                message: `Player ${game.players[i].name} hand ${hand.map(card => cardDisplay(card)).join(', ')}`
            }, game.players[i].player_id);
        }
    }

    return game;
}

// Functions moved to common_utils.ts

export const check_win = async (game: Game) => {
    const the_fool = game_done(game);
    if (the_fool !== null) {

        // Update ELO ratings before changing game state
        await updateEloRatings(game);

        // Set game status to GAME_OVER to show win screen
        game.status = GAME_STATUS.GAME_OVER;

        // set all players to idle but keep their hands for display
        game.players.forEach((player: PrivatePlayer) => {
            if (player.is_ai) {
                player.status = PLAYER_STATUS.READY;
            } else {
                player.status = PLAYER_STATUS.IDLE;
            }
        });

        // Keep table_battles, deck, and elimination_order for win screen display
        // These will be cleared when someone hits continue

        // Game done notification will be sent through animation events
        animationEvents.addMagicTransitionEvent(`Game done. Player ${the_fool} ends up the fool`);
    }
}

// Functions moved to common_utils.ts

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
                // Deck ran out - no special notification needed
                break;
            }
            defenseHand.push(c);
            cards_drawn++;
        }
        // Player refill handled through animation events
    }

    // Then go around starting from firstAttacker
    let pIndex = game.first_attacker;
    do {
        const hand = game.players[pIndex].hand;
        let cards_drawn = 0;

        while (hand.length < CARDS_PER_PLAYER) {
            const c = draw(game);
            if (c === null) {
                // Deck ran out - no special notification needed
                break;
            }
            hand.push(c);
            cards_drawn++;
        }
        if (cards_drawn > 0) {
            // Player refill handled through animation events
        } else if (cards_drawn === 0 && hand.length === 0) {
            // no cards were drawn, but if they were still "in", this is where they win
            if (game.players[pIndex].status === PLAYER_STATUS.IN) {
                // Player win handled through animation events in check_win
                game.players[pIndex].status = PLAYER_STATUS.OUT;
                game.players[pIndex].awaiting_attack = false;
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
export const getBotEloRating = async (botId: string): Promise<{ elo_rating: number, games_played: number, nickname: string, strategy_key: string }> => {
    const { data, error } = await supabaseClient
        .from('bots')
        .select('elo_rating, games_played, nickname, strategy_key')
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
        const playerRatings = new Map<string, { elo_rating: number, games_played: number }>();
        const botData = new Map<string, { elo_rating: number, games_played: number, nickname: string, strategy_key: string }>();
        const humanPlayers: string[] = [];
        const botPlayers: string[] = [];

        for (const player of game.players) {
            if (player.is_ai) {
                const botInfo = await getBotEloRating(player.player_id);
                playerRatings.set(player.player_id, botInfo);
                botData.set(player.player_id, botInfo);
                botPlayers.push(player.player_id);
            } else {
                const rating = await getOrCreateEloRating(player.player_id);
                playerRatings.set(player.player_id, rating);
                humanPlayers.push(player.player_id);
            }
        }

        // Determine final rankings based on elimination order
        const rankings = calculateGameRankings(game);

        // Safety check: ensure all players are in rankings
        if (rankings.length !== game.players.length) {
            console.error(`ERROR: Rankings incomplete! Expected ${game.players.length} players, got ${rankings.length} in rankings`);
            console.error('This will cause incorrect ELO calculations. Skipping ELO update.');
            return;
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
        const humanRatingUpdates: Array<{ user_id: string, elo_rating: number, previous_elo: number, games_played: number }> = [];
        for (const playerId of humanPlayers) {
            const change = ratingChanges.get(playerId) || 0;
            const currentRating = playerRatings.get(playerId)!;
            const newRating = Math.max(0, currentRating.elo_rating + change); // Prevent negative ratings

            humanRatingUpdates.push({
                user_id: playerId,
                elo_rating: newRating,
                previous_elo: currentRating.elo_rating, // Store previous ELO
                games_played: currentRating.games_played + 1
            });
        }

        if (humanRatingUpdates.length > 0) {
            await supabaseClient
                .from('user_elo_ratings')
                .upsert(humanRatingUpdates);
        }

        // Update bot ratings
        const botRatingUpdates: Array<{ id: string, nickname: string, strategy_key: string, elo_rating: number, previous_elo: number, games_played: number }> = [];
        for (const playerId of botPlayers) {
            const change = ratingChanges.get(playerId) || 0;
            const currentRating = playerRatings.get(playerId)!;
            const currentBotData = botData.get(playerId)!;
            const newRating = Math.max(0, currentRating.elo_rating + change); // Prevent negative ratings

            botRatingUpdates.push({
                id: playerId,
                nickname: currentBotData.nickname,
                strategy_key: currentBotData.strategy_key,
                elo_rating: newRating,
                previous_elo: currentRating.elo_rating,
                games_played: currentRating.games_played + 1
            });
        }

        if (botRatingUpdates.length > 0) {
            const { data: botUpdateData, error: botUpdateError } = await supabaseClient
                .from('bots')
                .upsert(botRatingUpdates)
                .select();

            if (botUpdateError) {
                console.error('Error updating bot ratings:', botUpdateError);
            } else {
            }
        }

    } catch (error) {
        console.error('Error updating ELO ratings:', error);
        // Don't throw error to prevent breaking game completion
    }
};
