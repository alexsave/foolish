import { corsHeaders } from './cors.ts';
import { Card, Game, LobbyGame, GAME_STATUS, Player, OtherPlayer, PLAYER_STATUS, PersonalGame, SERVER_EVENT_TYPE, PRIVATE_EVENT_TYPE } from './types.ts';
import { ACE_VALUE, CARDS_PER_PLAYER, SUITS, START_VALUE, VALUE_MAP, SUIT_MAP, LCG_A, LCG_C, LCG_M } from './constants.ts';
import { createClient } from 'jsr:@supabase/supabase-js';
import { emailToName } from './common_utils.ts';

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
export const personalize_game = (game: Game, player_id: string | null): PersonalGame => {
    let self: Player | null = null;
    if (player_id !== null) {
        self = game.players.find(player => player.id === player_id)!;
    }
    return {
        id: game.id,
        deck_length: game.deck.length,
        flipped: game.flipped,
        self: self ?? undefined,
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
            // this is because id is handled differently in places
            payload: {...message, game_id: game_id}
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

export const verify_hands_in_players_hand = (player: Player, cards: Card[]) => {
    for (const card of cards) {
        if (!player.hand.some(handCard => card_comp(handCard, card))) {
            throw new Error(`Card ${cardDisplay(card)} is not in player ${player.id}'s hand`);
        }
    }
}

export const no_cards_left = (game: Game) => {
    return game.deck.length === 0 && game.flipped === null;
}

export const check_win = (game: Game) => {
    const the_fool = game_done(game);
    if (the_fool !== null) {

        game.status = GAME_STATUS.WAITING;
        // set all players to idle
        game.players.forEach((player: Player) => {
            player.status = PLAYER_STATUS.IDLE;
            player.hand = [];
        });
        game.table_battles = [];
        game.deck = refill_deck();

        broadcastToGame(game.id, {
            type: 'game_done',
            message: `Game done. Player ${the_fool} ends up the fool`,
            game: personalize_game(game, null)
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
    let defenseHand = game.players[game.currently_attacked].hand;
    if (defenseHand.length === 0) {
        // they draw first
        let cards_drawn = 0;
        while (defenseHand.length < CARDS_PER_PLAYER) {
            const c = draw(game);
            if (c === null) {
                broadcastToGame(game.id, {
                    type: 'deck_ran_out',
                    message: 'Deck ran out',
                    game: personalize_game(game, null)
                });
                break;
            }
            defenseHand.push(c);
            cards_drawn++;
        }
        broadcastToGame(game.id, {
            type: 'player_refilled',
            message: `Player ${game.players[game.currently_attacked].name} refilled their empty hand with ${cards_drawn} cards`,
            cards: defenseHand,
            game: personalize_game(game, null)
        });
    }

    // Then go around starting from firstAttacker
    let pIndex = game.first_attacker;
    do {
        const hand = game.players[pIndex].hand;
        let cards_drawn = 0;

        while (hand.length < CARDS_PER_PLAYER) {
            const c = draw(game);
            if (c === null) {
                broadcastToGame(game.id, {
                    type: 'deck_ran_out',
                    message: 'Deck ran out',
                    game: personalize_game(game, null)
                });
                break;
            }
            hand.push(c);
            cards_drawn++;
        }
        if (cards_drawn > 0) {
            broadcastToGame(game.id, {
                    type: 'player_refilled',
                    message: `Player ${game.players[pIndex].name} drew ${cards_drawn} cards`,
                cards: hand,
                game: personalize_game(game, null)
            });
        } else if (cards_drawn === 0 && game.players[pIndex].hand.length === 0) {
            // no cards were drawn, but if they were still "in", this is where they win
            if (game.players[pIndex].status === PLAYER_STATUS.IN) {
                broadcastToGame(game.id, {
                        type: 'player_wins',
                        message: `Player ${game.players[pIndex].name} got rid of all their cards`,
                        game: personalize_game(game, null)
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
