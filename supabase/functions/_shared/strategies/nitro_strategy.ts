import { Card, Game, PrivatePlayer, PLAYER_STATUS } from '../types.ts';
import { BotStrategy, LegalMove } from '../bot_interfaces.ts';
import { canCover } from '../common_utils.ts';

// Nitro — built from scratch, growing one principle at a time. Every change
// is validated against the full previously-passing range before advancing
// the frontier.

// ---- card-level costs ----------------------------------------------------

// Cost we want to MINIMIZE when committing cards in different contexts.
// Attacks / passes — the card is being thrown into the round. Low-value
// non-trumps are cheapest; trumps get a heavy penalty so we don't fritter
// them on small exchanges.
const offensiveCardCost = (card: Card, powerSuit: number): number =>
    card.suit === powerSuit ? 100 + card.value : card.value;

// Cover cost: prefer the TIGHTEST card. Same-suit minimum-gap, else
// lowest trump.
const coverPairCost = (attack: Card, defense: Card, powerSuit: number): number => {
    if (defense.suit === powerSuit && attack.suit !== powerSuit) {
        return 100 + defense.value;
    }
    return defense.value - attack.value;
};

const offensiveMoveCost = (move: LegalMove, powerSuit: number): number => {
    if (!move.cards || move.cards.length === 0) return 0;
    let s = 0;
    for (const c of move.cards) s += offensiveCardCost(c, powerSuit);
    return s;
};

const coverMoveCost = (move: LegalMove, powerSuit: number): number => {
    if (!move.cards || move.cards.length === 0) return 0;
    if (!move.attack_cards || move.attack_cards.length !== move.cards.length) {
        return offensiveMoveCost(move, powerSuit);
    }
    let s = 0;
    for (let i = 0; i < move.cards.length; i++) {
        s += coverPairCost(move.attack_cards[i], move.cards[i], powerSuit);
    }
    return s;
};

const cheapest = <T>(items: T[], cost: (t: T) => number): T =>
    items.reduce((best, m) => cost(m) < cost(best) ? m : best);

// Greedy check: can `hand` cover EVERY attack in `attacks`? Cover with
// cheapest available card per attack (lowest same-suit higher, else
// lowest trump). No backtracking — good enough to tell pickup-vs-cover.
const canFullyCover = (attacks: Card[], hand: Card[], powerSuit: number): boolean => {
    const remaining = [...hand];
    for (const a of attacks) {
        let bestIdx = -1;
        let bestCost = Infinity;
        for (let i = 0; i < remaining.length; i++) {
            const c = remaining[i];
            if (!canCover(a, c, powerSuit)) continue;
            const cost = c.suit === powerSuit && a.suit !== powerSuit
                ? 100 + c.value
                : c.value - a.value;
            if (cost < bestCost) {
                bestCost = cost;
                bestIdx = i;
            }
        }
        if (bestIdx === -1) return false;
        remaining.splice(bestIdx, 1);
    }
    return true;
};

const findOpponent = (game: Game, botPlayerId: string): PrivatePlayer | null => {
    for (const p of game.players) {
        if (p.player_id !== botPlayerId && p.status === PLAYER_STATUS.IN) return p;
    }
    return null;
};

const trumpCount = (hand: Card[], powerSuit: number): number => {
    let c = 0;
    for (const card of hand) if (card.suit === powerSuit) c++;
    return c;
};

// ---- decision logic ------------------------------------------------------

export class NitroStrategy implements BotStrategy {
    readonly name = 'nitro';

    async chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        const trump = game.power_suit;
        const opp = findOpponent(game, botPlayerId);
        const me = game.players.find(p => p.player_id === botPlayerId)!;

        // Game-progression variables. The same move can be right or wrong
        // depending on these, so the rules below branch on them.
        const deckLeft = game.deck.length + (game.flipped ? 1 : 0);
        const deckEmpty = deckLeft === 0;
        const myTrumps = trumpCount(me.hand, trump);
        const oppTrumps = opp ? trumpCount(opp.hand, trump) : 0;
        const myHandSize = me.hand.length;
        const oppHandSize = opp ? opp.hand.length : 0;

        // ---------- DEFENDER ----------
        const coverMoves = legalMoves.filter(m => m.type === 'cover');
        if (coverMoves.length > 0) {
            // Principle 1: cover beats pickup.
            // Principle 3: pick the TIGHTEST cover (cover-cost function).
            //
            // Principle 9 (defender, perfect info): among similarly-priced
            // covers, prefer cards whose VALUE the opponent does NOT hold.
            //   Why: covering with a card adds that value to the table.
            //   The attacker can immediately follow up with any matching-
            //   value card from their hand. If their followups include a
            //   trump or another card we can't cover, our defense
            //   collapses on the next turn. Using a value the opponent
            //   doesn't hold defuses that vector.
            //   Counter (no opponent visible — N>2 / not 1v1): the
            //   findOpponent helper returns the first IN opponent. In
            //   1v1 that's the unique opponent. In 3+ player games it's
            //   one of several — the penalty still gives a noisy but
            //   useful signal, but more sophisticated multi-opponent
            //   reasoning is out of scope for this iteration.
            const oppMatchPenalty = (move: LegalMove): number => {
                if (!opp || !move.cards) return 0;
                let p = 0;
                for (const c of move.cards) {
                    for (const oc of opp.hand) {
                        if (oc.value === c.value) p++;
                    }
                }
                return 30 * p;
            };
            return cheapest(
                coverMoves,
                m => coverMoveCost(m, trump) + oppMatchPenalty(m),
            );
        }

        // ---------- ATTACKER ----------
        const attackMoves = legalMoves.filter(m => m.type === 'attack');
        const goodMove = legalMoves.find(m => m.type === 'good');

        if (attackMoves.length > 0) {
            // Principle 6 (attacker, perfect-info, ENDGAME only): prefer
            // attacks the defender CANNOT fully cover. Restrict to non-trump
            // forcings — pushing a trump into the defender's hand on pickup
            // arms them with a card we can't beat.
            //   Why: while the deck still feeds both players, a forced
            //   pickup just refills both back to 6 — gain is small. Once
            //   the deck is empty, the defender's hand is permanently
            //   bloated by what we push. That's the win condition.
            //   Counter: if no non-trump forcing exists, fall back below.
            if (deckEmpty && opp) {
                const uncoveredOnTable = game.table_battles
                    .filter(b => b.defense === null).map(b => b.attack);
                const forcing = attackMoves.filter(m => {
                    const cards = m.cards ?? [];
                    if (cards.some(c => c.suit === trump)) return false;
                    const all = [...uncoveredOnTable, ...cards];
                    return !canFullyCover(all, opp.hand, trump);
                });
                if (forcing.length > 0) {
                    return cheapest(forcing, m => offensiveMoveCost(m, trump));
                }
            }

            return cheapest(attackMoves, m => offensiveMoveCost(m, trump));
        }

        const passMoves = legalMoves.filter(m => m.type === 'pass');
        if (passMoves.length > 0) {
            return cheapest(passMoves, m => offensiveMoveCost(m, trump));
        }

        if (goodMove) return goodMove;

        const pickupMove = legalMoves.find(m => m.type === 'pickup');
        if (pickupMove) return pickupMove;

        // Suppress unused-variable warnings until a future iteration uses them.
        void deckLeft; void myHandSize; void oppHandSize;
        return legalMoves[legalMoves.length - 1];
    }
}
