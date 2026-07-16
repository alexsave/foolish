import { Game, PrivatePlayer } from '@api/core/types.ts';

// Reorder the player's own hand to the given index order. Mutates player.hand.
//
// SECURITY: card_indices must be a PERMUTATION of [0..n) — same length, every
// index in range, AND each index used exactly once. The uniqueness check is the
// load-bearing one: without it a client could POST e.g. [0,0,0,0,0,0] and the
// reorder below would produce six copies of one card (dropping the other five) —
// minting duplicate cards into their hand and corrupting the 36-card deck (the
// duplicates persist through commit_game). With it, a rearrange can only permute
// the hand it already holds.
export function handleRearrangeHand(game: Game, player_id: string, card_indices: unknown): void {
    if (!Array.isArray(card_indices)) {
        throw new Error('Missing required field: card_indices');
    }

    const player: PrivatePlayer | undefined = game.players.find((p) => p.player_id === player_id);
    if (!player) {
        throw new Error('You are not in this game');
    }

    const n = player.hand.length;
    if (card_indices.length !== n ||
        !card_indices.every((idx: unknown) => Number.isInteger(idx) && (idx as number) >= 0 && (idx as number) < n)) {
        throw new Error('Invalid card indices');
    }

    // Must be a permutation — each index exactly once — or the reorder duplicates
    // / drops cards.
    if (new Set(card_indices).size !== n) {
        throw new Error('Invalid card indices');
    }

    player.hand = (card_indices as number[]).map((index) => player.hand[index]);
}
