import { Card, Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, SERVER_EVENT_TYPE, AnimationEvent, ANIMATION_EVENT_TYPE } from '../types.ts';
import { executeWithGameLock, check_win, broadcastToGameUsers } from '../utils.ts';
import { canCover, get_next_player_index, validate_defender_status, verify_cards_in_players_hand, card_comp, cardDisplay, refillPlayerHandsWithEvents } from '../common_utils.ts';
import { lockedBotLoop } from '../bot_actions.ts';

// Validation function for cover moves
export function validateCover(game: Game, player_id: string, cover_cards: Card[], attack_cards: Card[]): void {
    // Can only cover during playing state
    if (game.status !== GAME_STATUS.PLAYING) {
        throw new Error(`Game ${game.id} is not in playing state`);
    }

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
    if (new Set(cover_cards).size !== cover_cards.length) {
        throw new Error(`Cards ${cover_cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // ensure that each of the attack cards are on the table AND uncovered
    for (const card of attack_cards) {
        if (!game.table_battles.some(battle => battle.attack.value === card.value && battle.defense === null)) {
            throw new Error(`Card ${cardDisplay(card)} is not on the table`);
        }
    }

    // check no duplicates
    if (new Set(attack_cards).size !== attack_cards.length) {
        throw new Error(`Cards ${attack_cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // can they cover?
    for (let i = 0; i < cover_cards.length; i++) {
        const cover_card = cover_cards[i];
        const attack_card = attack_cards[i];
        if (!canCover(attack_card, cover_card, game.power_suit)) {
            throw new Error(`Card ${cardDisplay(cover_card)} cannot cover ${cardDisplay(attack_card)}`);
        }
    }

    // assert same size of arrays
    if (cover_cards.length !== attack_cards.length) {
        throw new Error(`Cover cards ${cover_cards.map(card => cardDisplay(card)).join(', ')} and attack cards ${attack_cards.map(card => cardDisplay(card)).join(', ')} have different sizes`);
    }
}

// Execution function for cover moves
export async function executeCover(game: Game, player_id: string, cover_cards: Card[], attack_cards: Card[], skipBroadcast: boolean = false): Promise<AnimationEvent[]> {
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
        
        // Add animation event for this cover
        events.push({
            type: ANIMATION_EVENT_TYPE.COVER,
            player_id: player_id,
            cards: [cover_card],
            target_card: attack_card,
            battle_index: attack_card_index,
            from_location: 'hand',
            to_location: 'table',
            message: `${defender.name} covered ${cardDisplay(attack_card)} with ${cardDisplay(cover_card)}`
        });
    }

    // remove the cards from the hand
    defender.hand = defender.hand.filter(card => !cover_cards.some(cover_card => card_comp(card, cover_card)));

    // If defender has no cards left, they may win
    if (defender.hand.length === 0) {
        // Count cards being discarded before clearing table_battles
        const discardedCards = game.table_battles.length * 2; // Each battle has attack + defense
        game.discard_pile_length += discardedCards;
        
        // Add discard event
        const allTableCards = game.table_battles.flatMap(battle => 
            battle.defense ? [battle.attack, battle.defense] : [battle.attack]
        );
        events.push({
            type: ANIMATION_EVENT_TYPE.DISCARD,
            cards: allTableCards,
            from_location: 'table',
            to_location: 'discard',
            message: `${allTableCards.length} cards discarded`
        });
        
        game.table_battles = [];
        const { refillEvents } = refillPlayerHandsWithEvents(game);
        events.push(...refillEvents);
        game.first_attacker = game.defender;
        // Reset done_attacking_this_round flag for all players when attacking shifts
        game.players.forEach(player => {
            player.done_attacking_this_round = false;
        });
        if (defender.hand.length === 0) {
            // Defender still has no cards after refilling - they win this round
            game.players[game.first_attacker].status = PLAYER_STATUS.OUT;
            game.players[game.first_attacker].awaiting_attack = false;
            game.elimination_order.push(game.players[game.first_attacker].player_id);
            
            // Add out event
            events.push({
                type: ANIMATION_EVENT_TYPE.OUT,
                player_id: game.players[game.first_attacker].player_id,
                message: `${game.players[game.first_attacker].name} is out`
            });
            
            await check_win(game);
            game.first_attacker = get_next_player_index(game, game.first_attacker);
        }
        game.defender = get_next_player_index(game, game.first_attacker);
        
        // Add defender move event
        events.push({
            type: ANIMATION_EVENT_TYPE.DEFENDER_MOVE,
            player_id: game.players[game.defender].player_id,
            message: `${game.players[game.defender].name} is now the defender`
        });
        
        // Game continues in playing state (no status change needed)
        return events;
    }

    // Check if all attacks are covered
    const all_attacks_covered = game.table_battles.every(battle => battle.defense !== null);
    if (all_attacks_covered) {
        // All attacks are covered - no status change needed, game continues in playing state

        // Check who can still play cards
        const playable_values = new Set<number>();
        for (const battle of game.table_battles) {
            playable_values.add(battle.attack.value)
            if (battle.defense !== null) {
                playable_values.add(battle.defense.value);
            }
        }

        const playable_players = game.players.filter(player => 
            player.player_id !== player_id && 
            player.hand.some(card => playable_values.has(card.value)) &&
            !player.done_attacking_this_round
        ).map(player => player.player_id);

        if (playable_players.length === 0) {
            // No one can play, end the round
            setTimeout(async () => {
                await executeWithGameLock(game.id, async (currentGame) => {
                    // Count cards being discarded before clearing table_battles
                    const discardedCards = currentGame.table_battles.length * 2; // Each battle has attack + defense
                    currentGame.discard_pile_length += discardedCards;
                    
                    // Add discard event
                    const allTableCards = currentGame.table_battles.flatMap(battle => 
                        battle.defense ? [battle.attack, battle.defense] : [battle.attack]
                    );
                    const discardEvent: AnimationEvent = {
                        type: ANIMATION_EVENT_TYPE.DISCARD,
                        cards: allTableCards,
                        from_location: 'table',
                        to_location: 'discard',
                        message: `${allTableCards.length} cards discarded`
                    };
                    
                    currentGame.table_battles = [];
                    const { refillEvents } = refillPlayerHandsWithEvents(currentGame);
                    
                    currentGame.first_attacker = currentGame.defender;
                    currentGame.defender = get_next_player_index(currentGame, currentGame.first_attacker);
                    
                    // Reset done_attacking_this_round flag for all players when attacking shifts
                    currentGame.players.forEach(player => {
                        player.done_attacking_this_round = false;
                    });
                    
                    // Check if game should end after refilling - at the very end
                    await check_win(currentGame);
                    
                    // Game continues in playing state (no status change needed unless game is over)
                    
                    // Broadcast the discard event and refill events
                    if (!skipBroadcast) {
                        const allEvents = [discardEvent, ...refillEvents];
                        await broadcastToGameUsers(currentGame, 'animation_events', {
                            type: 'animation_sequence',
                            events: allEvents,
                            sequence_id: crypto.randomUUID(),
                            timestamp: Date.now()
                        });
                    }

                    // Schedule bot actions if the new first attacker is a bot
                    if (currentGame.players[currentGame.first_attacker].is_ai) {
                        lockedBotLoop(currentGame.id);
                    }
                    
                    // Return the standardized format
                    return { game: currentGame, events: [discardEvent, ...refillEvents] };
                });
            }, 1000 + Math.random() * 5000);
        } else {
            // Someone can play cards
            game.players.forEach(player => {
                if (playable_players.includes(player.player_id)) {
                    player.awaiting_attack = true;
                }
            });
        }
    }
    
    return events;
}

// Combined function with validation
export async function handleCover(game: Game, player_id: string, cover_cards: Card[], attack_cards: Card[]): Promise<AnimationEvent[]> {
    validateCover(game, player_id, cover_cards, attack_cards);
    return await executeCover(game, player_id, cover_cards, attack_cards, true);
} 
