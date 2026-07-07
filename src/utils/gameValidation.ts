import { Card, PersonalGame, PublicPlayer } from '@shared/types.ts';
import { canCover, get_next_player_index } from '@shared/common_utils.ts';
import { findUnambiguousCover } from './coverCombinations';

// These mirror the kernel's validation rules (cnitro/src/game.c handle_*)
// for UI affordances and optimistic gating only — the server re-validates
// every request against the kernel, which stays authoritative. Keep each
// check in lockstep with its handle_* counterpart; e2e/attack_cover_parity
// and e2e/pass_parity fuzz them against the kernel.

// Every caller gates a SELF-initiated affordance/optimistic action, so the
// acting seat is always the local player's.
const selfSeat = (game: PersonalGame): number =>
    game.players.findIndex(player => player.player_id === game.self?.player_id);

// Boolean validation functions for UI (buttons/drag) - return true if valid
export const canAttack = (game: PersonalGame, cards: Card[]): boolean => {
    if (cards.length === 0) return false;

    // Get the defender's hand size
    const defenderHandSize = game.players[game.defender]?.hand_length || 0;

    // Calculate total UNCOVERED cards that would be on the table after this attack
    // Only count uncovered battles because covered battles are waiting for "good"
    const uncoveredBattles = game.table_battles.filter(battle => !battle.defense).length;
    const totalAfterAttack = uncoveredBattles + cards.length;

    // Cannot attack with more cards than defender can handle
    if (totalAfterAttack > defenderHandSize) {
        return false;
    }

    if (game.table_battles.length === 0) {
        // First attack of the bout: only the first attacker may open (the
        // kernel rejects everyone else with NOT_FIRST_ATTACKER), and all
        // cards must share a value.
        if (selfSeat(game) !== game.first_attacker) return false;
        return cards.every(card => card.value === cards[0].value);
    } else {
        // Subsequent attack: all card values must already be on the table
        const tableValues = new Set(game.table_battles.flatMap(battle =>
            [battle.attack.value, ...(battle.defense ? [battle.defense.value] : [])]
        ));

        return cards.every(card => tableValues.has(card.value));
    }
};

export const canPass = (game: PersonalGame, cards: Card[]): boolean => {
    if (cards.length === 0 || game.table_battles.length === 0) return false;

    // All selected cards must have the same value
    if (!cards.every(card => card.value === cards[0].value)) {
        return false;
    }

    // All table battles must be uncovered and have the same value as the selected cards
    const allUncoveredWithSameValue = game.table_battles.every(battle =>
        battle.defense === null && battle.attack.value === cards[0].value
    );

    if (!allUncoveredWithSameValue) {
        return false;
    }

    // Find the next player (clockwise from defender). Must skip ELIMINATED
    // players exactly like the server (get_next_player_index) and the bot's
    // legal-move enumeration do — otherwise, when the seat immediately after the
    // defender is out, this looks at an out player's empty hand and wrongly hides
    // a pass the server would accept.
    const nextPlayerIndex = get_next_player_index(game, game.defender);
    const nextPlayerHandSize = game.players[nextPlayerIndex]?.hand_length || 0;

    // Calculate total cards that would be passed (only uncovered battles get passed)
    // Note: The check above already ensures all battles are uncovered for passing,
    // but we count explicitly for clarity and future-proofing
    const uncoveredBattles = game.table_battles.filter(battle => !battle.defense).length;
    const totalCardsAfterPass = uncoveredBattles + cards.length;

    // Cannot pass if next player doesn't have enough cards to defend
    return totalCardsAfterPass <= nextPlayerHandSize;
};

// The seat that BECOMES the defender after the current defender passes / the bout
// ends. Must skip ELIMINATED players exactly like the server and bot do — a naive
// `(defender + 1) % players.length` lands on an out seat when the next seat is
// eliminated, which diverges from the authoritative rotation. Used by the
// optimistic pass animation so its predicted defender matches the server's.
export const nextDefenderIndex = (game: PersonalGame): number =>
    get_next_player_index(game, game.defender);

// True when the selection covers uncovered attacks in exactly one
// unambiguous way. Delegates to the ONE shared cover-mapping resolver
// (coverCombinations.ts) — this used to be one of three hand-rolled copies.
export const canCoverCards = (game: PersonalGame, selectedCards: Card[]): boolean => {
    if (selectedCards.length === 0) return false;
    return findUnambiguousCover(selectedCards, game.table_battles, game.power_suit) !== null;
};

// Throwing validation functions that gate the OPTIMISTIC animation (the
// request is sent regardless; the kernel is authoritative). Each mirrors
// its handle_* counterpart in cnitro/src/game.c so a move the kernel will
// reject never animates optimistically.
export const validateAttack = (game: PersonalGame, cards: Card[]): void => {
    const table_battles = game.table_battles;
    const uncovered_cards = table_battles.filter(battle => battle.defense === null).length;
    const defender: PublicPlayer = game.players[game.defender];
    const defender_cards = defender.hand_length;

    if (cards.length === 0) {
        throw new Error('No cards provided');
    }

    if (uncovered_cards + cards.length > defender_cards) {
        throw new Error('No room in defenders hand');
    }

    if (table_battles.length === 0) {
        // handle_attack: only the first attacker may open a bout, with
        // same-value cards
        if (selfSeat(game) !== game.first_attacker) {
            throw new Error('Not the first attacker');
        }
        if (!cards.every(card => card.value === cards[0].value)) {
            throw new Error('First attack cards must share a value');
        }
    } else if (!cards.every(card => table_battles.some(battle => battle.attack.value === card.value || battle.defense?.value === card.value))) {
        throw new Error('Some card values are not on the table');
    }
};

export const validatePass = (game: PersonalGame, cards: Card[]): void => {
    const table_battles = game.table_battles;

    if (cards.length === 0 || table_battles.length === 0) {
        throw new Error('Cannot pass');
    }

    if (!cards.every(card => card.value === cards[0].value)) {
        throw new Error('Some card values are not the same');
    }

    if (!table_battles.every(battle => battle.defense === null && battle.attack.value === cards[0].value)) {
        throw new Error('Cannot pass');
    }

    // handle_pass PASS_CAPACITY: the next defender must be able to cover
    // every table card plus the passed ones
    const nextDefender = get_next_player_index(game, game.defender);
    const nextDefenderHandSize = game.players[nextDefender]?.hand_length || 0;
    if (table_battles.length + cards.length > nextDefenderHandSize) {
        throw new Error('Next defender cannot cover the pass');
    }
};

export const validatePickup = (game: PersonalGame): void => {
    const table_battles = game.table_battles;

    if (table_battles.length === 0) {
        throw new Error('Cannot pickup');
    }
};

export const validateCover = (game: PersonalGame, coverCards: Card[], attackCards: Card[]): void => {
    const table_battles = game.table_battles;

    if (coverCards.length === 0 || coverCards.length !== attackCards.length) {
        throw new Error('Cannot cover');
    }

    if (table_battles.length === 0) {
        throw new Error('Cannot cover');
    }

    // handle_cover: each target must be an UNCOVERED attack on the table,
    // matched by exact card, and no target may be named twice (the defender
    // double-tap gap — the kernel rejects it; without this mirror a second
    // cover of the same attack animated optimistically inside the
    // ANIMATION_TIME window and then flickered back).
    const usedTargets = new Set<string>();
    for (let i = 0; i < coverCards.length; i++) {
        const coverCard = coverCards[i];
        const attackCard = attackCards[i];
        const targetKey = `${attackCard.suit}-${attackCard.value}`;
        if (usedTargets.has(targetKey)) {
            throw new Error('Attack card targeted twice');
        }
        usedTargets.add(targetKey);
        const onTableUncovered = table_battles.some(battle =>
            battle.defense === null &&
            battle.attack.suit === attackCard.suit &&
            battle.attack.value === attackCard.value
        );
        if (!onTableUncovered) {
            throw new Error('Attack card is not uncovered on the table');
        }
        if (!canCover(attackCard, coverCard, game.power_suit)) {
            throw new Error('Cover card value does not match attack card value');
        }
    }
};
