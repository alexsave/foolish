// Utility functions for the game
import express from 'express';
import { 
    Game, Card, Player, PersonalGame, OtherPlayer, PlayerStatus, PLAYER_STATUS, 
    GAME_STATUS, Battle 
} from '../types';
import { 
    CARDS_PER_PLAYER, SUITS, START_VALUE, ACE_VALUE, VALUE_MAP, SUIT_MAP,
    LCG_A, LCG_C, LCG_M
} from '../constants';
import { database } from '../database';

export const wrap400 = (execute: (req: express.Request, res: express.Response) => void) => (req: express.Request, res: express.Response) => {
    try {
        execute(req, res);
    } catch (e: any) {
        res.status(400).end(JSON.stringify({ error: e.message }));
    }
}

export const createId = () => {
    return crypto.randomUUID().slice(0, 6);
}

export const cardDisplay = (card: Card) => `${VALUE_MAP[card.value]} of ${SUIT_MAP[card.suit]}`;

// Seeded random generator
let currentSeed = 4 + 20;

export const seededRand = () => {
    // Math.random()
    currentSeed = (LCG_A * currentSeed + LCG_C) % LCG_M;
    return currentSeed / LCG_M; // Normalize to a value between 0 (inclusive) and 1 (exclusive)
};

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
        deck_length: game.deck.length,
        flipped: game.flipped,
        self: game.players.find(player => player.id === player_id)!,
        players: game.players.map(other_player),
        status: game.status,
        firstAttacker: game.firstAttacker,
        currentlyAttacked: game.currentlyAttacked,
        previousFirstAttacker: game.previousFirstAttacker,
        previousCurrentlyAttacked: game.previousCurrentlyAttacked,
        table: game.table,
        powerSuit: game.powerSuit
    }
}

// Helper method to validate player is/isn't defender
export const validate_defender_status = (game: Game, player_id: string, should_be_defender: boolean) => {
    const isDefender = game.players[game.currentlyAttacked].id === player_id;
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

export const get_next_player_index = (game: Game, current_player: number): number => {
    let next_player = (current_player + 1) % game.players.length;
    while (game.players[next_player].status === PLAYER_STATUS.OUT) {
        next_player = (next_player + 1) % game.players.length;
    }
    return next_player;
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

export const determine_lowest_power_index = (game: Game): number => {
    // Whoever has lowest power
    // With 2 players it's possible no one has it. Also with 4.
    // With 5 it's guaranteed
    // This is actually kind interesting
    // In the 36 card case (6+), you have 9 power cards
    // only 8 max can be distributed to players at dealing because of the flipped card
    // there are still non power 27 cards that can be distributed
    // at most, with 4 players, 4*6=24 cards are all non-power
    // but odds are 27/35 * 26/34 * 25/33 ...
    // but with 5, there must be 3 power cards in the hand
    // with 52, there are 13 power cards and 39 nonpower
    // max of 6 it's possible, 7 it's impossible cuz 42 cards are out
    // Because
    let lowestPowerValue = ACE_VALUE + 1;
    let lowestPowerPlayer = -1;
    for (let i = 0; i < game.players.length; i++) {
        let hand = game.players[i].hand;
        for (let j = 0; j < hand.length; j++) {
            let card = hand[j];
            if (card.suit === game.powerSuit) {
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
    game.firstAttacker = game.firstAttacker;
    game.currentlyAttacked = (game.firstAttacker + 1) % game.players.length;
    game.previousFirstAttacker = game.firstAttacker;
    game.previousCurrentlyAttacked = game.currentlyAttacked;
}

export const check_win = (game_id: string) => {
    const { games, public_game_channel } = database;
    const game = games[game_id];
    const the_fool = game_done(game);
    if (the_fool !== null) {

        public_game_channel.push({
            game_id: game_id,
            message: {
                type: 'game_done',
                message: `Game done. Player ${the_fool} ends up the fool`
            }
        });
        game.status = GAME_STATUS.WAITING;
        // set all players to idle
        game.players.forEach((player: Player) => {
            player.status = PLAYER_STATUS.IDLE;
            player.hand = [];
        });
        game.table = [];
        game.deck = refill_deck();
    }
}

export const refill = (game_id: string) => {
    const { games, public_game_channel } = database;
    const game = games[game_id];

    if (no_cards_left(game)) {
        return;
    }

    // If the deck was already empty, defending should've gotten them a win
    // most importantly, check if currently Attacked cleared their hand
    let defenseHand = game.players[game.currentlyAttacked].hand;
    if (defenseHand.length === 0) {
        // they draw first
        let cards_drawn = 0;
        while (defenseHand.length < CARDS_PER_PLAYER) {
            const c = draw(game);
            if (c === null) {
                public_game_channel.push({
                    game_id: game_id,
                    message: {
                        type: 'deck_ran_out',
                        message: 'Deck ran out',
                        game: game
                    }
                });
                break;
            }
            defenseHand.push(c);
            cards_drawn++;
        }
        public_game_channel.push({
            game_id: game_id,
            message: {
                type: 'player_refilled',
                message: `Player ${game.players[game.currentlyAttacked].name} refilled their empty hand with ${cards_drawn} cards`,
                cards: defenseHand,
                game: game
            }
        });
    }

    // Then go around starting from firstAttacker
    let pIndex = game.firstAttacker;
    do {
        const hand = game.players[pIndex].hand;
        let cards_drawn = 0;

        while (hand.length < CARDS_PER_PLAYER) {
            const c = draw(game);
            if (c === null) {
                public_game_channel.push({
                    game_id: game_id,
                    message: {
                        type: 'deck_ran_out',
                        message: 'Deck ran out',
                        game: game
                    }
                });
                break;
            }
            hand.push(c);
            cards_drawn++;
        }
        if (cards_drawn > 0) {
            public_game_channel.push({
                game_id: game_id,
                message: {
                    type: 'player_refilled',
                    message: `Player ${game.players[pIndex].name} drew ${cards_drawn} cards`,
                    cards: hand,
                    game: game
                }
            });
        } else if (cards_drawn === 0 && game.players[pIndex].hand.length === 0) {
            // no cards were drawn, but if they were still "in", this is where they win
            if (game.players[pIndex].status === PLAYER_STATUS.IN) {
                public_game_channel.push({
                    game_id: game_id,
                    message: {
                        type: 'player_wins',
                        message: `Player ${game.players[pIndex].name} got rid of all their cards`,
                        game: game
                    }
                });
                game.players[pIndex].status = PLAYER_STATUS.OUT;
                check_win(game_id);
            }
        }
        pIndex = get_next_player_index(game, pIndex);
        //pIndex = (pIndex + 1) % game.players.length;
    } while (pIndex !== game.firstAttacker/* && !no_cards_left(game)*/);
};

export const start_game = (game_id: string) => {
    const { games, public_game_channel, private_user_channel } = database;

    // Assume that this is safe to call because we only call from server
    const game = games[game_id];

    // This is the game entry
    game.status = 'playing';
    game.players.forEach(player => {
        player.status = PLAYER_STATUS.IN;
    });

    game.deck = refill_deck();

    const hands = initialize_hands(game);
    for (let i = 0; i < game.players.length; i++) {
        game.players[i].hand = hands[i];

        private_user_channel.push({
            user_id: game.players[i].id,
            message: {
                type: 'player_hand',
                message: `Player ${game.players[i].name} hand ${game.players[i].hand.map(card => cardDisplay(card)).join(', ')}`,
                //hand: game.players[i].hand
            }
        });
    }

    let flipped_card = draw(game);
    while (flipped_card!.value === ACE_VALUE) {
        // move back to deck
        game.deck.push(flipped_card!);
        flipped_card = draw(game);
    }
    game.flipped = flipped_card;
    game.powerSuit = game.flipped!.suit;

    // Everyone needs to know
    public_game_channel.push({
        game_id: game_id,
        message: {
            type: 'flipped_card',
            message: `Flipped card is ${cardDisplay(game.flipped!)}`,

        }
    });

    const lowest_power_index = determine_lowest_power_index(game);

    public_game_channel.push({
        game_id: game_id,
        message: {
            type: 'first_attacker',
            message: `Player ${game.players[lowest_power_index].name} is the first attacker`
        }
    });

    game.firstAttacker = lowest_power_index;
    set_positions(game);


    // request attack from first attacker

    game.status = GAME_STATUS.FIRST_ATTACKER;
    public_game_channel.push({
        game_id: game_id,
        message: {
            type: 'game_status',
            message: `Wait for first attacker to attack`
        }
    });
    private_user_channel.push({
        user_id: game.players[game.firstAttacker].id,
        message: {
            type: 'request_first_attack',
            message: `Please choose an attack. Options are ${game.players[game.firstAttacker].hand.map(card => cardDisplay(card)).join(', ')}`,
        }
    });

} 