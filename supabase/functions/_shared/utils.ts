import { corsHeaders } from './cors.ts';
import { Card, Game, LobbyGame, GAME_STATUS, Player, OtherPlayer, PLAYER_STATUS, PersonalGame, SERVER_EVENT_TYPE, PRIVATE_EVENT_TYPE } from './types.ts';
import { ACE_VALUE, CARDS_PER_PLAYER, SUITS, START_VALUE, VALUE_MAP, SUIT_MAP, LCG_A, LCG_C, LCG_M } from './constants.ts';
import { createClient } from 'jsr:@supabase/supabase-js';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

export const createId = (): string => crypto.randomUUID().slice(0, 6);

// clear everything but player name and status. save some bytes
export const lobbify_game = (game: Game): LobbyGame => {
    return {
        id: game.id,
        players: game.players.map(player => ({ name: player.name, status: player.status, id: player.id })),
        status: game.status === GAME_STATUS.WAITING ? GAME_STATUS.WAITING : GAME_STATUS.PLAYING
    };
};

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

export const emailToName = (email: string): string => {
  return email.split('@')[0];
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

export const personalize_game = (game: Game, player_id: string): PersonalGame => {
    return {
        id: game.id,
        deck_length: game.deck.length,
        flipped: game.flipped,
        self: game.players.find(player => player.id === player_id)!,
        players: game.players.map(other_player),
        status: game.status,
        first_attacker: game.first_attacker,
        currently_attacked: game.currently_attacked,
        previous_first_attacker: game.previous_first_attacker,
        previous_currently_attacked: game.previous_currently_attacked,
        table_battles: game.table_battles,
        power_suit: game.power_suit
    }
}

// =============================================================================
// REALTIME BROADCAST UTILITIES
// =============================================================================

export const broadcastToGame = async (game_id: string, message: any): Promise<void> => {
    try {
        const channel = supabaseClient.channel(`game-${game_id}`, {
            config: { private: true }
        });
        
        await channel.send({
            type: 'broadcast',
            event: 'game_message',
            payload: message
        });
        
        // Clean up channel
        await supabaseClient.removeChannel(channel);
    } catch (error) {
        console.error('Error broadcasting to game:', error);
        // Don't throw error to avoid breaking the main flow
    }
};

export const broadcastToUser = async (user_id: string, message: any): Promise<void> => {
    try {
        // Get user email and extract prefix
        const { data: user, error: userError } = await supabaseClient
            .from('auth.users')
            .select('email')
            .eq('id', user_id)
            .single();
        
        if (userError || !user?.email) {
            console.error('Error getting user email for broadcast:', userError);
            return;
        }
        
        const name = emailToName(user.email);
        const channel = supabaseClient.channel(`user-${name}`, {
            config: { private: true }
        });
        
        await channel.send({
            type: 'broadcast',
            event: 'private_message',
            payload: message
        });
        
        // Clean up channel
        await supabaseClient.removeChannel(channel);
    } catch (error) {
        console.error('Error broadcasting to user:', error);
        // Don't throw error to avoid breaking the main flow
    }
};

export const start_game = (game: Game) => {

    // This is the game entry
    game.status = 'playing';
    game.players.forEach(player => {
        player.status = PLAYER_STATUS.IN;
    });

    game.deck = refill_deck();

    const hands = initialize_hands(game);
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

    // Everyone needs to know
    broadcastToGame(game.id, {
        game_id: game.id,
        message: {
            type: SERVER_EVENT_TYPE.FLIPPED_CARD,
            message: `Flipped card is ${cardDisplay(game.flipped!)}`,
            game: game
        }
    });
    const lowest_power_index = determine_lowest_power_index(game);


    game.first_attacker = lowest_power_index;
    set_positions(game);


    // request attack from first attacker

    game.status = GAME_STATUS.FIRST_ATTACKER;



    broadcastToGame(game.id, {
        game_id: game.id,
        message: {
            type: SERVER_EVENT_TYPE.FIRST_ATTACKER,
            game: game,
            message: `Player ${game.players[lowest_power_index].name} is the first attacker, wait for them to attack`
        }
    });

    for (let i = 0; i < game.players.length; i++) {
        if (i === game.first_attacker) {
            broadcastToUser(game.players[i].id, {
                type: PRIVATE_EVENT_TYPE.REQUEST_FIRST_ATTACK,
                message: `Please choose an attack. Options are ${game.players[game.first_attacker].hand.map(card => cardDisplay(card)).join(', ')}`,
                game: game
            });
        } else {
            broadcastToUser(game.players[i].id, {
                type: PRIVATE_EVENT_TYPE.PLAYER_HAND,
                message: `Player ${game.players[i].name} hand ${game.players[i].hand.map(card => cardDisplay(card)).join(', ')}`,
                game: game
            });
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
    if (game.deck.length === 0) {
        if (game.flipped === null) {
            return null;
        }
        const copy: Card = game.flipped;
        game.flipped = null;
        return copy;
    }
    // Make this more secure
    const index = Math.floor(seededRand() * game.deck.length);
    const card = game.deck.splice(index, 1)[0];
    return card;
};

export const determine_lowest_power_index = (game: Game): number => {
    let lowestPowerValue = ACE_VALUE + 1;
    let lowestPowerPlayer = -1;
    for (let i = 0; i < game.players.length; i++) {
        let hand = game.players[i].hand;
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
    game.currently_attacked = (game.first_attacker + 1) % game.players.length;
    game.previous_first_attacker = game.first_attacker;
    game.previous_currently_attacked = game.currently_attacked;
}

// Seeded random generator
let currentSeed = 4 + 20;

export const seededRand = () => {
    // Math.random()
    currentSeed = (LCG_A * currentSeed + LCG_C) % LCG_M;
    return currentSeed / LCG_M; // Normalize to a value between 0 (inclusive) and 1 (exclusive)
};