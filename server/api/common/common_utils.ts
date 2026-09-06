// NO import of ./wasm/engine.ts here: this module is shared with the CLIENT
// (canCover, personalize_game, clone helpers), and a static engine import
// would drag the embedded wasm base64 into every page bundle. The kernel-
// delegating start_game lives in ./game_lifecycle.ts (server/tests only).
import { Card, Game, GAME_STATUS, PersonalGame, PLAYER_STATUS, PrivatePlayer, PublicPlayer, PublicGame, Battle, LogCardPair } from "@api/core/types.ts";
import { GameLog, UnsavedGameLog } from '@api/core/types.ts';
import { VALUE_MAP, SUIT_MAP } from '@api/core/constants.ts';
import { derivedUuid } from '@sdk/ts/wire/detid.ts';

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

// Kernel twin: get_next_player_index in c/src/game.c, which carries the same
// one-player guard. The browser no longer calls this - it goes through
// clientGuards.nextPlayerIndex - but the frozen bot oracle
// offlinefun/localtest/frozen/champion_strategy.ts still does, and this module
// must not import the engine (see the header). e2e/wasm_engine.test.ts polices
// the parity.
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

// Kernel twin: can_cover in c/src/game.c. The interactive board now asks the
// kernel directly (clientGuards.canCoverPair); what is left here are the frozen
// bot oracles in offlinefun/localtest/frozen/, which e2e/bot_parity.test.ts
// drives against the kernel and which must not gain a wasm dependency.
// e2e/wasm_engine.test.ts polices the parity over the full card cross-product.
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
// (c/src/game.c): per-draw random splice, non-Ace trump flip,
// lowest-trump-holder attacks first. The old TS helpers (draw, refill_deck,
// no_cards_left, seededRandom, set_positions, initialize_hands,
// determine_lowest_power_index) were deleted with it — nothing outside this
// file imported them.

// Turn-eligibility projection: whether the seat may act in the current
// state. The kernel counterpart is should_bot_act in c/src/game.c
// (e2e/wasm_engine.test.ts polices parity). Lives here — NOT in
// pure_bot_actions.ts, which pulls the wasm embeds — so the client can
// share it (AnimationContext's bot-move poll gate).
export const shouldBotActCore = (game: Game, bot: PrivatePlayer, botIndex: number): boolean => {
    if (game.status !== GAME_STATUS.PLAYING) {
        return false;
    }

    // Check if bot is out - they should never act
    if (bot.status !== PLAYER_STATUS.IN) {
        return false;
    }

    const isFirstAttack = game.table_battles.length === 0;
    const isDefender = botIndex === game.defender;
    // Note: every() returns true for empty arrays, so check length first
    const allAttacksCovered = game.table_battles.length > 0 &&
        game.table_battles.every(battle => battle.defense !== null);

    if (isFirstAttack) {
        // First attack: only first attacker can act
        return botIndex === game.first_attacker;
    }

    if (isDefender) {
        // Defender can only act when there are uncovered attacks
        // If all attacks are covered, defender just waits for attackers to add more or say "good"
        return !allAttacksCovered;
    }

    // Attacker is eligible iff they haven't said "good" yet. good_players is the single
    // source of truth — awaiting_attack used to gate this too, but the two could drift out
    // of sync and deadlock the round.
    return !game.good_players?.includes(bot.player_id);
}

// Kernel twin: game_done in c/src/game.c (e2e/wasm_engine.test.ts polices the
// parity). No browser caller; this serves the Supabase edge's synchronous
// check_win_sync and the Node harnesses, neither of which can reach
// clientGuards - the edge would have to swallow the guards.wasm embed, and
// check_win_sync would have to become async.
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
            // The optimistic-concurrency token: authoritative REST loads seed
            // the client's broadcast reorder-drop gate from it (the packed
            // responses carry it in their envelope).
            version: game.version,
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
            version: game.version,
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

// Refill logic lives in the C kernel (c/src/game.c refill_player_hands),
// run inside the action handlers' kernel transitions; game start is the
// kernel-delegating start_game in ./game_lifecycle.ts (kept out of this
// module so the client bundle never pulls the wasm embed). The old TS
// refillPlayerHandsWithEvents compatibility wrapper had no callers left and
// was deleted with the split.

// Helper function to add a log to the game's pending logs
// This is what action handlers should call instead of saving directly to DB
//
// The id is DERIVED from (game id, position in the log list) rather than drawn.
// Nothing reads it - cloneGameLog above copies it and that is the only mention
// in the codebase - so the entropy bought nothing, and it cost the ability to
// compare one run of a game against another. Same rule as appendLogs in
// sdk/ts/wasm/engine.ts; see sdk/ts/wire/detid.ts for why.
export const addLog = (game: Game, log: UnsavedGameLog): void => {
    const savedLog: GameLog = {
        ...log,
        id: derivedUuid(game.id, game.logs.length),
        created_at: new Date().toISOString()
    };
    game.logs.push(savedLog);
};
