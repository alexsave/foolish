import { Card, Game, LobbyGame, GAME_STATUS, Player, OtherPlayer, PLAYER_STATUS, PersonalGame, SERVER_EVENT_TYPE, PRIVATE_EVENT_TYPE } from './types';
import { ACE_VALUE, CARDS_PER_PLAYER, SUITS, START_VALUE, VALUE_MAP, SUIT_MAP, LCG_A, LCG_C, LCG_M } from './constants';


export const createId = (): string => crypto.randomUUID().slice(0, 6);

// clear everything but player name and status. save some bytes
export const lobbify_game = (game: Game): LobbyGame => {
    return {
        id: game.id,
        players: game.players.map(player => ({ name: player.name, status: player.status, id: player.id })),
        status: game.status === GAME_STATUS.WAITING ? GAME_STATUS.WAITING : GAME_STATUS.PLAYING,
    };
};

export const emailToName = (email: string): string => {
  return email.split('@')[0];
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