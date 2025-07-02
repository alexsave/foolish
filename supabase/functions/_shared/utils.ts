import { corsHeaders } from './cors.ts';
import { Card, Game, LobbyGame, GAME_STATUS, Player, OtherPlayer, PLAYER_STATUS, PersonalGame, SERVER_EVENT_TYPE, PRIVATE_EVENT_TYPE, PrivatePlayer, PublicGame, PublicPlayer } from './types.ts';
import { ACE_VALUE, CARDS_PER_PLAYER, SUITS, START_VALUE, VALUE_MAP, SUIT_MAP, LCG_A, LCG_C, LCG_M } from './constants.ts';
import { createClient } from 'jsr:@supabase/supabase-js';
import { emailToName } from './common_utils.ts';
import "jsr:@supabase/functions-js/edge-runtime.d.ts"

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

export const createId = (): string => crypto.randomUUID().slice(0, 6);

export const wrap400 = (execute: (req: Request) => Promise<Response>) => async (req: Request): Promise<Response> => {
    try {
        return execute(req);
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
}

export const verify_game_id = async (game_id: string): Promise<void> => {
    const { data: game, error: gameError } = await supabaseClient.from('games').select('*').eq('id', game_id).single();
    if (gameError) {
        console.error('Error loading game', gameError);
        throw new Error(`Game ${game_id} not found`);
    }
}

export const verify_player_in_game = async (game_id: string, player_id: string): Promise<void> => {
    const { data: player_game, error: player_gameError } = await supabaseClient.from('player_games').select('*').eq('game_id', game_id).eq('player_id', player_id).single();
    if (player_gameError) {
        console.error('Error loading player game', player_gameError);
        throw new Error(`Player ${player_id} not in game ${game_id}`);
    }
}

const other_player = (player: Player): OtherPlayer => {
    return { 
        name: player.name, 
        id: player.id, 
        hand_length: player.hand.length, 
        status: player.status === PLAYER_STATUS.AWAITING_ATTACK ? PLAYER_STATUS.IN : player.status 
    };
}

// Come to think of it, if we are broadcasting to the game, we can't be sending "self"
export const personalize_game = (game: Game, player_id: string): PersonalGame => {
    const self: PrivatePlayer = game.player_hands.find(hand => hand.player_id === player_id)!;
    // for some reason we can't remove game_decks from the game type
    // TODO: why?
    // everything except game_decks , added self
    const personalGame: PersonalGame = {
        id: game.id,
        name: game.name,
        deck_length: game.deck_length,
        flipped: game.flipped,
        players: game.players,
        status: game.status,
        power_suit: game.power_suit,
        first_attacker: game.first_attacker,
        currently_attacked: game.currently_attacked,
        table_battles: game.table_battles,

        self: self,
    }
    return personalGame;
}

// =============================================================================
// NEW DATABASE HELPER FUNCTIONS FOR SEPARATED SCHEMA - Using JOINs
// =============================================================================

// Load complete game state from separated tables using efficient JOINs
export const loadCompleteGame = async (game_id: string): Promise<Game> => {
    // Use JOIN to get all data in one query
    const { data, error } = await supabaseClient
        .from('games')
        .select(`
            *,
            game_decks(deck),
            player_hands(player_id, hand)
        `)
        .eq('id', game_id)
        .single();

    if (error) {
        console.error('Error loading complete game', error);
        throw new Error(`Game ${game_id} not found`);
    }
    // it is already complete. Game type def is wrong
    return data;
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
  "currently_attacked": 0,
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
    }
  ]
}
    */

    // Reconstruct complete game object
    /*const completeGame: Game = {
        ...data,
        deck: data.game_decks?.[0]?.deck || [],
        players: data.players.map((player: any) => {
            const handData = data.player_hands?.find((h: any) => h.player_id === player.id);
            return {
                ...player,
                hand: handData?.hand || []
            };
        })
    };

    return completeGame;*/
};

// Save complete game state to separated tables using efficient upserts
// TODO This could easily return public state for later use. we calculate lengths here so its very useful
export const saveCompleteGame = async (game: Game): Promise<void> => {
    // Update lengths here too
    // Update public game data (remove deck and hands from players)
    const publicPlayers = game.players.map(player => ({
        name: player.name,
        id: player.id,
        status: player.status,
        // TODO: find a better way
        hand_length: game.player_hands.find(hand => hand.player_id === player.id)!.hand.length
    }));
    console.log(' deck length', game.game_decks.deck.length);

    await supabaseClient
        .from('games')
        .update({
            name: game.name || 'Untitled Game',
            deck_length: game.game_decks.deck.length,
            flipped: game.flipped,
            players: publicPlayers,
            status: game.status,
            power_suit: game.power_suit,
            first_attacker: game.first_attacker,
            currently_attacked: game.currently_attacked,
            table_battles: game.table_battles
        })
        .eq('id', game.id);

    // Update deck efficiently
    await supabaseClient
        .from('game_decks')
        .upsert({
            game_id: game.id,
            deck: game.game_decks.deck
        });
    console.log(JSON.stringify(game.player_hands, null, 2));

    // Batch update all player hands
    const handUpdates: PrivatePlayer[] = game.player_hands.map(hand => {
        return {
            game_id: game.id,
            player_id: hand.player_id,
            hand: hand.hand
        }
    });
    console.log(JSON.stringify(handUpdates, null, 2));

    if (handUpdates.length > 0) {
        const { error: handError } = await supabaseClient
            .from('player_hands')
            .upsert(handUpdates);
        
        if (handError) {
            console.error('Error upserting player hands:', handError);
            throw new Error(`Failed to save player hands: ${handError.message}`);
        }
    }
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
            deck_length: game.deck_length,
            flipped: game.flipped,
            players: game.players,
            status: game.status,
            power_suit: game.power_suit,
            first_attacker: game.first_attacker,
            currently_attacked: game.currently_attacked,
            table_battles: game.table_battles,
        };

        // Send personalized message to each player
        for (const player of game.players) {
            const self: PrivatePlayer = game.player_hands.find(hand => hand.player_id === player.id)!;
            // Create personalized game state by adding player's self data
            const personalizedGame: PersonalGame = {
                ...baseGameState,
                self: self
            };

            // Create personalized message with filtered game state
            const personalizedMessage = {
                ...baseMessage,
                game: personalizedGame
            };
            const channel = supabaseClient.channel(`gu-${game.id}-${player.name}`, {
                config: { private: true }
            });
            await channel.send({
                type: 'broadcast',
                event: messageType,
                payload: personalizedMessage
            });

            await supabaseClient.removeChannel(channel);
        }
    } catch (error) {
        console.error('Error broadcasting to game users:', error);
    }
};

// Legacy functions kept for backward compatibility - will be deprecated
export const broadcastToGame = async (game_id: string, message: any): Promise<void> => {
    console.warn('broadcastToGame is deprecated. Use broadcastToGameUsers instead.');
    // For now, we need the game object to use the new method
    // This will be removed once all usages are updated
};

export const broadcastToUser = async (user_id: string, message: any): Promise<void> => {
    console.warn('broadcastToUser is deprecated. Use broadcastToGameUsers instead.');
    // For now, we need the game object to use the new method  
    // This will be removed once all usages are updated
};

export const start_game = async (game: Game) => {
    // This is the game entry
    game.status = 'playing';
    game.players.forEach(player => {
        player.status = PLAYER_STATUS.IN;
    });

    game.game_decks.deck = refill_deck();

    const hands: Card[][] = initialize_hands(game);
    for (let i = 0; i < game.players.length; i++) {
        const id = game.players[i].id;  
        const hand = game.player_hands.find(hand => hand.player_id === id)!;
        hand.hand = hands[i];
    }

    let flipped_card = draw(game);
    while (flipped_card!.value === ACE_VALUE) {
        // move back to deck
        game.game_decks.deck.push(flipped_card!);
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
        const id = game.players[i].id;
        const hand = game.player_hands.find(hand => hand.player_id === id)!;
        if (i === game.first_attacker) {
            broadcastToGameUser(game, 'private_message', {
                type: PRIVATE_EVENT_TYPE.REQUEST_FIRST_ATTACK,
                message: `Please choose an attack. Options are ${hand.hand.map(card => cardDisplay(card)).join(', ')}`
            }, game.players[i].id);
        } else {
            broadcastToGameUser(game, 'private_message', {
                type: PRIVATE_EVENT_TYPE.PLAYER_HAND,
                message: `Player ${game.players[i].name} hand ${hand.hand.map(card => cardDisplay(card)).join(', ')}`
            }, game.players[i].id);
        }
    }

    return game;
}

export const refill_deck = (): Card[] => {
    const deck: Card[] = [];
    for (let i = 0; i < SUITS.length; i++) {
        for (let j = START_VALUE; j <= ACE_VALUE; j++) {
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
    if (game.game_decks.deck.length === 0) {
        if (game.flipped === null) {
            return null;
        }
        const copy: Card = game.flipped;
        game.flipped = null;
        return copy;
    }
    // Make this more secure
    const index = Math.floor(seededRand() * game.game_decks.deck.length);
    const card = game.game_decks.deck.splice(index, 1)[0];
    return card;
};

export const determine_lowest_power_index = (game: Game): number => {
    let lowestPowerValue = ACE_VALUE + 1;
    let lowestPowerPlayer = -1;
    for (let i = 0; i < game.players.length; i++) {
        const id = game.players[i].id;
        const hand = game.player_hands.find(hand => hand.player_id === id)!;
        for (let j = 0; j < hand.hand.length; j++) {
            let card = hand.hand[j];
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
    game.currently_attacked = (game.first_attacker + 1) % game.players.length;
}

// Seeded random generator
let currentSeed = Math.floor(Math.random() * 1000000);

export const seededRand = () => {
    // Math.random()
    currentSeed = (LCG_A * currentSeed + LCG_C) % LCG_M;
    return currentSeed / LCG_M; // Normalize to a value between 0 (inclusive) and 1 (exclusive)
};

// Helper method to validate player is/isn't defender
export const validate_defender_status = (game: Game, player_id: string, should_be_defender: boolean) => {
    const isDefender = game.players[game.currently_attacked].id === player_id;
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
    return game.game_decks.deck.length === 0 && game.flipped === null;
}

export const check_win = (game: Game) => {
    const the_fool = game_done(game);
    if (the_fool !== null) {

        game.status = GAME_STATUS.WAITING;
        // set all players to idle
        game.players.forEach((player: PublicPlayer) => {
            player.status = PLAYER_STATUS.IDLE;
        });
        game.player_hands.forEach((hand: PrivatePlayer) => {
            hand.hand = [];
        });
        game.table_battles = [];
        game.game_decks.deck = refill_deck();

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
        return in_players[0].id;
    }
    return null;
}


export const canCover = (attack: Card, defense: Card, powerSuit: number) => {
    if (defense.suit !== attack.suit) {
        // only different suit scenario that works
        return defense.suit === powerSuit && attack.suit !== powerSuit;
    }
    return defense.value > attack.value;
};

export const refill = (game: Game) => {

    if (no_cards_left(game)) {
        return;
    }

    // If the deck was already empty, defending should've gotten them a win
    // most importantly, check if currently Attacked cleared their hand
    const defenseId = game.players[game.currently_attacked].id;
    const defenseHand = game.player_hands.find(hand => hand.player_id === defenseId)!.hand;
    if (defenseHand.length === 0) {
        // they draw first
        let cards_drawn = 0;
        while (defenseHand.length < CARDS_PER_PLAYER) {
            const c = draw(game);
            if (c === null) {
                broadcastToGameUsers(game, 'game_update', {
                    type: 'deck_ran_out',
                    message: 'Deck ran out'
                });
                break;
            }
            defenseHand.push(c);
            cards_drawn++;
        }
        broadcastToGameUsers(game, 'game_update', {
            type: 'player_refilled',
            message: `Player ${game.players[game.currently_attacked].name} refilled their empty hand with ${cards_drawn} cards`,
            cards_drawn: cards_drawn
        });
    }

    // Then go around starting from firstAttacker
    let pIndex = game.first_attacker;
    do {
        const id = game.players[pIndex].id;
        const hand = game.player_hands.find(hand => hand.player_id === id)!.hand;
        let cards_drawn = 0;

        while (hand.length < CARDS_PER_PLAYER) {
            const c = draw(game);
            if (c === null) {
                broadcastToGameUsers(game, 'game_update', {
                    type: 'deck_ran_out',
                    message: 'Deck ran out'
                });
                break;
            }
            hand.push(c);
            cards_drawn++;
        }
        if (cards_drawn > 0) {
            broadcastToGameUsers(game, 'game_update', {
                type: 'player_refilled',
                message: `Player ${game.players[pIndex].name} drew ${cards_drawn} cards`,
                cards_drawn: cards_drawn
            });
        } else if (cards_drawn === 0 && hand.length === 0) {
            // no cards were drawn, but if they were still "in", this is where they win
            if (game.players[pIndex].status === PLAYER_STATUS.IN) {
                broadcastToGameUsers(game, 'game_update', {
                    type: 'player_wins',
                    message: `Player ${game.players[pIndex].name} got rid of all their cards`
                });
                game.players[pIndex].status = PLAYER_STATUS.OUT;
                check_win(game);
            }
        }
        pIndex = get_next_player_index(game, pIndex);
        //pIndex = (pIndex + 1) % game.players.length;
    } while (pIndex !== game.first_attacker/* && !no_cards_left(game)*/);
};

export const get_next_player_index = (game: Game, current_player: number): number => {
    let next_player = (current_player + 1) % game.players.length;
    while (game.players[next_player].status === PLAYER_STATUS.OUT) {
        next_player = (next_player + 1) % game.players.length;
    }
    return next_player;
}
