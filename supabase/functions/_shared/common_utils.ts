import { Card, Game, PersonalGame, PLAYER_STATUS, PrivatePlayer, PublicPlayer, GAME_STATUS, PublicGame, LOG_TYPE, AnimationEvent, ANIMATION_EVENT_TYPE, Battle, LogCardPair } from "./types.ts";
import { GameLog, UnsavedGameLog } from './types.ts';
import { ACE_VALUE, CARDS_PER_PLAYER, SUITS, VALUE_MAP, SUIT_MAP, MAX_PLAYERS } from './constants.ts';
import { kernelStartGame, kernelRefill } from './wasm/engine.ts';

// Fast deep clone for Game objects - avoids expensive JSON.parse(JSON.stringify())
const cloneCard = (card: Card): Card => ({ suit: card.suit, value: card.value });

const cloneBattle = (battle: Battle): Battle => ({
    attack: cloneCard(battle.attack),
    defense: battle.defense ? cloneCard(battle.defense) : null
});

const cloneCardPair = (pair: LogCardPair): LogCardPair => ({
    primary: cloneCard(pair.primary),
    target: pair.target ? cloneCard(pair.target) : pair.target
});

const cloneGameLog = (log: GameLog): GameLog => ({
    id: log.id,
    created_at: log.created_at,
    game_id: log.game_id,
    log_type: log.log_type,
    player_id: log.player_id,
    card_pairs: log.card_pairs.map(cloneCardPair),
    defender_index: log.defender_index
});

const clonePlayer = (player: PrivatePlayer): PrivatePlayer => ({
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

// Guard a card-array argument from a client payload BEFORE any handler logic
// touches it, so a malformed payload (non-array, or cards missing numeric
// suit/value) becomes a clean 400 rejection rather than a confusing TypeError
// ("cards is not iterable") or a "Card undefined of undefined" message deep in
// validation.
export const verify_card_array = (cards: unknown, label: string): asserts cards is Card[] => {
    if (!Array.isArray(cards)) {
        throw new Error(`${label} must be an array of cards`);
    }
    for (const card of cards) {
        if (!card || typeof card !== 'object' || typeof (card as any).suit !== 'number' || typeof (card as any).value !== 'number') {
            throw new Error(`${label} contains an invalid card`);
        }
    }
};

export const verify_cards_in_players_hand = (player: PrivatePlayer, cards: Card[]) => {
    for (const card of cards) {
        if (!player.hand.some(handCard => card_comp(handCard, card))) {
            throw new Error(`Card ${cardDisplay(card)} is not in player ${player.player_id}'s hand`);
        }
    }
}



// Dealing, drawing and first-attacker selection live in the C kernel
// (cnitro/src/game.c): per-draw random splice, non-Ace trump flip,
// lowest-trump-holder attacks first. The old TS helpers (draw, refill_deck,
// no_cards_left, seededRandom, set_positions, initialize_hands,
// determine_lowest_power_index) were deleted with it — nothing outside this
// file imported them.

// Thin projection kept for synchronous use; the kernel counterpart is
// game_done in cnitro/src/game.c (e2e/wasm_engine.test.ts polices parity).
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
// ratings move in smaller, less jumpy steps.
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

// Refill logic — lives in the C kernel (cnitro/src/game.c
// refill_player_hands), compiled to WASM. The kernel keeps the deliberate
// deviation from canonical Durak: the defender draws in clockwise rotation
// slot, and draws FIRST when their hand was emptied on a clean cover.
// Exported for API compatibility; the action handlers run refill inside
// their kernel transition.
export const refillPlayerHandsWithEvents = (game: Game): { refillEvents: any[], drawLogs: any[] } => {
    return kernelRefill(game);
};

// Starts the game with all the animations. The deal/flip/first-attacker
// rules live in the C kernel (cnitro/src/game.c start_game): player-major
// deal, non-Ace trump flip (Aces pushed back and redrawn), lowest-trump
// holder attacks first. The event stream (MAGIC → per-player DEAL → FLIPPED →
// DEFENDER_MOVE → MAGIC) is reconstructed from kernel snapshots, identical
// to the old TS implementation (verified by the differential parity harness).
export const start_game = (game: Game): AnimationEvent[] => {
    // Guard against starting game if it's already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return [];
    }
    // Defense in depth: the lobby caps players at MAX_PLAYERS, but never deal
    // an oversized lobby — more hands than the deck holds leaves a player with
    // no cards and crashes the deal. Reject cleanly instead.
    if (game.players.length > MAX_PLAYERS) {
        throw new Error(`Cannot start a game with ${game.players.length} players (max ${MAX_PLAYERS})`);
    }
    return kernelStartGame(game);
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
