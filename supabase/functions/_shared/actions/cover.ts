import { Card, Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, SERVER_EVENT_TYPE } from '../types.ts';
import { saveCompleteGame, executeWithGameLock, check_win, broadcastToGameUsers } from '../utils.ts';
import { canCover, get_next_player_index, validate_defender_status, verify_cards_in_players_hand, card_comp, cardDisplay, refillPlayerHands } from '../common_utils.ts';
import { scheduleBotActions } from '../bot_actions.ts';

// Validation function for cover moves
export function validateCover(game: Game, player_id: string, cover_cards: Card[], attack_cards: Card[]): void {
    if (game.status !== GAME_STATUS.FREE_PLAY && game.status !== GAME_STATUS.ONLY_DEFEND) {
        throw new Error(`Game ${game.id} is not in free_play or only_defend mode`);
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
export async function executeCover(game: Game, player_id: string, cover_cards: Card[], attack_cards: Card[]): Promise<void> {
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
    }

    // remove the cards from the hand
    defender.hand = defender.hand.filter(card => !cover_cards.some(cover_card => card_comp(card, cover_card)));

    // If defender has no cards left, they may win
    if (defender.hand.length === 0) {
        game.table_battles = [];
        refillPlayerHands(game);
        game.first_attacker = game.defender;
        // Reset done_attacking_this_round flag for all players when attacking shifts
        game.players.forEach(player => {
            player.done_attacking_this_round = false;
        });
        if (defender.hand.length === 0) {
            game.players[game.first_attacker].status = PLAYER_STATUS.OUT;
            game.elimination_order.push(game.players[game.first_attacker].player_id);
            await check_win(game);
            game.first_attacker = get_next_player_index(game, game.first_attacker);
        }
        game.defender = get_next_player_index(game, game.first_attacker);
        return;
    }

    // Check if all attacks are covered
    const all_attacks_covered = game.table_battles.every(battle => battle.defense !== null);
    if (all_attacks_covered) {
        game.status = GAME_STATUS.WAIT_FOR_ATTACKERS;

        // Check who can still play cards
        const playable_values = new Set<number>();
        for (const battle of game.table_battles) {
            playable_values.add(battle.attack.value)
            if (battle.defense !== null) {
                playable_values.add(battle.defense.value);
            }
        }

        const playable_players = game.players.filter(player => player.player_id !== player_id && player.hand.some(card => playable_values.has(card.value))).map(player => player.player_id);

        if (playable_players.length === 0) {
            // No one can play, end the round
            setTimeout(async () => {
                await executeWithGameLock(game.id, async () => {
                    // Reload game to ensure we have the latest state
                    const { loadCompleteGame } = await import('../utils.ts');
                    const currentGame = await loadCompleteGame(game.id);
                    
                    currentGame.table_battles = [];
                    refillPlayerHands(currentGame);
                    currentGame.first_attacker = currentGame.defender;
                    currentGame.defender = get_next_player_index(currentGame, currentGame.first_attacker);
                    currentGame.status = GAME_STATUS.FIRST_ATTACKER;
                    // Reset done_attacking_this_round flag for all players when attacking shifts
                    currentGame.players.forEach(player => {
                        player.done_attacking_this_round = false;
                    });
                    await saveCompleteGame(currentGame);

                    broadcastToGameUsers(currentGame, 'game_update', {
                        type: SERVER_EVENT_TYPE.SUCCESSFULLY_COVERED,
                        message: `Player ${defender.name} successfully covered ${attack_cards.map(card => cardDisplay(card)).join(', ')}`
                    });

                    // Schedule bot actions if the new first attacker is a bot. We only do this because this is async
                    if (currentGame.players[currentGame.first_attacker].is_ai) {
                        scheduleBotActions(currentGame.id);
                    }
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
}

// Combined function with validation
export async function handleCover(game: Game, player_id: string, cover_cards: Card[], attack_cards: Card[]): Promise<void> {
    validateCover(game, player_id, cover_cards, attack_cards);
    await executeCover(game, player_id, cover_cards, attack_cards);
} 