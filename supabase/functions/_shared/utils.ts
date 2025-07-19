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
} from './common_utils.ts';
import { Card, Game, GAME_STATUS, PLAYER_STATUS, PersonalGame, SERVER_EVENT_TYPE, PRIVATE_EVENT_TYPE, PrivatePlayer, PublicGame, PublicPlayer, PlayerHand, UserEloRating, BotHand, AnimationEvent, ANIMATION_EVENT_TYPE } from './types.ts';
import { ACE_VALUE, CARDS_PER_PLAYER } from './constants.ts';
import { createClient, User } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"
import { getAuthenticatedUser } from './auth.ts';
import { lockedBotLoop } from './bot_actions.ts';

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
export const executeWithGameLock = async (game_id: string, operation: (game: Game) => Promise<{game: Game, events: AnimationEvent[]}>): Promise<{game: Game, events: AnimationEvent[]}> => {
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

// Animation event manager for collecting events during operations
class AnimationEventManager {
    private events: AnimationEvent[] = [];
    
    clear() {
        this.events = [];
    }
    
    getEvents(): AnimationEvent[] {
        return [...this.events];
    }
    
    addEvent(event: AnimationEvent) {
        this.events.push(event);
    }
    
    addAttackEvent(player_id: string, cards: Card[]) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.ATTACK_PASS,
            player_id,
            cards,
            from_location: 'hand',
            to_location: 'table'
        });
    }
    
    addPassEvent(player_id: string, cards: Card[]) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.ATTACK_PASS,
            player_id,
            cards,
            from_location: 'hand',
            to_location: 'table'
        });
    }
    
    addCoverEvent(player_id: string, cover_card: Card, attack_card: Card, battle_index: number) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.COVER,
            player_id,
            cards: [cover_card],
            target_card: attack_card,
            battle_index,
            from_location: 'hand',
            to_location: 'table'
        });
    }
    
    addPickupEvent(player_id: string, cards: Card[]) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.PICKUP,
            player_id,
            cards,
            from_location: 'table',
            to_location: 'hand'
        });
    }
    
    addMagicTransitionEvent(message: string) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
            message
        });
    }
    
    addDealEvent(player_id: string, cards: Card[]) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.DEAL,
            player_id,
            cards,
            from_location: 'deck',
            to_location: 'hand'
        });
    }
    
    addFlippedEvent(card: Card) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.FLIPPED,
            cards: [card],
            from_location: 'deck',
            to_location: 'flipped'
        });
    }
    
    addDefenderMoveEvent(player_id: string) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.DEFENDER_MOVE,
            player_id
        });
    }
    
    addOutEvent(player_id: string) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.OUT,
            player_id
        });
    }
    
    addRefillEvent(player_id: string, cards: Card[]) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.REFILL,
            player_id,
            cards,
            from_location: 'deck',
            to_location: 'hand'
        });
    }
    
    addCardsToTrashEvent(cards: Card[]) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.CARDS_TO_TRASH,
            cards,
            from_location: 'table',
            to_location: 'discard'
        });
    }
}

// Export a global instance for use across the application
export const animationEvents = new AnimationEventManager();

// Broadcast animation events to all players
export const broadcastAnimationEvents = async (game: Game, events: AnimationEvent[]): Promise<void> => {
    console.log(`[ANIMATION DEBUG] broadcastAnimationEvents called with ${events.length} events for game ${game.id}`);
    
    if (events.length === 0) {
        console.log(`[ANIMATION DEBUG] No events to broadcast, returning early`);
        return;
    }
    
    const payload = {
        type: 'animation_sequence',
        events: events,
        sequence_id: crypto.randomUUID(),
        timestamp: Date.now()
    };
    
    console.log(`[ANIMATION DEBUG] Broadcasting animation_events to game users with payload:`, JSON.stringify(payload, null, 2));
    
    await broadcastToGameUsers(game, 'animation_events', payload);
    
    console.log(`[ANIMATION DEBUG] broadcastToGameUsers completed`);
};

export const wrap400 = (execute: (user: User, user_name: string, body: any, game: Game) => Promise<{game: Game, events: AnimationEvent[]}>, run_bots: boolean = false) => {
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
            let events: AnimationEvent[] = [];
            
            if (game_id) {
                // Execute operation with database lock for this specific game
                const { game, events: operationEvents } = await executeWithGameLock(game_id, (game) => execute(user, user_name, body, game));
                result = game;
                events = operationEvents;
                
                console.log(`[ANIMATION DEBUG] Game ${game_id}: Received ${events.length} events from action`);
                if (events.length > 0) {
                    console.log(`[ANIMATION DEBUG] Events:`, JSON.stringify(events, null, 2));
                }
                
                // Broadcast animation events if any were collected
                if (events.length > 0) {
                    console.log(`[ANIMATION DEBUG] Broadcasting ${events.length} events to game users`);
                    await broadcastAnimationEvents(result, events);
                    console.log(`[ANIMATION DEBUG] Broadcast complete`);
                }
            } else {
                // No game_id, execute immediately (for operations that don't involve games)
                // pretty much only create
                const operationResult = await execute(user, user_name, body, {} as Game);
                
                result = operationResult.game;
                events = operationResult.events;
                
                console.log(`[ANIMATION DEBUG] No game_id: Received ${events.length} events from action`);
                
                // Broadcast for game creation
                if (result && result.id && events.length > 0) {
                    console.log(`[ANIMATION DEBUG] Broadcasting ${events.length} events for game creation`);
                    await broadcastAnimationEvents(result, events);
                    console.log(`[ANIMATION DEBUG] Creation broadcast complete`);
                }
            }

            // Schedule bot actions if this was a game operation
            if (game_id && run_bots) {
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
        discard_pile_length: game.discard_pile_length,
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

// Broadcast bot cycle completion - consolidates all bot actions into a single broadcast
export const broadcastBotCycleComplete = async (game: Game, botActions: Array<{botName: string, actionType: string, message: string}>): Promise<void> => {
    if (botActions.length === 0) {
        return;
    }
    
    // Create a summary message of all bot actions
    const actionSummary = botActions.map(action => `${action.botName}: ${action.actionType}`).join(', ');
    const message = botActions.length === 1 
        ? botActions[0].message 
        : `Multiple bot actions: ${actionSummary}`;
    
    await broadcastToGameUsers(game, 'game_update', {
        type: 'bot_cycle_complete',
        message: message,
        bot_actions: botActions,
        action_count: botActions.length
    });
}

// Optimized method that sends personalized messages to each player's game-user channel
export const broadcastToGameUsers = async (game: Game, messageType: string, baseMessage: any): Promise<void> => {
    console.log(`[BROADCAST DEBUG] broadcastToGameUsers called for game ${game.id} with messageType: ${messageType}`);
    console.log(`[BROADCAST DEBUG] baseMessage:`, JSON.stringify(baseMessage, null, 2));
    
    try {
        // Calculate base game state once (shared for all players)
        const baseGameState: PublicGame = {
            id: game.id,
            name: game.name,
            deck_length: game.deck.length,
            discard_pile_length: game.discard_pile_length,
            flipped: game.flipped,
            players: game.players.map((player: PrivatePlayer) => ({
                name: player.name,
                player_id: player.player_id,
                status: player.status,
                hand_length: player.hand.length,
                is_ai: player.is_ai
            }) as PublicPlayer),
            status: game.status,
            power_suit: game.power_suit,
            first_attacker: game.first_attacker,
            defender: game.defender,
            table_battles: game.table_battles,
            elimination_order: game.elimination_order,
        };

        console.log(JSON.stringify(baseGameState) + " baseGameState");

        console.log(`[BROADCAST DEBUG] Sending to ${game.players.length} players`);

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
            const channelName = `gu-${game.id}-${player.player_id}`;
            console.log(`[BROADCAST DEBUG] Creating channel: ${channelName} for player ${player.name}`);
            
            const channel = supabaseClient.channel(channelName, {
                config: { private: true }
            });
            
            console.log(`[BROADCAST DEBUG] Sending to channel ${channelName}`);
            await channel.send({
                type: 'broadcast',
                event: messageType,
                payload: personalizedMessage
            });

            await supabaseClient.removeChannel(channel);
            console.log(`[BROADCAST DEBUG] Sent and removed channel ${channelName}`);
        }

        // Send to publicly visible game channel (for spectators)
        const publicChannelName = `game-${game.id}`;
        console.log(`[BROADCAST DEBUG] Creating public channel: ${publicChannelName}`);
        
        const channel = supabaseClient.channel(publicChannelName, {
            config: { private: true }
        });
        
        console.log(`[BROADCAST DEBUG] Sending to public channel ${publicChannelName}`);
        await channel.send({
            type: 'broadcast',
            event: messageType,
            payload: {...baseMessage, game: baseGameState}
        });
        await supabaseClient.removeChannel(channel);
        console.log(`[BROADCAST DEBUG] Sent and removed public channel ${publicChannelName}`);

    } catch (error) {
        console.error('Error broadcasting to game users:', error);
    }
};

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
            player.status = PLAYER_STATUS.IDLE;
        });
        
        // Keep table_battles, deck, and elimination_order for win screen display
        // These will be cleared when someone hits continue

        broadcastToGameUsers(game, 'game_update', {
            type: 'game_done',
            message: `Game done. Player ${the_fool} ends up the fool`
        });
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
export const getBotEloRating = async (botId: string): Promise<{elo_rating: number, games_played: number, nickname: string, strategy_key: string}> => {
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
        const playerRatings = new Map<string, {elo_rating: number, games_played: number}>();
        const botData = new Map<string, {elo_rating: number, games_played: number, nickname: string, strategy_key: string}>();
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
        console.log('=== ELO UPDATE DEBUG ===');
        console.log('Game ID:', game.id);
        console.log('Players in game:', game.players.map(p => ({ id: p.player_id, name: p.name, is_ai: p.is_ai, status: p.status })));
        console.log('Elimination order from game:', game.elimination_order);
        console.log('Rankings calculated:', rankings);
        console.log('Game rankings:', rankings.map((id, index) => `${index + 1}. ${game.players.find(p => p.player_id === id)?.name} (${id})`));
        console.log('========================');

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
            console.log(`Player ${game.players.find(p => p.player_id === playerId)?.name} (${playerId}) - Total ELO change: ${totalChange > 0 ? '+' : ''}${totalChange}`);
        }

        // Update human player ratings
        const humanRatingUpdates: Array<{user_id: string, elo_rating: number, previous_elo: number, games_played: number}> = [];
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
        const botRatingUpdates: Array<{id: string, nickname: string, strategy_key: string, elo_rating: number, previous_elo: number, games_played: number}> = [];
        for (const playerId of botPlayers) {
            const change = ratingChanges.get(playerId) || 0;
            const currentRating = playerRatings.get(playerId)!;
            const currentBotData = botData.get(playerId)!;
            const newRating = Math.max(0, currentRating.elo_rating + change); // Prevent negative ratings
            
            console.log(`Bot ${playerId} ELO change: ${currentRating.elo_rating} → ${newRating} (${change > 0 ? '+' : ''}${change})`);
            
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
            console.log('Updating bot ratings:', JSON.stringify(botRatingUpdates, null, 2));
            
            const { data: botUpdateData, error: botUpdateError } = await supabaseClient
                .from('bots')
                .upsert(botRatingUpdates)
                .select();
            
            if (botUpdateError) {
                console.error('Error updating bot ratings:', botUpdateError);
            } else {
                console.log('Bot ratings updated successfully:', botUpdateData);
            }
        }

        console.log('ELO ratings updated successfully for game:', game.id);
    } catch (error) {
        console.error('Error updating ELO ratings:', error);
        // Don't throw error to prevent breaking game completion
    }
};
