import { Card, Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, AnimationEvent, ANIMATION_EVENT_TYPE, LOG_TYPE } from '../types.ts';
import { addLog, cloneGame } from '../common_utils.ts';
import { canCover, get_next_player_index, validate_defender_status, verify_cards_in_players_hand, verify_card_array, card_comp, cardDisplay, refillPlayerHandsWithEvents } from '../common_utils.ts';

// Validation function for cover moves
export function validateCover(game: Game, player_id: string, cover_cards: Card[], attack_cards: Card[]): void {
    // Can only cover during playing state
    if (game.status !== GAME_STATUS.PLAYING) {
        throw new Error(`Game ${game.id} is not in playing state`);
    }

    verify_card_array(cover_cards, 'cover_cards');
    verify_card_array(attack_cards, 'attack_cards');

    const uncoveredAttacks = game.table_battles.filter(battle => battle.defense === null);
    if (uncoveredAttacks.length === 0) {
        throw new Error('No uncovered attacks to cover');
    }

    // check if player is the defender
    validate_defender_status(game, player_id, true);

    const defender: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;

    // verify cards in hand
    verify_cards_in_players_hand(defender, cover_cards);

    // check no duplicates
    if (new Set(cover_cards.map(c => `${c.suit}-${c.value}`)).size !== cover_cards.length) {
        throw new Error(`Cards ${cover_cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // ensure that each of the attack cards are on the table AND uncovered.
    // Match by EXACT card (suit+value), not just value: executeCover locates the
    // battle with card_comp, so a value-only check here lets a request naming an
    // already-covered card slip past validation when another same-rank attack is
    // still uncovered, then throw the uncaught 'SEVERE: Card not found on table'
    // in execution (reachable via a defender double-tapping cover on one of two
    // same-rank attacks).
    for (const card of attack_cards) {
        if (!game.table_battles.some(battle => card_comp(battle.attack, card) && battle.defense === null)) {
            throw new Error(`Card ${cardDisplay(card)} is not on the table`);
        }
    }

    // check no duplicates
    if (new Set(attack_cards.map(c => `${c.suit}-${c.value}`)).size !== attack_cards.length) {
        throw new Error(`Cards ${attack_cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // assert same size of arrays — BEFORE the canCover loop below, which pairs
    // cover_cards[i] with attack_cards[i]; a mismatched length would otherwise
    // index undefined and throw a "reading 'suit'" TypeError instead of this clean
    // rejection.
    if (cover_cards.length !== attack_cards.length) {
        throw new Error(`Cover cards ${cover_cards.map(card => cardDisplay(card)).join(', ')} and attack cards ${attack_cards.map(card => cardDisplay(card)).join(', ')} have different sizes`);
    }

    // can they cover?
    for (let i = 0; i < cover_cards.length; i++) {
        const cover_card = cover_cards[i];
        const attack_card = attack_cards[i];
        if (!canCover(attack_card, cover_card, game.power_suit)) {
            throw new Error(`Card ${cardDisplay(cover_card)} cannot cover ${cardDisplay(attack_card)}`);
        }
    }
}

// Execution function for cover moves
export function executeCover(game: Game, player_id: string, cover_cards: Card[], attack_cards: Card[]): AnimationEvent[] {
    const events: AnimationEvent[] = [];
    
    // Guard against modifying game state if game is already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return events;
    }
    
    const defender: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;

    // now cover the cards
    for (let i = 0; i < cover_cards.length; i++) {
        const cover_card = cover_cards[i];
        const attack_card = attack_cards[i];
        const attack_card_index = game.table_battles.findIndex(battle => card_comp(battle.attack, attack_card) && battle.defense === null);
        if (attack_card_index === -1) {
            throw new Error('SEVERE: Card not found on table');
        }
        game.table_battles[attack_card_index].defense = cover_card;
        
        // Remove the card from the hand immediately
        defender.hand = defender.hand.filter(card => !card_comp(card, cover_card));
        
        // Log the cover event with primary (cover card) and target (attack card)
        addLog(game, {
            game_id: game.id,
            log_type: LOG_TYPE.COVER,
            player_id: player_id,
            card_pairs: [{ primary: cover_card, target: attack_card }],
            defender_index: null
        });
        
        // Capture game state after this specific cover
        const gameStateAfterCover = cloneGame(game);
        
        // Add animation event for this cover with the intermediate game state
        events.push({
            type: ANIMATION_EVENT_TYPE.COVER,
            player_id: player_id,
            cards: [cover_card],
            target_card: attack_card,
            battle_index: attack_card_index,
            from_location: 'hand',
            to_location: 'table',
            message: `${defender.name} covered ${cardDisplay(attack_card)} with ${cardDisplay(cover_card)}`,
            game_state: gameStateAfterCover
        });
    }

    // If defender has no cards left, they may win
    if (defender.hand.length === 0) {
        // Count cards being discarded before clearing table_battles
        const discardedCards = game.table_battles.length * 2; // Each battle has attack + defense
        game.discard_pile_length += discardedCards;
        
        // Add discard event with current game state
        const allTableCards = game.table_battles.flatMap(battle => 
            battle.defense ? [battle.attack, battle.defense] : [battle.attack]
        );
        
        // Clear table battles
        game.table_battles = [];
        const gameStateAfterDiscard = cloneGame(game);
        
        // Log discard event
        addLog(game, {
            game_id: game.id,
            log_type: LOG_TYPE.DISCARD,
            player_id: null, // System event
            card_pairs: allTableCards.map(card => ({ primary: card, target: null })),
            defender_index: null
        });
        
        events.push({
            type: ANIMATION_EVENT_TYPE.DISCARD,
            cards: allTableCards,
            from_location: 'table',
            to_location: 'discard',
            message: `${allTableCards.length} cards discarded`,
            game_state: gameStateAfterDiscard
        });
        
        // Refill hands. Each refill event already carries its own
        // per-iteration snapshot via cloneGame.
        const { refillEvents, drawLogs } = refillPlayerHandsWithEvents(game);
        for (const refillEvent of refillEvents) {
            events.push(refillEvent);
        }
        
        // Add draw logs to game logs
        for (const drawLog of drawLogs) {
            addLog(game, {
                game_id: game.id,
                log_type: LOG_TYPE.DRAW,
                player_id: drawLog.player_id,
                card_pairs: drawLog.cards.map((card: Card) => ({ primary: card, target: null })),
                defender_index: null
            });
        }
        
        game.first_attacker = game.defender;
        
        // Reset good fields when round ends
        game.good_players = [];
        game.good_timestamp = null;
        
        if (defender.hand.length === 0) {
            // Defender still has no cards after refilling - they win this round.
            // Guard: refill's no_cards_left branch may have already marked
            // them OUT and pushed to elimination_order. Skip if already done.
            const wasIn = game.players[game.first_attacker].status === PLAYER_STATUS.IN;
            game.players[game.first_attacker].status = PLAYER_STATUS.OUT;
            game.players[game.first_attacker].awaiting_attack = false;
            if (wasIn) game.elimination_order.push(game.players[game.first_attacker].player_id);
            
            // Log player going out
            addLog(game, {
                game_id: game.id,
                log_type: LOG_TYPE.PLAYER_OUT,
                player_id: game.players[game.first_attacker].player_id,
                card_pairs: [],
                defender_index: null
            });
            
            // Capture state after player goes out
            const gameStateAfterOut = cloneGame(game);
            
            // Add out event
            events.push({
                type: ANIMATION_EVENT_TYPE.OUT,
                player_id: game.players[game.first_attacker].player_id,
                message: `${game.players[game.first_attacker].name} is out`,
                game_state: gameStateAfterOut
            });
            
            game.first_attacker = get_next_player_index(game, game.first_attacker);
        }
        
        game.defender = get_next_player_index(game, game.first_attacker);
        
        // Log defender change
        addLog(game, {
            game_id: game.id,
            log_type: LOG_TYPE.DEFENDER_CHANGE,
            player_id: null, // System event
            card_pairs: [],
            defender_index: game.defender
        });
        
        // Capture state after defender move
        const gameStateAfterDefenderMove = cloneGame(game);
        
        // Add defender move event
        events.push({
            type: ANIMATION_EVENT_TYPE.DEFENDER_MOVE,
            player_id: game.players[game.defender].player_id,
            message: `${game.players[game.defender].name} is now the defender`,
            game_state: gameStateAfterDefenderMove
        });
        
        // Game continues in playing state (no status change needed)
        return events;
    }

    // Reset good fields since board state changed
    // Defense cards introduce new values to the table, so attackers should be able to reconsider
    // This happens on EVERY cover, not just when all attacks are covered
    game.good_players = [];
    game.good_timestamp = null;
    
    // Check if all attacks are covered
    // Note: every() returns true for empty arrays, but we should have battles after covering
    const all_attacks_covered = game.table_battles.length > 0 && 
        game.table_battles.every(battle => battle.defense !== null);
    if (all_attacks_covered) {
        // All attacks are covered - set timestamp to start the 60-second countdown
        game.good_timestamp = Date.now();
        //console.log(`All attacks covered at ${game.good_timestamp}. Starting 60-second countdown.`);
        
        // Start auto-discard monitoring loop (fire-and-forget)
        // The state we get into will trigger the auto-discard loop automatically in utils.ts

        // All attacks are covered - no status change needed, game continues in playing state
        // Attackers must now press 'good' to proceed

        // Set awaiting_attack to true for all non-defender, non-out players
        game.players.forEach((p, index) => {
            if (index !== game.defender && p.status === PLAYER_STATUS.IN) {
                p.awaiting_attack = true;
            }
        });
    }
    
    return events;
}

// Combined function with validation
export function handleCover(game: Game, player_id: string, cover_cards: Card[], attack_cards: Card[]): AnimationEvent[] {
    validateCover(game, player_id, cover_cards, attack_cards);
    return executeCover(game, player_id, cover_cards, attack_cards);
} 
