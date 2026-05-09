import { Card, Game } from '../types.ts';
import { BotStrategy, LegalMove } from '../bot_interfaces.ts';

// Nitro — built from scratch, starting from a trivial baseline and growing
// principle by principle. Every change is validated against the full
// previously-passing range before advancing the frontier.

// Card value used everywhere we need to pick "the cheapest card to spend".
// Trumps are heavily penalized so we don't fritter them on small exchanges.
const cardCost = (card: Card, powerSuit: number): number =>
    card.suit === powerSuit ? 100 + card.value : card.value;

const moveCost = (move: LegalMove, powerSuit: number): number => {
    if (!move.cards || move.cards.length === 0) return 0;
    let s = 0;
    for (const c of move.cards) s += cardCost(c, powerSuit);
    return s;
};

export class NitroStrategy implements BotStrategy {
    readonly name = 'nitro';

    async chooseMove(game: Game, _botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        const trump = game.power_suit;

        // Principle 1 (defender): Prefer covering an attack over picking it up.
        //   Why: pickup adds cards to your hand (worse position); covering
        //   moves cards toward discard. When multiple covers are legal,
        //   spend the cheapest cards (lowest non-trumps first).
        //   When this should NOT apply: if every legal cover uses a trump
        //   that would be more valuable kept, pickup may be better — handled
        //   in a later iteration once we have evidence it matters.
        const coverMoves = legalMoves.filter(m => m.type === 'cover');
        if (coverMoves.length > 0) {
            return coverMoves.reduce((best, m) => moveCost(m, trump) < moveCost(best, trump) ? m : best);
        }

        // Principle 2 (attacker): Prefer attacking over saying "good".
        //   Why: every attack a defender survives costs them resources;
        //   refusing free attacks just lets the round end early.
        //   Attack with the cheapest legal cards; trumps are last resort.
        //   Counter-condition: late game we may want to *not* attack to
        //   avoid arming the defender's pickup — handled later.
        const attackMoves = legalMoves.filter(m => m.type === 'attack');
        if (attackMoves.length > 0) {
            return attackMoves.reduce((best, m) => moveCost(m, trump) < moveCost(best, trump) ? m : best);
        }

        // If we're defender and cannot cover, prefer pass over pickup.
        //   Why: passing forwards the burden to the next player (in 1v1, the
        //   opponent becomes defender). Pickup grows our hand.
        const passMoves = legalMoves.filter(m => m.type === 'pass');
        if (passMoves.length > 0) {
            return passMoves.reduce((best, m) => moveCost(m, trump) < moveCost(best, trump) ? m : best);
        }

        const goodMoves = legalMoves.filter(m => m.type === 'good');
        if (goodMoves.length > 0) return goodMoves[0];

        const pickupMoves = legalMoves.filter(m => m.type === 'pickup');
        if (pickupMoves.length > 0) return pickupMoves[0];

        return legalMoves[legalMoves.length - 1];
    }
}
