import { Card, Game, PersonalGame, PLAYER_STATUS, PrivatePlayer, PublicPlayer, GAME_STATUS, PublicGame, LOG_TYPE, AnimationEvent, ANIMATION_EVENT_TYPE, Battle, LogCardPair } from "./types.ts";
import { GameLog, UnsavedGameLog } from './types.ts';
import { ACE_VALUE, CARDS_PER_PLAYER, SUITS, VALUE_MAP, SUIT_MAP } from './constants.ts';

// Fast deep clone for Game objects - avoids expensive JSON.parse(JSON.stringify())
export const cloneCard = (card: Card): Card => ({ suit: card.suit, value: card.value });

export const cloneBattle = (battle: Battle): Battle => ({
    attack: cloneCard(battle.attack),
    defense: battle.defense ? cloneCard(battle.defense) : null
});

export const cloneCardPair = (pair: LogCardPair): LogCardPair => ({
    primary: cloneCard(pair.primary),
    target: pair.target ? cloneCard(pair.target) : pair.target
});

export const cloneGameLog = (log: GameLog): GameLog => ({
    id: log.id,
    created_at: log.created_at,
    game_id: log.game_id,
    log_type: log.log_type,
    player_id: log.player_id,
    card_pairs: log.card_pairs.map(cloneCardPair),
    defender_index: log.defender_index
});

export const clonePlayer = (player: PrivatePlayer): PrivatePlayer => ({
    player_id: player.player_id,
    status: player.status,
    name: player.name,
    hand_length: player.hand_length,
    is_ai: player.is_ai,
    hand: player.hand.map(cloneCard),
    awaiting_attack: player.awaiting_attack,
    strategy_key: player.strategy_key
});

export const cloneGame = (game: Game): Game => ({
    id: game.id,
    name: game.name,
    deck_length: game.deck_length,
    discard_pile_length: game.discard_pile_length,
    flipped: game.flipped ? cloneCard(game.flipped) : null,
    players: game.players.map(clonePlayer),
    status: game.status,
    power_suit: game.power_suit,
    first_attacker: game.first_attacker,
    defender: game.defender,
    table_battles: game.table_battles.map(cloneBattle),
    elimination_order: [...game.elimination_order],
    good_timestamp: game.good_timestamp,
    good_players: [...game.good_players],
    deck: game.deck.map(cloneCard),
    logs: game.logs.map(cloneGameLog)
});

export const get_next_player_index = (game: PublicGame, current_player: number): number => {
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

export const getCardValue = (card: Card, powerSuit: number): number => {
    let baseValue = card.value;
    if (card.suit === powerSuit) {
        baseValue += 20; // Trump bonus
    }
    return baseValue;
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

// Temporary: Seeded RNG for deterministic testing

let seed = 1237;
export function seededRandom() {
    seed = (seed * 1664525 + 1013904223) % 4294967296;
    return seed / 4294967296;
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
    // TEMP: Use seeded random for deterministic testing
    const index = Math.floor(Math.random() * game.deck.length);
    // const index = Math.floor(Math.random() * game.deck.length);
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

export const personalize_game = (game: Game, player_id: string): PersonalGame | PublicGame => {
    // everything except game_decks , added self
    const self = game.players.find(player => player.player_id === player_id)!;
    if (self) {
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
            good_timestamp: game.good_timestamp,
            good_players: game.good_players,
            self: self
        }
        return personalGame;
    } else {
        const publicGame: PublicGame = {
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
            good_timestamp: game.good_timestamp,
            good_players: game.good_players,
        }
        return publicGame;
    }
}

// Standard ELO rating calculation. K-factor lowered from 32 -> 10 so live-game
// ratings move in smaller, less jumpy steps (mirrored in src/common/common_utils.ts).
export const calculateEloChange = (playerRating: number, opponentRating: number, actualScore: number, kFactor: number = 10): number => {
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

// Refill logic that creates animation events for cards drawn.
// Note: canonical Durak rules say the defender draws last (and never first).
// We intentionally deviate: defender draws in clockwise rotation slot, and
// draws first when their hand was emptied on a clean cover.
export const refillPlayerHandsWithEvents = (game: Game): { refillEvents: any[], drawLogs: any[] } => {
    const refillEvents: any[] = [];
    const drawLogs: any[] = []; // Track draw events for game logs

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
        return { refillEvents, drawLogs };
    }

    // If the deck was already empty, defending should've gotten them a win
    // most importantly, check if defender cleared their hand
    const defenseHand = game.players[game.defender].hand;
    if (defenseHand.length === 0) {
        // they draw first
        const defenderInitialHandSize = defenseHand.length;
        const drawnCards: Card[] = [];
        
        while (defenseHand.length < CARDS_PER_PLAYER) {
            const isFlippedNext = game.deck.length === 0 && game.flipped !== null;
            const c = draw(game);
            if (c === null) {
                break;
            }
            defenseHand.push(c);
            
            // Track drawn cards: known if it was the flipped card, unknown otherwise
            drawnCards.push(isFlippedNext ? c : { suit: -1, value: -1 });
        }
        
        // Add refill event for defender if they drew cards
        const defenderCardsDrawn = defenseHand.length - defenderInitialHandSize;
        if (defenderCardsDrawn > 0) {
            const actualCardsDrawn = defenseHand.slice(-defenderCardsDrawn);
            refillEvents.push({
                type: 'refill',
                player_id: game.players[game.defender].player_id,
                cards: actualCardsDrawn,
                from_location: 'deck',
                to_location: 'hand',
                message: `${game.players[game.defender].name} drew ${defenderCardsDrawn} cards`,
                game_state: cloneGame(game)
            });

            // Add draw log
            drawLogs.push({
                player_id: game.players[game.defender].player_id,
                cards: drawnCards
            });
        }
    }

    // Then go around starting from firstAttacker
    let pIndex = game.first_attacker;
    const visited = new Set<number>();
    do {
        // Bail when we'd revisit — first_attacker may be marked OUT inside
        // the body, in which case get_next_player_index never returns it.
        if (visited.has(pIndex)) break;
        visited.add(pIndex);
        const hand = game.players[pIndex].hand;
        const initialHandSize = hand.length;
        const drawnCards: Card[] = [];

        while (hand.length < CARDS_PER_PLAYER) {
            const isFlippedNext = game.deck.length === 0 && game.flipped !== null;
            const c = draw(game);
            if (c === null) {
                break;
            }
            hand.push(c);
            
            // Track drawn cards: known if it was the flipped card, unknown otherwise
            drawnCards.push(isFlippedNext ? c : { suit: -1, value: -1 });
        }
        
        // Add refill event for this player if they drew cards
        const cardsDrawn = hand.length - initialHandSize;
        if (cardsDrawn > 0) {
            const actualCardsDrawn = hand.slice(-cardsDrawn);
            refillEvents.push({
                type: 'refill',
                player_id: game.players[pIndex].player_id,
                cards: actualCardsDrawn,
                from_location: 'deck',
                to_location: 'hand',
                message: `${game.players[pIndex].name} drew ${cardsDrawn} cards`,
                game_state: cloneGame(game)
            });

            // Add draw log
            drawLogs.push({
                player_id: game.players[pIndex].player_id,
                cards: drawnCards
            });
        }
        
        // Check if player has no cards and should be marked as OUT
        if (hand.length === 0 && game.players[pIndex].status === PLAYER_STATUS.IN) {
            game.players[pIndex].status = PLAYER_STATUS.OUT;
            game.players[pIndex].awaiting_attack = false;
            game.elimination_order.push(game.players[pIndex].player_id);
        }
        
        pIndex = get_next_player_index(game, pIndex);
    } while (pIndex !== game.first_attacker);
    
    return { refillEvents, drawLogs };
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


// Stats the game with all the animations
export const start_game = (game: Game): AnimationEvent[] => {
    // Guard against starting game if it's already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return [];
    }

    const events: AnimationEvent[] = [];

    // Log game start - marks the beginning of this play session
    addLog(game, {
        game_id: game.id,
        log_type: LOG_TYPE.GAME_START,
        player_id: null, // System event
        card_pairs: [],
        defender_index: null
    });

    // This is the game entry
    game.status = GAME_STATUS.PLAYING;
    game.players.forEach(player => {
        player.status = PLAYER_STATUS.IN;
    });

    game.deck = refill_deck(game.players.length);
    game.elimination_order = []; // Initialize elimination order tracking
    game.good_timestamp = null; // Initialize good timestamp
    game.good_players = []; // Initialize good players list

    // Lead with a transition into the PLAYING view with empty hands, so the
    // client switches from Lobby to GameDisplay before the first DEAL fires.
    events.push({
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `All players ready - starting game!`,
        game_state: cloneGame(game)
    });

    // Draw and emit per player so each DEAL snapshot reflects the deck
    // drained by exactly that player's batch (deck: 36 → 30 → 24 → ...).
    // initialize_hands() can't be used here because it drains the full
    // 6×N cards up front, baking a post-deal deck count into every snapshot.
    for (let i = 0; i < game.players.length; i++) {
        const hand: Card[] = [];
        for (let k = 0; k < CARDS_PER_PLAYER; k++) {
            hand.push(draw(game)!);
        }
        game.players[i].hand = hand;
        events.push({
            type: ANIMATION_EVENT_TYPE.DEAL,
            player_id: game.players[i].player_id,
            cards: hand,
            from_location: 'deck',
            to_location: 'hand',
            game_state: cloneGame(game)
        });
    }

    let flipped_card = draw(game);
    while (flipped_card!.value === ACE_VALUE) {
        // move back to deck
        game.deck.push(flipped_card!);
        flipped_card = draw(game);
    }
    game.flipped = flipped_card;
    game.power_suit = game.flipped!.suit;

    // Add flipped card animation AFTER deal animations
    events.push({
        type: ANIMATION_EVENT_TYPE.FLIPPED,
        cards: [game.flipped!],
        from_location: 'deck',
        to_location: 'flipped',
        game_state: cloneGame(game)
    });

    const lowest_power_index = determine_lowest_power_index(game);
    game.first_attacker = lowest_power_index;
    set_positions(game);

    // Add animation event for defender position
    if (game.players[game.defender]) {
        events.push({
            type: ANIMATION_EVENT_TYPE.DEFENDER_MOVE,
            player_id: game.players[game.defender].player_id,
            game_state: cloneGame(game)
        });
    }

    // First attacker notification will be included in the start game animation sequence
    events.push({
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `Player ${game.players[lowest_power_index].name} is the first attacker, wait for them to attack`,
        game_state: cloneGame(game)
    });

    // Send private messages to players (these don't go through animation events)
    // I have not once actually seen this so I'm removing it
    /*for (let i = 0; i < game.players.length; i++) {
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
    }*/

    return events;
}

// Helper function to add a log to the game's pending logs
// This is what action handlers should call instead of saving directly to DB
export const addLog = (game: Game, log: UnsavedGameLog): void => {
    const savedLog: GameLog = {
        ...log,
        id: crypto.randomUUID(),
        created_at: new Date().toISOString()
    };
    game.logs.push(savedLog);
};
