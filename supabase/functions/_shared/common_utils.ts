import { Card, Game, PersonalGame, PLAYER_STATUS, PrivatePlayer, PublicPlayer, GAME_STATUS } from "./types.ts";
import { ACE_VALUE, CARDS_PER_PLAYER, SUITS, VALUE_MAP, SUIT_MAP } from './constants.ts';

export const get_next_player_index = (game: Game | PersonalGame, current_player: number): number => {
    // Check if there's only one player left in the game
    const in_players = game.players.filter(player => player.status === PLAYER_STATUS.IN);
    if (in_players.length <= 1) {
        // If there's only one player left, the game should end
        // Return the current player to avoid infinite loops, but this shouldn't happen
        console.warn('get_next_player_index called with only one player left - game should have ended');
        return current_player;
    }
    
    let next_player = (current_player + 1) % game.players.length;
    while (game.players[next_player].status === PLAYER_STATUS.OUT) {
        next_player = (next_player + 1) % game.players.length;
    }
    return next_player;
}

export const canCover = (attack: Card, defense: Card, powerSuit: number) => {
    if (defense.suit !== attack.suit) {
        // only different suit scenario that works
        return defense.suit === powerSuit && attack.suit !== powerSuit;
    }
    return defense.value > attack.value;
};

// Pure utility functions moved from utils.ts to avoid JSR dependencies in tests
export const cardDisplay = (card: Card) => `${VALUE_MAP[card.value]} of ${SUIT_MAP[card.suit]}`;

export const card_comp = (card1: Card, card2: Card): boolean => {
    return card1.suit === card2.suit && card1.value === card2.value;
};

export const validate_defender_status = (game: Game, player_id: string, should_be_defender: boolean) => {
    const isDefender = game.players[game.defender].player_id === player_id;
    if (isDefender !== should_be_defender) {
        throw new Error(`Player ${player_id} is ${should_be_defender ? 'not ' : ''}the defender`);
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

export const initialize_hands = (game: Game): Card[][] => {
    const result: Card[][] = [];
    for (let j = 0; j < game.players.length; j++) {
        result.push([]);
    }
    for (let i = 0; i < CARDS_PER_PLAYER; i++) {
        for (let j = 0; j < game.players.length; j++) {
            const c = draw(game)!;
            result[j].push(c);
        }
    }
    return result;
}

export const game_done = (game: Game): string | null => {
    // only one 1 left, everyone else is out
    const in_players = game.players.filter(player => player.status === PLAYER_STATUS.IN);
    const out_players = game.players.filter(player => player.status === PLAYER_STATUS.OUT);
    if (in_players.length === 1 && out_players.length === game.players.length - 1) {
        return in_players[0].player_id;
    }
    return null;
}

// Pure functions moved from utils.ts to avoid JSR dependencies in tests
export const createId = (): string => crypto.randomUUID().slice(0, 6);

export const verify_player_in_game = (game: Game, player_id: string): void => {
    if (!game.players.find(player => player.player_id === player_id)) {
        throw new Error(`Player ${player_id} not in game ${game.id}`);
    }
}

export const other_player = (player: PrivatePlayer): PublicPlayer => {
    return { 
        name: player.name, 
        player_id: player.player_id, 
        status: player.status,
        hand_length: player.hand.length,
        is_ai: player.is_ai,
    };
}

export const personalize_game = (game: Game, player_id: string): PersonalGame => {
    // everything except game_decks , added self
    const self = game.players.find(player => player.player_id === player_id)!;
    const personalGame: PersonalGame = {
        id: game.id,
        name: game.name,
        deck_length: game.deck.length,
        discard_pile_length: game.discard_pile_length,
        flipped: game.flipped,
        players: game.players.map(player => other_player(player)),
        status: game.status,
        power_suit: game.power_suit,
        first_attacker: game.first_attacker,
        defender: game.defender,
        table_battles: game.table_battles,
        elimination_order: game.elimination_order,
        self: self
    }
    return personalGame;
}

// Standard ELO rating calculation
export const calculateEloChange = (playerRating: number, opponentRating: number, actualScore: number, kFactor: number = 32): number => {
    const expectedScore = 1 / (1 + Math.pow(10, (opponentRating - playerRating) / 400));
    return Math.round(kFactor * (actualScore - expectedScore));
};

// Calculate final rankings based on elimination order
export const calculateGameRankings = (game: Game): string[] => {
    const rankings: string[] = [];
    
    console.log('calculateGameRankings debug:');
    console.log('- elimination_order:', game.elimination_order);
    console.log('- all players:', game.players.map(p => ({ id: p.player_id, name: p.name, status: p.status })));
    
    // Add winners in order they got rid of cards (elimination_order[0] = 1st place, etc.)
    // Deduplicate elimination_order to handle backend bugs
    const uniqueEliminationOrder = Array.from(new Set(game.elimination_order));
    console.log('- unique elimination_order:', uniqueEliminationOrder);
    
    for (let i = 0; i < uniqueEliminationOrder.length; i++) {
        rankings.push(uniqueEliminationOrder[i]);
    }
    
    // Add the fool (player not in elimination_order) as last place
    const fool = game.players.find(p => !uniqueEliminationOrder.includes(p.player_id));
    console.log('- fool found:', fool ? { id: fool.player_id, name: fool.name, status: fool.status } : null);
    
    if (fool) {
        rankings.push(fool.player_id); // Fool is last place
    }
    
    console.log('- final rankings:', rankings);
    console.log('- expected player count:', game.players.length, 'actual ranking count:', rankings.length);
    
    return rankings;
};

// Pure refill logic without side effects (no broadcasting, no async check_win)
export const refillPlayerHands = (game: Game): void => {
    // If no cards left in deck, still need to mark players with 0 cards as OUT
    if (no_cards_left(game)) {
        // Check all players and mark those with 0 cards as OUT
        for (let i = 0; i < game.players.length; i++) {
            const player = game.players[i];
            if (player.hand.length === 0 && player.status === PLAYER_STATUS.IN) {
                player.status = PLAYER_STATUS.OUT;
                player.awaiting_attack = false;
                game.elimination_order.push(player.player_id);
            }
        }
        return;
    }

    // If the deck was already empty, defending should've gotten them a win
    // most importantly, check if defender cleared their hand
    const defenseHand = game.players[game.defender].hand;
    if (defenseHand.length === 0) {
        // they draw first
        while (defenseHand.length < CARDS_PER_PLAYER) {
            const c = draw(game);
            if (c === null) {
                break;
            }
            defenseHand.push(c);
        }
    }

    // Then go around starting from firstAttacker
    let pIndex = game.first_attacker;
    do {
        const hand = game.players[pIndex].hand;

        while (hand.length < CARDS_PER_PLAYER) {
            const c = draw(game);
            if (c === null) {
                break;
            }
            hand.push(c);
        }
        
        // Check if player has no cards and should be marked as OUT
        if (hand.length === 0 && game.players[pIndex].status === PLAYER_STATUS.IN) {
            game.players[pIndex].status = PLAYER_STATUS.OUT;
            game.players[pIndex].awaiting_attack = false;
            game.elimination_order.push(game.players[pIndex].player_id);
        }
        
        pIndex = get_next_player_index(game, pIndex);
    } while (pIndex !== game.first_attacker);
};

// Pure win check logic without side effects (no ELO updates, no broadcasting)
export const checkWinAndResetGame = (game: Game): string | null => {
    const the_fool = game_done(game);
    if (the_fool !== null) {
        // Guard against overwriting GAME_OVER status - only continue/ should do this
        if (game.status === GAME_STATUS.GAME_OVER) {
            return the_fool;
        }
        
        // Reset game state to waiting
        game.status = GAME_STATUS.WAITING;
        // set all players to idle
        game.players.forEach((player: PrivatePlayer) => {
            player.status = PLAYER_STATUS.IDLE;
            player.hand = [];
        });
        game.table_battles = [];
        game.deck = refill_deck(game.players.length);
        game.elimination_order = []; // Reset elimination order
        game.discard_pile_length = 0; // Reset discard pile length
        
        return the_fool;
    }
    return null;
};


