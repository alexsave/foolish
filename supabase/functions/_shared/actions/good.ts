import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, AnimationEvent, ANIMATION_EVENT_TYPE, LOG_TYPE, Card } from '../types.ts';
import { refillPlayerHandsWithEvents, cloneGame } from '../common_utils.ts';
import { get_next_player_index } from '../common_utils.ts';
import { addLog } from '../common_utils.ts';

// Shared logic for discarding cards and transitioning to next round
// Used by both player "good" actions and auto-discard timeout
export function executeRoundTransition(game: Game, reason: string): AnimationEvent[] {
    const events: AnimationEvent[] = [];
    
    // Guard against modifying game state if game is already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return events;
    }

    console.log(`${reason} - proceeding to next round`);

    // Add animation event for magic transition
    const gameStateForTransition = cloneGame(game);
    events.push({
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `${reason} - proceeding to next round`,
        game_state: gameStateForTransition
    });

    // Count cards being discarded before clearing table_battles
    const discardedCards = game.table_battles.length * 2; // Each battle has attack + defense
    game.discard_pile_length += discardedCards;
    
    // Add animation event for cards going to discard pile
    const allTableCards = game.table_battles.flatMap(battle => 
        battle.defense ? [battle.attack, battle.defense] : [battle.attack]
    );
    
    // Clear table battles
    game.table_battles = [];
    
    // Log discard event
    addLog(game, {
        game_id: game.id,
        log_type: LOG_TYPE.DISCARD,
        player_id: null, // System event
        card_pairs: allTableCards.map(card => ({ primary: card, target: null })),
        defender_index: null
    });
    
    if (allTableCards.length > 0) {
        const gameStateAfterDiscard = cloneGame(game);
        const discardEvent: AnimationEvent = {
            type: ANIMATION_EVENT_TYPE.CARDS_TO_TRASH,
            cards: allTableCards,
            from_location: 'table',
            to_location: 'discard',
            message: `${allTableCards.length} cards discarded`,
            game_state: gameStateAfterDiscard
        };
        events.push(discardEvent);
    }
    
    // Refill player hands. Each event already carries its own per-iteration
    // snapshot via cloneGame, so the deck drains card-by-card and each
    // player's hand fills in turn (rather than all snapping at once).
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
    game.defender = get_next_player_index(game, game.first_attacker);
    
    // Log defender change
    addLog(game, {
        game_id: game.id,
        log_type: LOG_TYPE.DEFENDER_CHANGE,
        player_id: null,
        card_pairs: [],
        defender_index: game.defender
    });
    
    // Reset good fields when round ends
    game.good_players = [];
    game.good_timestamp = null;
    
    // Game continues in playing state (no status change needed unless game is over)
    return events;
}

// Validation function for good moves
function validateGood(game: Game, player_id: string): void {
    // Can only say good during playing state
    if (game.status !== GAME_STATUS.PLAYING) {
        throw new Error(`Game ${game.id} is not in playing state`);
    }

    const player = game.players.find(player => player.player_id === player_id)!;
    if (player.status !== PLAYER_STATUS.IN) {
        throw new Error(`Player ${player_id} is not ready to attack`);
    }

    // Defender cannot say good
    if (game.players[game.defender].player_id === player_id) {
        throw new Error(`Defender cannot say good`);
    }

    // First attacker cannot say good if table is empty (must make initial attack)
    const playerIndex = game.players.findIndex(p => p.player_id === player_id);
    if (game.table_battles.length === 0 && playerIndex === game.first_attacker) {
        throw new Error(`First attacker must attack - cannot say good with empty table`);
    }

    // Player must not have already said good
    if (game.good_players && game.good_players.includes(player_id)) {
        throw new Error(`Player has already said good`);
    }
}

// Execution function for good moves
function executeGood(game: Game, player_id: string): AnimationEvent[] {
    const events: AnimationEvent[] = [];
    
    // Guard against modifying game state if game is already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return events;
    }
    
    const player: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;

    // Initialize good_players array if it doesn't exist
    if (!game.good_players) {
        game.good_players = [];
    }

    // Add this player to the list of players who have said good
    if (!game.good_players.includes(player_id)) {
        game.good_players.push(player_id);
        
        // Log good event
        addLog(game, {
            game_id: game.id,
            log_type: LOG_TYPE.GOOD,
            player_id: player_id,
            card_pairs: [],
            defender_index: null
        });
    }

    // Set them to done attacking
    player.awaiting_attack = false;

    // Get all attackers (non-defender, non-out players)
    const allAttackers = game.players.filter((p, index) => 
        index !== game.defender && 
        p.status === PLAYER_STATUS.IN
    );

    // Check if all attackers have pressed good
    // Note: every() returns true for empty arrays, so check length first
    const allAttackersGood = allAttackers.length > 0 && allAttackers.every(attacker => 
        game.good_players.includes(attacker.player_id)
    );

    // Check if all attacks are covered
    // Note: every() returns true for empty arrays, so check length first
    const allAttacksCovered = game.table_battles.length > 0 && 
        game.table_battles.every(battle => battle.defense !== null);
    
    // Check if 1 minute has passed since good_timestamp was set
    const oneMinutePassed = game.good_timestamp !== null && game.good_timestamp !== undefined
        ? (Date.now() - game.good_timestamp >= 60000) 
        : false;

    // Only proceed to next round if all attackers said good AND all attacks are covered.
    // Timeout fallback disabled (was: allAttackersGood || /*oneMinutePassed*/) — long-game
    // sessions no longer auto-discard out from under absent attackers.
    const canTransition = (allAttackersGood /* || oneMinutePassed */) && allAttacksCovered;
    
    // Always log when someone says good
    console.log(`✅ ${player.name} said good` + (canTransition ? '' : `. Waiting: ` +
        `Attackers ${game.good_players.length}/${allAttackers.length}, ` +
        `Covered: ${allAttacksCovered}, ` +
        `${game.good_timestamp ? Math.max(0, Math.round((60000 - (Date.now() - game.good_timestamp)) / 1000)) : 60}s left`));
    
    if (!canTransition) {
        // Trigger auto-discard loop to monitor timeout (fire-and-forget)
        // The state we get into will trigger the auto-discard loop automatically in utils.ts
        
        return events;
    }

    // All conditions met - proceed to next round
    const transitionReason = allAttackersGood 
        ? `All ${allAttackers.length} attackers said good and all attacks covered`
        : `60-second timeout reached and all attacks covered (${game.good_players.length}/${allAttackers.length} attackers ready)`;

    // Use shared round transition logic
    return executeRoundTransition(game, transitionReason);
}

// Combined function with validation
export function handleGood(game: Game, player_id: string): AnimationEvent[] {
    validateGood(game, player_id);
    return executeGood(game, player_id);
} 